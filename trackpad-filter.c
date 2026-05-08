/*
 * trackpad-filter.c
 * Grabs Apple SPI Trackpad, filters touches that originate in dead zones,
 * forwards remaining events via uinput virtual device.
 *
 * Dead zone decision is made ONCE when finger first touches. If a finger
 * starts in the active zone, it stays active even if it slides into a
 * dead zone area. This prevents mid-gesture drops.
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
#define BATCH_MAX_EVENTS 128

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
    int left_pct;
    int right_pct;
    int top_pct;
    /* Computed boundaries */
    int x_left;   /* touches with x < x_left are dead */
    int x_right;  /* touches with x > x_right are dead */
    int y_top;    /* touches with y < y_top are dead (y_min = physical top) */
};

/*
 * Slot states:
 *   INACTIVE - no finger in this slot
 *   PENDING  - finger down, waiting for first X+Y to decide
 *   LIVE     - finger started in active zone, always forwarded
 *   BLOCKED  - finger started in dead zone, always dropped
 */
enum slot_state { SLOT_INACTIVE = 0, SLOT_PENDING, SLOT_LIVE, SLOT_BLOCKED };

struct slot_info {
    enum slot_state state;
    int x, y;
    int has_x, has_y;
};

/* Find trackpad event device */
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

/* Query trackpad axis ranges from device */
static int get_axes(int fd, struct trackpad_axes *axes) {
    struct input_absinfo absinfo;

    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &absinfo) < 0) {
        if (ioctl(fd, EVIOCGABS(ABS_X), &absinfo) < 0) {
            perror("Cannot get X axis info");
            return -1;
        }
    }
    axes->x_min = absinfo.minimum;
    axes->x_max = absinfo.maximum;

    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &absinfo) < 0) {
        if (ioctl(fd, EVIOCGABS(ABS_Y), &absinfo) < 0) {
            perror("Cannot get Y axis info");
            return -1;
        }
    }
    axes->y_min = absinfo.minimum;
    axes->y_max = absinfo.maximum;

    return 0;
}

/*
 * Apple SPI Trackpad Y axis: Y_min = top (near keyboard), Y_max = bottom (near user)
 * Confirmed by testing: blocking y < threshold blocks area near keyboard.
 */
static void compute_boundaries(struct dead_zones *dz, const struct trackpad_axes *axes) {
    int x_range = axes->x_max - axes->x_min;
    int y_range = axes->y_max - axes->y_min;

    dz->x_left  = axes->x_min + x_range * dz->left_pct / 100;
    dz->x_right = axes->x_max - x_range * dz->right_pct / 100;
    dz->y_top   = axes->y_min + y_range * dz->top_pct / 100;
}

