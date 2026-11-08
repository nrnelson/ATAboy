// Platform layer for Linux and macOS: clock, random numbers, serial port.

#if !defined(_WIN32)

#include "internal.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

uint64_t ab_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void ab_sleep_ms(uint32_t ms) {
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

uint32_t ab_random32(void) {
    uint32_t v = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = read(fd, &v, sizeof(v));
        close(fd);
        if (n == (ssize_t)sizeof(v)) return v;
    }
    struct timespec ts;                      // fallback: not secret, just varied
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint32_t)ts.tv_nsec ^ ((uint32_t)ts.tv_sec << 12) ^ (uint32_t)getpid();
}

struct ab_serial { int fd; };

bool ab_is_serial_path(const char *path) {
    if (!path || !*path) return false;
    char real[PATH_MAX];
    const char *p = realpath(path, real) ? real : path;
    const char *base = strrchr(p, '/');
    base = base ? base + 1 : p;
    return strncmp(base, "tty", 3) == 0 || strncmp(base, "cu.", 3) == 0;
}

int ab_serial_open(const char *path, ab_serial **out) {
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (errno == EACCES || errno == EPERM)
            return ab_fail(AB_ERR_PERM, "no access to %s (add the user to the group that owns it, "
                           "usually dialout or uucp, and log in again)", path);
        if (errno == EBUSY)
            return ab_fail(AB_ERR_TRANSPORT, "%s is in use by another program", path);
        return ab_fail(AB_ERR_TRANSPORT, "cannot open %s: %s", path, strerror(errno));
    }
    // One program at a time: interleaved frames from two programs would only
    // time out.  (Root can still open the port.)
#ifdef TIOCEXCL
    if (ioctl(fd, TIOCEXCL) != 0 && errno != ENOTTY) { /* best effort */ }
#endif
    struct termios t;
    if (tcgetattr(fd, &t) != 0) {
        int e = errno;
        close(fd);
        return ab_fail(AB_ERR_TRANSPORT, "%s is not a serial port: %s", path, strerror(e));
    }
    cfmakeraw(&t);
    t.c_cflag |= CREAD | CLOCAL;
    t.c_cflag &= ~(tcflag_t)(CRTSCTS | HUPCL);
    t.c_iflag &= ~(tcflag_t)(IXON | IXOFF | IXANY);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &t) != 0) {
        int e = errno;
        close(fd);
        return ab_fail(AB_ERR_TRANSPORT, "cannot set up %s: %s", path, strerror(e));
    }
    tcflush(fd, TCIOFLUSH);
    ab_serial *s = calloc(1, sizeof(*s));
    if (!s) { close(fd); return ab_fail(AB_ERR_NOMEM, "out of memory"); }
    s->fd = fd;
    *out = s;
    return AB_OK;
}

static int wait_fd(int fd, short ev, uint64_t deadline_ms) {
    for (;;) {
        uint64_t now = ab_now_ms();
        if (now >= deadline_ms) return 0;
        uint64_t left = deadline_ms - now;
        struct pollfd p = {fd, ev, 0};
        int r = poll(&p, 1, left > INT_MAX ? INT_MAX : (int)left);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return -1;
        if (r == 0) return 0;
        if (p.revents & (POLLERR | POLLNVAL)) return -1;
        return 1;                              // POLLHUP: let read() report it
    }
}

int ab_serial_write(ab_serial *s, const uint8_t *buf, size_t n, uint64_t deadline_ms) {
    while (n) {
        int w = wait_fd(s->fd, POLLOUT, deadline_ms);
        if (w == 0) return ab_fail(AB_ERR_TRANSPORT, "serial write timed out");
        if (w < 0) return ab_fail(AB_ERR_TRANSPORT, "serial port error (device unplugged?)");
        ssize_t k = write(s->fd, buf, n);
        if (k < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            return ab_fail(AB_ERR_TRANSPORT, "serial write failed: %s", strerror(errno));
        }
        buf += k;
        n -= (size_t)k;
    }
    return AB_OK;
}

long ab_serial_read(ab_serial *s, uint8_t *buf, size_t cap, uint64_t deadline_ms) {
    for (;;) {
        int w = wait_fd(s->fd, POLLIN, deadline_ms);
        if (w == 0) return 0;
        if (w < 0) return ab_fail(AB_ERR_TRANSPORT, "serial port error (device unplugged?)");
        ssize_t k = read(s->fd, buf, cap);
        if (k > 0) return (long)k;
        if (k == 0) return ab_fail(AB_ERR_TRANSPORT, "serial port closed (device unplugged?)");
        if (errno == EAGAIN || errno == EINTR) continue;
        return ab_fail(AB_ERR_TRANSPORT, "serial read failed: %s", strerror(errno));
    }
}

void ab_serial_drain(ab_serial *s) {
    uint8_t junk[4096];
    tcflush(s->fd, TCIFLUSH);
    for (int i = 0; i < 64; i++) {
        struct pollfd p = {s->fd, POLLIN, 0};
        if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN)) return;
        if (read(s->fd, junk, sizeof(junk)) <= 0) return;
    }
}

void ab_serial_close(ab_serial *s) {
    if (!s) return;
    close(s->fd);
    free(s);
}

#endif
