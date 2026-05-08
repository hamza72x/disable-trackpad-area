/*
 * trackpad-filter.c
 * Grabs Apple SPI Trackpad, filters touches in configurable dead zones,
 * forwards remaining events via uinput virtual device.
 *
 * Usage: trackpad-filter [--left PCT] [--right PCT] [--top PCT] [--bottom PCT]
 * Defaults: --left 15 --right 15 --top 20 --bottom 0
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

static volatile int running = 1;

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
    int bottom_pct;
    /* Computed boundaries */
    int x_left;
    int x_right;
    int y_top;
    int y_bottom;
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

static void compute_boundaries(struct dead_zones *dz, struct trackpad_axes *axes) {
    int x_range = axes->x_max - axes->x_min;
    int y_range = axes->y_max - axes->y_min;

    dz->x_left   = axes->x_min + x_range * dz->left_pct / 100;
    dz->x_right  = axes->x_max - x_range * dz->right_pct / 100;
    dz->y_top    = axes->y_min + y_range * dz->top_pct / 100;
    dz->y_bottom = axes->y_max - y_range * dz->bottom_pct / 100;
}

static int is_in_dead_zone(struct dead_zones *dz, int x, int y, int has_x, int has_y) {
    if (has_x) {
        if (x < dz->x_left || x > dz->x_right)
            return 1;
    }
    if (has_y) {
        if (y < dz->y_top || y > dz->y_bottom)
            return 1;
    }
    return 0;
}

static int setup_uinput(int source_fd) {
    int ufd;
    struct uinput_setup usetup;
    struct input_absinfo absinfo;
    unsigned long evbits[8] = {0};
    unsigned long keybits[16] = {0};
    unsigned long absbits[4] = {0};
    unsigned long mscbits[2] = {0};
    unsigned long propbits[2] = {0};

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
        "  --left   PCT   Left dead zone percentage   (default: 15)\n"
        "  --right  PCT   Right dead zone percentage  (default: 15)\n"
        "  --top    PCT   Top dead zone percentage    (default: 20)\n"
        "  --bottom PCT   Bottom dead zone percentage (default: 0)\n"
        "  --help         Show this help\n",
        prog);
}

