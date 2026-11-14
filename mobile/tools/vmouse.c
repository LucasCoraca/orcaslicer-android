// virtual USB-like mouse via uinput for testing: reads commands from stdin
//   m dx dy | b left|right|middle 1|0 | w n | s ms | q
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static int fd;
static void emit(int type, int code, int val) { struct input_event e; memset(&e, 0, sizeof e); e.type = type; e.code = code; e.value = val; write(fd, &e, sizeof e); }
static void syn(void) { emit(EV_SYN, SYN_REPORT, 0); }
int main(void) {
    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("uinput"); return 1; }
    ioctl(fd, UI_SET_EVBIT, EV_KEY); ioctl(fd, UI_SET_KEYBIT, BTN_LEFT); ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT); ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(fd, UI_SET_EVBIT, EV_REL); ioctl(fd, UI_SET_RELBIT, REL_X); ioctl(fd, UI_SET_RELBIT, REL_Y); ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
    struct uinput_setup us; memset(&us, 0, sizeof us); us.id.bustype = BUS_USB; us.id.vendor = 0x1234; us.id.product = 0x5678; strcpy(us.name, "orca test mouse");
    ioctl(fd, UI_DEV_SETUP, &us); ioctl(fd, UI_DEV_CREATE);
    char line[128];
    while (fgets(line, sizeof line, stdin)) {
        int a, b; char w[16];
        if (sscanf(line, "m %d %d", &a, &b) == 2) {
            // small steps: Android's pointer acceleration stays near 1 at low speed
            int sx = a > 0 ? 1 : -1, sy = b > 0 ? 1 : -1, ax = a * sx, ay = b * sy;
            while (ax > 0 || ay > 0) { int dx = ax > 4 ? 4 : ax, dy = ay > 4 ? 4 : ay; emit(EV_REL, REL_X, dx * sx); emit(EV_REL, REL_Y, dy * sy); syn(); ax -= dx; ay -= dy; usleep(8000); }
        } else if (sscanf(line, "b %15s %d", w, &a) == 2) {
            emit(EV_KEY, !strcmp(w, "left") ? BTN_LEFT : !strcmp(w, "right") ? BTN_RIGHT : BTN_MIDDLE, a); syn();
        } else if (sscanf(line, "w %d", &a) == 1) { emit(EV_REL, REL_WHEEL, a); syn(); }
        else if (sscanf(line, "s %d", &a) == 1) usleep(a * 1000);
        else if (line[0] == 'q') break;
    }
    ioctl(fd, UI_DEV_DESTROY); close(fd); return 0;
}