static inline int is_in_dead_zone(const struct dead_zones *dz, int x, int y) {
    if (x < dz->x_left || x > dz->x_right)
        return 1;
    if (y < dz->y_top)
        return 1;
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
    if (ufd < 0) {
        perror("open /dev/uinput");
        return -1;
    }

    if (ioctl(source_fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) {
        perror("EVIOCGBIT EV");
        close(ufd);
        return -1;
    }

    for (int i = 0; i < EV_MAX; i++) {
        if (evbits[i / (8 * sizeof(long))] & (1UL << (i % (8 * sizeof(long))))) {
            ioctl(ufd, UI_SET_EVBIT, i);
        }
    }

    if (ioctl(source_fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) >= 0) {
        for (int i = 0; i < KEY_MAX; i++) {
            if (keybits[i / (8 * sizeof(long))] & (1UL << (i % (8 * sizeof(long))))) {
                ioctl(ufd, UI_SET_KEYBIT, i);
            }
        }
    }

    if (ioctl(source_fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) >= 0) {
        for (int i = 0; i < ABS_MAX; i++) {
            if (absbits[i / (8 * sizeof(long))] & (1UL << (i % (8 * sizeof(long))))) {
                ioctl(ufd, UI_SET_ABSBIT, i);
                if (ioctl(source_fd, EVIOCGABS(i), &absinfo) >= 0) {
                    struct uinput_abs_setup abs_setup;
                    memset(&abs_setup, 0, sizeof(abs_setup));
                    abs_setup.code = i;
                    abs_setup.absinfo = absinfo;
                    ioctl(ufd, UI_ABS_SETUP, &abs_setup);
                }
            }
        }
    }

    if (ioctl(source_fd, EVIOCGBIT(EV_MSC, sizeof(mscbits)), mscbits) >= 0) {
        for (int i = 0; i < MSC_MAX; i++) {
            if (mscbits[i / (8 * sizeof(long))] & (1UL << (i % (8 * sizeof(long))))) {
                ioctl(ufd, UI_SET_MSCBIT, i);
            }
        }
    }

    if (ioctl(source_fd, EVIOCGPROP(sizeof(propbits)), propbits) >= 0) {
        for (int i = 0; i < INPUT_PROP_MAX; i++) {
            if (propbits[i / (8 * sizeof(long))] & (1UL << (i % (8 * sizeof(long))))) {
                ioctl(ufd, UI_SET_PROPBIT, i);
            }
        }
    }

    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_VIRTUAL;
    usetup.id.vendor  = 0x05ac;
    usetup.id.product = 0x0343;
    usetup.id.version = 1;
    snprintf(usetup.name, UINPUT_MAX_NAME_SIZE, "Filtered Trackpad");

    if (ioctl(ufd, UI_DEV_SETUP, &usetup) < 0) {
        perror("UI_DEV_SETUP");
        close(ufd);
        return -1;
    }

    if (ioctl(ufd, UI_DEV_CREATE) < 0) {
        perror("UI_DEV_CREATE");
        close(ufd);
        return -1;
    }

    usleep(200000);
    return ufd;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  --left   PCT   Left dead zone percentage   (default: 20)\n"
        "  --right  PCT   Right dead zone percentage  (default: 20)\n"
        "  --top    PCT   Top dead zone percentage    (default: 25)\n"
        "  --help         Show this help\n"
        "\n"
        "Bottom is never disabled.\n",
        prog);
}

