// Replay driver for the fuzz targets in builds without libFuzzer (gcc,
// mingw, or clang without ATABOY_FUZZ; see "Fuzz targets" in CMakeLists.txt).
//
//   fuzz_<name> [file|directory]...
//
// Feeds the target's LLVMFuzzerTestOneInput() an empty input, then every file
// named (every regular file of a directory, not recursive), each in a buffer
// of exactly its size so the sanitizers see any over-read.  Exits non-zero
// if a path can't be read; a bug in the target crashes or trips a sanitizer.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_MSC_VER)
#include <windows.h>
#else
#include <dirent.h>
#endif

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define REPLAY_MAX_INPUT (64u << 20)

static unsigned replayed;

static int run_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "%s: cannot open\n", path); return 1; }
    size_t cap = 4096, n = 0;
    uint8_t *buf = malloc(cap);
    for (;;) {
        if (!buf) { fclose(f); fprintf(stderr, "%s: out of memory\n", path); return 1; }
        n += fread(buf + n, 1, cap - n, f);
        if (n < cap || cap >= REPLAY_MAX_INPUT) break;
        uint8_t *nb = realloc(buf, cap * 2);
        if (!nb) { free(buf); buf = NULL; continue; }
        buf = nb;
        cap *= 2;
    }
    int err = ferror(f);
    fclose(f);
    if (err) { free(buf); fprintf(stderr, "%s: read error\n", path); return 1; }
    uint8_t *exact = malloc(n ? n : 1);           // exactly n bytes: over-reads are caught
    if (!exact) { free(buf); fprintf(stderr, "%s: out of memory\n", path); return 1; }
    memcpy(exact, buf, n);
    free(buf);
    LLVMFuzzerTestOneInput(exact, n);
    free(exact);
    replayed++;
    return 0;
}

static int run_dir(const char *dir) {
    int rc = 0;
    char path[4096];
#if defined(_MSC_VER)
    WIN32_FIND_DATAA fd;
    snprintf(path, sizeof(path), "%s\\*", dir);
    HANDLE h = FindFirstFileA(path, &fd);
    if (h == INVALID_HANDLE_VALUE) { fprintf(stderr, "%s: cannot list\n", dir); return 1; }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
        rc |= run_file(path);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) { fprintf(stderr, "%s: cannot list\n", dir); return 1; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;        // ., .. and hidden files
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        rc |= run_file(path);
    }
    closedir(d);
#endif
    return rc;
}

int main(int argc, char **argv) {
    int rc = 0;
    LLVMFuzzerTestOneInput((const uint8_t *)"", 0);
    replayed++;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) != 0) { fprintf(stderr, "%s: not found\n", argv[i]); rc = 1; continue; }
        rc |= (st.st_mode & S_IFMT) == S_IFDIR ? run_dir(argv[i]) : run_file(argv[i]);
    }
    printf("%s: replayed %u input(s)%s\n", argv[0], replayed, rc ? ", with errors" : "");
    return rc;
}
