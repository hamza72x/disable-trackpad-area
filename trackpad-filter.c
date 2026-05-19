/*
 * trackpad-filter.c
 * Grabs Apple SPI Trackpad, filters touches that originate in dead zones,
 * forwards remaining events via uinput virtual device.
 *
 * Per-slot filtering: BLOCKED slots are made invisible to downstream.
 * MT_TRACKING_ID for blocked slots never emitted. BTN_TOUCH and
 * BTN_TOOL_* are regenerated from live-slot count so downstream
 * sees consistent state. BTN_LEFT and other globals pass through.
 *
 * Decision sticky: finger that starts in active zone stays LIVE even
 * if it slides into dead zone.
 *
 * Usage: trackpad-filter [--left PCT] [--right PCT] [--top PCT]
 * Defaults: --left 20 --right 20 --top 25
 *
 * Build: gcc -O2 -o trackpad-filter trackpad-filter.c
 * Run:   sudo ./trackpad-filter
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>

#define MAX_SLOTS 16
#define READ_BUF_EVENTS 64
#define OUT_BUF_EVENTS 512

static volatile sig_atomic_t running = 1;

static void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

struct trackpad_axes {
    int x_min, x_max;
    int y_min, y_max;
};

struct dead_zones {
    int left_pct, right_pct, top_pct;
    int x_left, x_right, y_top;
};

enum slot_state { SLOT_INACTIVE = 0, SLOT_PENDING, SLOT_LIVE, SLOT_BLOCKED };

struct slot_info {
    enum slot_state state;
    int tracking_id;   /* id from device, valid when state != INACTIVE */
    int x, y;
    int has_x, has_y;
    int emitted;       /* did we send MT_TRACKING_ID +id to uinput */
};

/* Global output state */
static struct slot_info slots[MAX_SLOTS];
static int in_slot = 0;          /* MT_SLOT from source */
static int out_slot = -1;        /* last MT_SLOT we emitted */
static struct input_event out_buf[OUT_BUF_EVENTS];
static int out_count = 0;

/* Regenerated key state we last sent downstream */
static int prev_btn_touch = 0;
static int prev_tool[6] = {0}; /* index 1..5 = N-tap state */

static int uinput_fd = -1;

static int find_trackpad(char *path, size_t pathlen) {
    char name[256];
    char devpath[64];
    int fd;
    for (int i = 0; i < 32; i++) {
        snprintf(devpath, sizeof(devpath), "/dev/input/event%d", i);
        fd = open(devpath, O_RDONLY);
        if (fd < 0) continue;
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0) {
            if (strstr(name, "Apple SPI Trackpad") ||
                strstr(name, "apple-spi-trackpad")) {
                close(fd);
                snprintf(path, pathlen, "%s", devpath);
                return 0;
            }
        }
        close(fd);
    }
    return -1;
}

static int get_axes(int fd, struct trackpad_axes *axes) {
    struct input_absinfo a;
    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &a) < 0) {
        if (ioctl(fd, EVIOCGABS(ABS_X), &a) < 0) {
            perror("get X axis"); return -1;
        }
    }
    axes->x_min = a.minimum; axes->x_max = a.maximum;
    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &a) < 0) {
        if (ioctl(fd, EVIOCGABS(ABS_Y), &a) < 0) {
            perror("get Y axis"); return -1;
        }
    }
    axes->y_min = a.minimum; axes->y_max = a.maximum;
    return 0;
}

static void compute_boundaries(struct dead_zones *dz,
                               const struct trackpad_axes *axes) {
    int xr = axes->x_max - axes->x_min;
    int yr = axes->y_max - axes->y_min;
    dz->x_left  = axes->x_min + xr * dz->left_pct  / 100;
    dz->x_right = axes->x_max - xr * dz->right_pct / 100;
    dz->y_top   = axes->y_min + yr * dz->top_pct   / 100;
}

static inline int is_in_dead_zone(const struct dead_zones *dz, int x, int y) {
    if (x < dz->x_left || x > dz->x_right) return 1;
    if (y < dz->y_top) return 1;
    return 0;
}

