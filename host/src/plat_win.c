// Platform layer for Windows: clock, random numbers, serial (COM) port.
// The ATAboy's serial ports use Windows' built-in usbser.sys (Windows 10+):
// no driver install, no admin rights.

#if defined(_WIN32)

#define _CRT_RAND_S
#include "internal.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

uint64_t ab_now_ms(void) { return (uint64_t)GetTickCount64(); }

void ab_sleep_ms(uint32_t ms) { Sleep(ms); }

uint32_t ab_random32(void) {
    unsigned int v = 0;
    if (rand_s(&v) == 0) return (uint32_t)v;
    return (uint32_t)GetTickCount64() ^ (uint32_t)GetCurrentProcessId() << 16;
}

struct ab_serial { HANDLE h; };

bool ab_is_serial_path(const char *path) {
    if (!path) return false;
    if (strncmp(path, "\\\\.\\", 4) == 0) path += 4;
    return (toupper((unsigned char)path[0]) == 'C' && toupper((unsigned char)path[1]) == 'O' &&
            toupper((unsigned char)path[2]) == 'M' && isdigit((unsigned char)path[3]));
}

static const char *win_msg(DWORD e, char *buf, size_t n) {
    if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, buf,
                        (DWORD)n, NULL))
        snprintf(buf, n, "error %lu", (unsigned long)e);
    size_t l = strlen(buf);
    while (l && (buf[l - 1] == '\n' || buf[l - 1] == '\r' || buf[l - 1] == '.')) buf[--l] = '\0';
    return buf;
}

int ab_serial_open(const char *path, ab_serial **out) {
    char full[64], m[256];
    if (strncmp(path, "\\\\.\\", 4) == 0) snprintf(full, sizeof(full), "%s", path);
    else snprintf(full, sizeof(full), "\\\\.\\%s", path);
    // No sharing: one program at a time, like TIOCEXCL on POSIX.
    HANDLE h = CreateFileA(full, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_ACCESS_DENIED)
            return ab_fail(AB_ERR_TRANSPORT, "%s is in use by another program (close terminals such as PuTTY)", path);
        return ab_fail(AB_ERR_TRANSPORT, "cannot open %s: %s", path, win_msg(e, m, sizeof(m)));
    }
    DCB dcb;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) {
        DWORD e = GetLastError();
        CloseHandle(h);
        return ab_fail(AB_ERR_TRANSPORT, "%s is not a serial port: %s", path, win_msg(e, m, sizeof(m)));
    }
    dcb.BaudRate = 115200;                  // ignored by CDC ACM, must be valid
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;   // the firmware sees the port as open
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(h, &dcb)) {
        DWORD e = GetLastError();
        CloseHandle(h);
        return ab_fail(AB_ERR_TRANSPORT, "cannot set up %s: %s", path, win_msg(e, m, sizeof(m)));
    }
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);
    ab_serial *s = calloc(1, sizeof(*s));
    if (!s) { CloseHandle(h); return ab_fail(AB_ERR_NOMEM, "out of memory"); }
    s->h = h;
    *out = s;
    return AB_OK;
}

static DWORD left_ms(uint64_t deadline_ms) {
    uint64_t now = ab_now_ms();
    if (now >= deadline_ms) return 0;
    uint64_t l = deadline_ms - now;
    return l > 0x7FFFFFFF ? 0x7FFFFFFF : (DWORD)l;
}

int ab_serial_write(ab_serial *s, const uint8_t *buf, size_t n, uint64_t deadline_ms) {
    char m[256];
    while (n) {
        DWORD l = left_ms(deadline_ms);
        if (!l) return ab_fail(AB_ERR_TRANSPORT, "serial write timed out");
        COMMTIMEOUTS to = {0};
        to.WriteTotalTimeoutConstant = l;
        SetCommTimeouts(s->h, &to);
        DWORD w = 0;
        DWORD chunk = n > 65536 ? 65536 : (DWORD)n;
        if (!WriteFile(s->h, buf, chunk, &w, NULL))
            return ab_fail(AB_ERR_TRANSPORT, "serial write failed: %s", win_msg(GetLastError(), m, sizeof(m)));
        buf += w;
        n -= w;
    }
    return AB_OK;
}

long ab_serial_read(ab_serial *s, uint8_t *buf, size_t cap, uint64_t deadline_ms) {
    char m[256];
    for (;;) {
        DWORD l = left_ms(deadline_ms);
        if (!l) return 0;
        // Return at once with whatever is buffered; otherwise wait up to l ms
        // for the first byte (documented ReadIntervalTimeout = MAXDWORD case).
        COMMTIMEOUTS to = {0};
        to.ReadIntervalTimeout = MAXDWORD;
        to.ReadTotalTimeoutMultiplier = MAXDWORD;
        to.ReadTotalTimeoutConstant = l;
        SetCommTimeouts(s->h, &to);
        DWORD got = 0;
        DWORD want = cap > 65536 ? 65536 : (DWORD)cap;
        if (!ReadFile(s->h, buf, want, &got, NULL))
            return ab_fail(AB_ERR_TRANSPORT, "serial read failed (device unplugged?): %s",
                           win_msg(GetLastError(), m, sizeof(m)));
        if (got) return (long)got;
    }
}

void ab_serial_drain(ab_serial *s) {
    PurgeComm(s->h, PURGE_RXCLEAR);
}

void ab_serial_close(ab_serial *s) {
    if (!s) return;
    CloseHandle(s->h);
    free(s);
}

#endif