int main(int argc, char **argv) {
    char devpath[64];
    int src_fd, uinput_fd;
    struct trackpad_axes axes;
    struct dead_zones dz = { .left_pct = 20, .right_pct = 20, .top_pct = 25 };

    struct slot_info slots[MAX_SLOTS];
    memset(slots, 0, sizeof(slots));
    int current_slot = 0;

    /* Single-touch tracking */
    int st_x = 0, st_y = 0;
    int st_has_x = 0, st_has_y = 0;

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--left") == 0 && i + 1 < argc) {
            dz.left_pct = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--right") == 0 && i + 1 < argc) {
            dz.right_pct = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--top") == 0 && i + 1 < argc) {
            dz.top_pct = atoi(argv[++i]);
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    /* Validate */
    if (dz.left_pct < 0 || dz.right_pct < 0 || dz.top_pct < 0) {
        fprintf(stderr, "Error: Percentages cannot be negative\n");
        return 1;
    }
    if (dz.left_pct + dz.right_pct >= 100 || dz.top_pct >= 100) {
        fprintf(stderr, "Error: Dead zones exceed 100%% of trackpad area\n");
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (find_trackpad(devpath, sizeof(devpath)) < 0) {
        fprintf(stderr, "Error: Cannot find Apple SPI Trackpad\n");
        return 1;
    }

    src_fd = open(devpath, O_RDONLY);
    if (src_fd < 0) {
        perror("open trackpad");
        return 1;
    }

    if (get_axes(src_fd, &axes) < 0) {
        close(src_fd);
        return 1;
    }

    compute_boundaries(&dz, &axes);

    printf("Trackpad: %s\n", devpath);
    printf("Axes: X[%d, %d] Y[%d, %d]\n", axes.x_min, axes.x_max, axes.y_min, axes.y_max);
    printf("Dead zones: left=%d%% right=%d%% top=%d%%\n",
           dz.left_pct, dz.right_pct, dz.top_pct);
    printf("Active area: X[%d, %d] Y[%d, %d]\n",
           dz.x_left, dz.x_right, dz.y_top, axes.y_max);
    fflush(stdout);

    uinput_fd = setup_uinput(src_fd);
    if (uinput_fd < 0) {
        close(src_fd);
        return 1;
    }

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
    struct input_event batch[BATCH_MAX_EVENTS];
    int batch_count = 0;
    int has_lift = 0;

    while (running) {
        ssize_t bytes = read(src_fd, read_buf, sizeof(read_buf));
        if (bytes < 0) {
            if (errno == EINTR) continue;
            break;
        }

        int nevents = bytes / (int)sizeof(struct input_event);

        for (int ei = 0; ei < nevents; ei++) {
            struct input_event *ev = &read_buf[ei];

            if (ev->type == EV_ABS) {
                switch (ev->code) {
                case ABS_MT_SLOT:
                    current_slot = ev->value;
                    if (current_slot < 0) current_slot = 0;
                    if (current_slot >= MAX_SLOTS) current_slot = MAX_SLOTS - 1;
                    break;

                case ABS_MT_TRACKING_ID:
                    if (ev->value == -1) {
                        /* Finger lifted */
                        slots[current_slot].state = SLOT_INACTIVE;
                        slots[current_slot].has_x = 0;
                        slots[current_slot].has_y = 0;
                        has_lift = 1;
                    } else {
                        /* New finger - pending until we know position */
                        slots[current_slot].state = SLOT_PENDING;
                        slots[current_slot].has_x = 0;
                        slots[current_slot].has_y = 0;
                    }
                    break;

                case ABS_MT_POSITION_X:
                    slots[current_slot].x = ev->value;
                    slots[current_slot].has_x = 1;
                    /* Only decide on first complete position */
                    if (slots[current_slot].state == SLOT_PENDING &&
                        slots[current_slot].has_y) {
                        slots[current_slot].state =
                            is_in_dead_zone(&dz, slots[current_slot].x,
                                            slots[current_slot].y)
                            ? SLOT_BLOCKED : SLOT_LIVE;
                    }
                    break;

                case ABS_MT_POSITION_Y:
                    slots[current_slot].y = ev->value;
                    slots[current_slot].has_y = 1;
                    if (slots[current_slot].state == SLOT_PENDING &&
                        slots[current_slot].has_x) {
                        slots[current_slot].state =
                            is_in_dead_zone(&dz, slots[current_slot].x,
                                            slots[current_slot].y)
                            ? SLOT_BLOCKED : SLOT_LIVE;
                    }
                    break;

                case ABS_X:
                    st_x = ev->value;
                    st_has_x = 1;
                    break;
                case ABS_Y:
                    st_y = ev->value;
                    st_has_y = 1;
                    break;
                }
            }

            if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
                int forward = 0;

                for (int i = 0; i < MAX_SLOTS; i++) {
                    if (slots[i].state == SLOT_LIVE ||
                        slots[i].state == SLOT_PENDING) {
                        forward = 1;
                        break;
                    }
                }

                /* Always forward lift events */
                if (has_lift)
                    forward = 1;

                /* Single-touch fallback */
                if (!forward && st_has_x && st_has_y) {
                    if (!is_in_dead_zone(&dz, st_x, st_y))
                        forward = 1;
                }

                if (forward && batch_count > 0) {
                    batch[batch_count++] = *ev;
                    write(uinput_fd, batch,
                          batch_count * (ssize_t)sizeof(struct input_event));
                } else if (forward) {
                    write(uinput_fd, ev, sizeof(*ev));
                }

                batch_count = 0;
                has_lift = 0;
                st_has_x = 0;
                st_has_y = 0;
            } else {
                if (batch_count < BATCH_MAX_EVENTS - 1)
                    batch[batch_count++] = *ev;
            }
        }
    }

    printf("\nStopping trackpad filter...\n");
    ioctl(src_fd, EVIOCGRAB, 0);
    close(src_fd);
    ioctl(uinput_fd, UI_DEV_DESTROY);
    close(uinput_fd);

    return 0;
}