static int setup_uinput(int source_fd) {
    int ufd;
    struct uinput_setup usetup;
    struct input_absinfo absinfo;
    unsigned long evbits[8] = {0};
    unsigned long keybits[(KEY_MAX + 8*sizeof(long) - 1) / (8*sizeof(long))];
    unsigned long absbits[(ABS_MAX + 8*sizeof(long) - 1) / (8*sizeof(long))];
    unsigned long mscbits[(MSC_MAX + 8*sizeof(long) - 1) / (8*sizeof(long))];
    unsigned long propbits[(INPUT_PROP_MAX + 8*sizeof(long) - 1) / (8*sizeof(long))];
    memset(keybits, 0, sizeof(keybits));
    memset(absbits, 0, sizeof(absbits));
    memset(mscbits, 0, sizeof(mscbits));
    memset(propbits, 0, sizeof(propbits));

    ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (ufd < 0) { perror("open /dev/uinput"); return -1; }

    if (ioctl(source_fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) {
        perror("EVIOCGBIT EV"); close(ufd); return -1;
    }
    for (int i = 0; i < EV_MAX; i++) {
        if (evbits[i / (8*sizeof(long))] & (1UL << (i % (8*sizeof(long)))))
            ioctl(ufd, UI_SET_EVBIT, i);
    }
    if (ioctl(source_fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) >= 0) {
        for (int i = 0; i < KEY_MAX; i++) {
            if (keybits[i / (8*sizeof(long))] & (1UL << (i % (8*sizeof(long)))))
                ioctl(ufd, UI_SET_KEYBIT, i);
        }
    }
    if (ioctl(source_fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) >= 0) {
        for (int i = 0; i < ABS_MAX; i++) {
            if (absbits[i / (8*sizeof(long))] & (1UL << (i % (8*sizeof(long))))) {
                ioctl(ufd, UI_SET_ABSBIT, i);
                if (ioctl(source_fd, EVIOCGABS(i), &absinfo) >= 0) {
                    struct uinput_abs_setup as;
                    memset(&as, 0, sizeof(as));
                    as.code = i; as.absinfo = absinfo;
                    ioctl(ufd, UI_ABS_SETUP, &as);
                }
            }
        }
    }
    if (ioctl(source_fd, EVIOCGBIT(EV_MSC, sizeof(mscbits)), mscbits) >= 0) {
        for (int i = 0; i < MSC_MAX; i++) {
            if (mscbits[i / (8*sizeof(long))] & (1UL << (i % (8*sizeof(long)))))
                ioctl(ufd, UI_SET_MSCBIT, i);
        }
    }
    if (ioctl(source_fd, EVIOCGPROP(sizeof(propbits)), propbits) >= 0) {
        for (int i = 0; i < INPUT_PROP_MAX; i++) {
            if (propbits[i / (8*sizeof(long))] & (1UL << (i % (8*sizeof(long)))))
                ioctl(ufd, UI_SET_PROPBIT, i);
        }
    }

    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_VIRTUAL;
    usetup.id.vendor  = 0x05ac;
    usetup.id.product = 0x0343;
    usetup.id.version = 1;
    snprintf(usetup.name, UINPUT_MAX_NAME_SIZE, "Filtered Trackpad");

    if (ioctl(ufd, UI_DEV_SETUP, &usetup) < 0) {
        perror("UI_DEV_SETUP"); close(ufd); return -1;
    }
    if (ioctl(ufd, UI_DEV_CREATE) < 0) {
        perror("UI_DEV_CREATE"); close(ufd); return -1;
    }
    usleep(200000);
    return ufd;
}

/* ---------- Output emission ---------- */

static void out_push(int type, int code, int value) {
    if (out_count >= OUT_BUF_EVENTS) {
        /* Flush early to avoid drop */
        ssize_t n = write(uinput_fd, out_buf,
                          out_count * (ssize_t)sizeof(struct input_event));
        (void)n;
        out_count = 0;
    }
    out_buf[out_count].time.tv_sec = 0;
    out_buf[out_count].time.tv_usec = 0;
    out_buf[out_count].type = type;
    out_buf[out_count].code = code;
    out_buf[out_count].value = value;
    out_count++;
}

static void ensure_out_slot(int s) {
    if (out_slot != s) {
        out_push(EV_ABS, ABS_MT_SLOT, s);
        out_slot = s;
    }
}

/* Emit MT_TRACKING_ID +id for slot s if not yet emitted */
static void ensure_emitted(int s) {
    if (!slots[s].emitted) {
        ensure_out_slot(s);
        out_push(EV_ABS, ABS_MT_TRACKING_ID, slots[s].tracking_id);
        slots[s].emitted = 1;
    }
}

/* Try to resolve PENDING slot, and flush its position to output if LIVE */
static void decide_and_flush(int s, const struct dead_zones *dz) {
    if (slots[s].state != SLOT_PENDING) return;
    if (!(slots[s].has_x && slots[s].has_y)) return;
    if (is_in_dead_zone(dz, slots[s].x, slots[s].y)) {
        slots[s].state = SLOT_BLOCKED;
    } else {
        slots[s].state = SLOT_LIVE;
        ensure_emitted(s);
        ensure_out_slot(s);
        out_push(EV_ABS, ABS_MT_POSITION_X, slots[s].x);
        out_push(EV_ABS, ABS_MT_POSITION_Y, slots[s].y);
    }
}

static int count_live_slots(void) {
    int c = 0;
    for (int i = 0; i < MAX_SLOTS; i++)
        if (slots[i].state == SLOT_LIVE) c++;
    return c;
}

/* Regenerate BTN_TOUCH / BTN_TOOL_* based on actual live count */
static void regenerate_btn_state(void) {
    int live = count_live_slots();
    int touch = (live > 0) ? 1 : 0;
    if (touch != prev_btn_touch) {
        out_push(EV_KEY, BTN_TOUCH, touch);
        prev_btn_touch = touch;
    }
    int tool_codes[6] = { 0, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP,
                          BTN_TOOL_TRIPLETAP, BTN_TOOL_QUADTAP,
                          BTN_TOOL_QUINTTAP };
    for (int n = 1; n <= 5; n++) {
        int want = (live == n) ? 1 : 0;
        if (want != prev_tool[n]) {
            out_push(EV_KEY, tool_codes[n], want);
            prev_tool[n] = want;
        }
    }
}

/* ---------- Main event handler ---------- */

static int handle_event(const struct input_event *ev,
                        const struct dead_zones *dz) {
    if (ev->type == EV_SYN) {
        if (ev->code == SYN_REPORT) {
            regenerate_btn_state();
            if (out_count > 0) {
                out_push(EV_SYN, SYN_REPORT, 0);
                ssize_t n = write(uinput_fd, out_buf,
                                  out_count * (ssize_t)sizeof(struct input_event));
                (void)n;
                out_count = 0;
            }
        } else {
            /* SYN_DROPPED etc — pass through if anything in flight */
            if (out_count > 0)
                out_push(ev->type, ev->code, ev->value);
        }
        return 0;
    }

    if (ev->type == EV_ABS) {
        switch (ev->code) {
        case ABS_MT_SLOT: {
            int s = ev->value;
            if (s < 0 || s >= MAX_SLOTS) {
                /* Drop frame's slot context but don't corrupt */
                in_slot = -1;
            } else {
                in_slot = s;
            }
            return 0;
        }
        case ABS_MT_TRACKING_ID: {
            if (in_slot < 0) return 0;
            if (ev->value == -1) {
                if (slots[in_slot].emitted) {
                    ensure_out_slot(in_slot);
                    out_push(EV_ABS, ABS_MT_TRACKING_ID, -1);
                }
                memset(&slots[in_slot], 0, sizeof(slots[in_slot]));
                slots[in_slot].state = SLOT_INACTIVE;
            } else {
                /* New touch on this slot */
                memset(&slots[in_slot], 0, sizeof(slots[in_slot]));
                slots[in_slot].state = SLOT_PENDING;
                slots[in_slot].tracking_id = ev->value;
            }
            return 0;
        }
        case ABS_MT_POSITION_X:
            if (in_slot < 0) return 0;
            slots[in_slot].x = ev->value;
            slots[in_slot].has_x = 1;
            if (slots[in_slot].state == SLOT_PENDING) {
                decide_and_flush(in_slot, dz);
            } else if (slots[in_slot].state == SLOT_LIVE) {
                ensure_emitted(in_slot);
                ensure_out_slot(in_slot);
                out_push(EV_ABS, ev->code, ev->value);
            }
            return 0;
        case ABS_MT_POSITION_Y:
            if (in_slot < 0) return 0;
            slots[in_slot].y = ev->value;
            slots[in_slot].has_y = 1;
            if (slots[in_slot].state == SLOT_PENDING) {
                decide_and_flush(in_slot, dz);
            } else if (slots[in_slot].state == SLOT_LIVE) {
                ensure_emitted(in_slot);
                ensure_out_slot(in_slot);
                out_push(EV_ABS, ev->code, ev->value);
            }
            return 0;
        case ABS_MT_TOUCH_MAJOR:
        case ABS_MT_TOUCH_MINOR:
        case ABS_MT_WIDTH_MAJOR:
        case ABS_MT_WIDTH_MINOR:
        case ABS_MT_ORIENTATION:
        case ABS_MT_PRESSURE:
        case ABS_MT_DISTANCE:
            if (in_slot < 0) return 0;
            if (slots[in_slot].state == SLOT_LIVE && slots[in_slot].emitted) {
                ensure_out_slot(in_slot);
                out_push(EV_ABS, ev->code, ev->value);
            }
            return 0;
        default:
            /* Globals: ABS_X, ABS_Y, ABS_PRESSURE, etc. Pass through. */
            out_push(ev->type, ev->code, ev->value);
            return 0;
        }
    }

    if (ev->type == EV_KEY) {
        switch (ev->code) {
        case BTN_TOUCH:
        case BTN_TOOL_FINGER:
        case BTN_TOOL_DOUBLETAP:
        case BTN_TOOL_TRIPLETAP:
        case BTN_TOOL_QUADTAP:
        case BTN_TOOL_QUINTTAP:
            /* Regenerated from live count at SYN */
            return 0;
        default:
            /* BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, etc. Pass through. */
            out_push(ev->type, ev->code, ev->value);
            return 0;
        }
    }

    /* EV_MSC and others pass through */
    out_push(ev->type, ev->code, ev->value);
    return 0;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n\n"
        "Options:\n"
        "  --left   PCT   Left dead zone percentage   (default: 20)\n"
        "  --right  PCT   Right dead zone percentage  (default: 20)\n"
        "  --top    PCT   Top dead zone percentage    (default: 25)\n"
        "  --help         Show this help\n\n"
        "Bottom is never disabled.\n",
        prog);
}

