// SHA-256 against the FIPS 180-4 / NIST example vectors.
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void hex(const uint8_t d[32], char out[65]) {
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

static bool one(const void *m, size_t n, const char *want) {
    uint8_t d[32];
    char h[65];
    ab_sha256(m, n, d);
    hex(d, h);
    if (strcmp(h, want)) { printf("  got  %s\n  want %s\n", h, want); return false; }
    return true;
}

int main(void) {
    CHECK(one("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK(one("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const char *m448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(one(m448, strlen(m448), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    const char *m896 = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    CHECK(one(m896, strlen(m896), "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"));
    // one million 'a', fed in uneven pieces
    {
        static char a[1000000];
        memset(a, 'a', sizeof(a));
        CHECK(one(a, sizeof(a), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
        ab_sha256_ctx c;
        ab_sha256_init(&c);
        size_t off = 0, step = 1;
        while (off < sizeof(a)) {
            size_t k = sizeof(a) - off < step ? sizeof(a) - off : step;
            ab_sha256_update(&c, a + off, k);
            off += k;
            step = step * 3 % 1031 + 1;
        }
        uint8_t d[32];
        char h[65];
        ab_sha256_final(&c, d);
        hex(d, h);
        CHECK(!strcmp(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    }
    // every padding boundary case around 55/56/63/64 bytes against one-shot
    {
        uint8_t buf[768];
        for (int i = 0; i < 768; i++) buf[i] = (uint8_t)i;
        CHECK(one(buf, sizeof(buf), "f3a25aa93aa2fbba28d79260535bbd6a5eb0fc1c24a8b0f04e12b484c1dfe363"));
        for (size_t n = 50; n < 140; n++) {
            uint8_t d1[32], d2[32];
            ab_sha256(buf, n, d1);
            ab_sha256_ctx c;
            ab_sha256_init(&c);
            for (size_t i = 0; i < n; i++) ab_sha256_update(&c, buf + i, 1);
            ab_sha256_final(&c, d2);
            CHECK(!memcmp(d1, d2, 32));
        }
    }
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("sha256 tests passed\n");
    return 0;
}