int main(int argc, char **argv) {
    char devpath[64];
    int src_fd, uinput_fd;
    struct input_event ev;
    struct trackpad_axes axes;
    struct dead_zones dz = { .left_pct = 15, .right_pct = 15, .top_pct = 20, .bottom_pct = 0 };

    /* Per-slot tracking */
    int slot_x[MAX_SLOTS] = {0};
    int slot_y[MAX_SLOTS] = {0};
    int slot_has_x[MAX_SLOTS] = {0};
    int slot_has_y[MAX_SLOTS] = {0};
    int slot_blocked[MAX_SLOTS] = {0};
    int slot_tracking[MAX_SLOTS] = {0};
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
        } else if (strcmp(argv[i], "--bottom") == 0 && i + 1 < argc) {
            dz.bottom_pct = atoi(argv[++i]);
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    /* Validate */
    if (dz.left_pct + dz.right_pct >= 100 || dz.top_pct + dz.bottom_pct >= 100) {
        fprintf(stderr, "Error: Dead zones exceed 100%% of trackpad area\n");
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Find trackpad */
    if (find_trackpad(devpath, sizeof(devpath)) < 0) {
        fprintf(stderr, "Error: Cannot find Apple SPI Trackpad\n");
        return 1;
    }

    /* Open source device */
    src_fd = open(devpath, O_RDONLY);
    if (src_fd < 0) {
        perror("open trackpad");
        return 1;
    }

    /* Get axis ranges from device */
    if (get_axes(src_fd, &axes) < 0) {
        close(src_fd);
        return 1;
    }

    /* Compute boundaries */
    compute_boundaries(&dz, &axes);

    printf("Trackpad: %s\n", devpath);
    printf("Axes: X[%d, %d] Y[%d, %d]\n", axes.x_min, axes.x_max, axes.y_min, axes.y_max);
    printf("Dead zones: left=%d%% right=%d%% top=%d%% bottom=%d%%\n",
           dz.left_pct, dz.right_pct, dz.top_pct, dz.bottom_pct);
    printf("Boundaries: x_left=%d x_right=%d y_top=%d y_bottom=%d\n",
           dz.x_left, dz.x_right, dz.y_top, dz.y_bottom);
    fflush(stdout);

    /* Set up uinput virtual device */
    uinput_fd = setup_uinput(src_fd);
    if (uinput_fd < 0) {
        close(src_fd);
        return 1;
    }

    /* Grab exclusive access */
    if (ioctl(src_fd, EVIOCGRAB, 1) < 0) {
        perror("EVIOCGRAB");
        close(src_fd);
        ioctl(uinput_fd, UI_DEV_DESTROY);
        close(uinput_fd);
        return 1;
    }

    printf("Filter active. Send SIGTERM to stop.\n");
    fflush(stdout);

    /* Event buffer for batching */
    struct input_event batch[64];
    int batch_count = 0;

    while (running) {
        ssize_t n = read(src_fd, &ev, sizeof(ev));
        if (n != sizeof(ev)) {
            if (errno == EINTR) continue;
            break;
        }

        if (ev.type == EV_ABS) {
            switch (ev.code) {
            case ABS_MT_SLOT:
                current_slot = ev.value;
                if (current_slot >= MAX_SLOTS) current_slot = MAX_SLOTS - 1;
                break;

            case ABS_MT_TRACKING_ID:
                if (ev.value == -1) {
                    slot_blocked[current_slot] = 0;
                    slot_tracking[current_slot] = 0;
                    slot_has_x[current_slot] = 0;
                    slot_has_y[current_slot] = 0;
                } else {
                    slot_tracking[current_slot] = 1;
                    slot_blocked[current_slot] = 0;
                    slot_has_x[current_slot] = 0;
                    slot_has_y[current_slot] = 0;
                }
                break;

            case ABS_MT_POSITION_X:
                slot_x[current_slot] = ev.value;
                slot_has_x[current_slot] = 1;
                if (!slot_blocked[current_slot]) {
                    if (slot_has_y[current_slot]) {
                        if (is_in_dead_zone(&dz, slot_x[current_slot], slot_y[current_slot], 1, 1))
                            slot_blocked[current_slot] = 1;
                    } else {
                        if (is_in_dead_zone(&dz, ev.value, 0, 1, 0))
                            slot_blocked[current_slot] = 1;
                    }
                }
                break;

            case ABS_MT_POSITION_Y:
                slot_y[current_slot] = ev.value;
                slot_has_y[current_slot] = 1;
                if (!slot_blocked[current_slot]) {
                    if (slot_has_x[current_slot]) {
                        if (is_in_dead_zone(&dz, slot_x[current_slot], slot_y[current_slot], 1, 1))
                            slot_blocked[current_slot] = 1;
                    } else {
                        if (is_in_dead_zone(&dz, 0, ev.value, 0, 1))
                            slot_blocked[current_slot] = 1;
                    }
                }
                break;

            case ABS_X:
                st_x = ev.value;
                st_has_x = 1;
                break;
            case ABS_Y:
                st_y = ev.value;
                st_has_y = 1;
                break;
            }
        }

        if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            int forward = 0;
            int all_blocked = 1;

            for (int i = 0; i < MAX_SLOTS; i++) {
                if (slot_tracking[i] && !slot_blocked[i]) {
                    all_blocked = 0;
                    forward = 1;
                    break;
                }
            }

            /* Always forward lift events */
            for (int i = 0; i < batch_count; i++) {
                if (batch[i].type == EV_ABS && batch[i].code == ABS_MT_TRACKING_ID
                    && batch[i].value == -1) {
                    forward = 1;
                    break;
                }
            }

            /* Single-touch fallback */
            if (all_blocked && st_has_x && st_has_y) {
                if (!is_in_dead_zone(&dz, st_x, st_y, 1, 1))
                    forward = 1;
            }

            if (forward) {
                for (int i = 0; i < batch_count; i++) {
                    write(uinput_fd, &batch[i], sizeof(batch[i]));
                }
                write(uinput_fd, &ev, sizeof(ev));
            }

            batch_count = 0;
            st_has_x = 0;
            st_has_y = 0;
        } else {
            if (batch_count < 64) {
                batch[batch_count++] = ev;
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