int main(int argc, char **argv) {
    char devpath[64];
    int src_fd;
    struct trackpad_axes axes;
    struct dead_zones dz = { .left_pct = 20, .right_pct = 20, .top_pct = 25 };

    memset(slots, 0, sizeof(slots));

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            print_usage(argv[0]); return 0;
        } else if (!strcmp(argv[i], "--left") && i + 1 < argc) {
            dz.left_pct = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--right") && i + 1 < argc) {
            dz.right_pct = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--top") && i + 1 < argc) {
            dz.top_pct = atoi(argv[++i]);
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]); return 1;
        }
    }

    if (dz.left_pct < 0 || dz.right_pct < 0 || dz.top_pct < 0) {
        fprintf(stderr, "Error: negative percentages\n"); return 1;
    }
    if (dz.left_pct + dz.right_pct >= 100 || dz.top_pct >= 100) {
        fprintf(stderr, "Error: dead zones exceed 100%%\n"); return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (find_trackpad(devpath, sizeof(devpath)) < 0) {
        fprintf(stderr, "Error: cannot find Apple SPI Trackpad\n"); return 1;
    }
    src_fd = open(devpath, O_RDONLY);
    if (src_fd < 0) { perror("open trackpad"); return 1; }
    if (get_axes(src_fd, &axes) < 0) { close(src_fd); return 1; }

    compute_boundaries(&dz, &axes);

    printf("Trackpad: %s\n", devpath);
    printf("Axes: X[%d, %d] Y[%d, %d]\n",
           axes.x_min, axes.x_max, axes.y_min, axes.y_max);
    printf("Dead zones: left=%d%% right=%d%% top=%d%%\n",
           dz.left_pct, dz.right_pct, dz.top_pct);
    printf("Active area: X[%d, %d] Y[%d, %d]\n",
           dz.x_left, dz.x_right, dz.y_top, axes.y_max);
    fflush(stdout);

    uinput_fd = setup_uinput(src_fd);
    if (uinput_fd < 0) { close(src_fd); return 1; }

    if (ioctl(src_fd, EVIOCGRAB, 1) < 0) {
        perror("EVIOCGRAB");
        close(src_fd);
        ioctl(uinput_fd, UI_DEV_DESTROY);
        close(uinput_fd);
        return 1;
    }

    printf("Filter active. Send SIGTERM to stop.\n");
    fflush(stdout);

    struct input_event read_buf[READ_BUF_EVENTS];

    while (running) {
        ssize_t bytes = read(src_fd, read_buf, sizeof(read_buf));
        if (bytes < 0) {
            if (errno == EINTR) continue;
            break;
        }
        int n = bytes / (int)sizeof(struct input_event);
        for (int i = 0; i < n; i++)
            handle_event(&read_buf[i], &dz);
    }

    printf("\nStopping trackpad filter...\n");
    ioctl(src_fd, EVIOCGRAB, 0);
    close(src_fd);
    ioctl(uinput_fd, UI_DEV_DESTROY);
    close(uinput_fd);
    return 0;
}
