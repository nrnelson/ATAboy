#ifndef ATABOY_CLI_H
#define ATABOY_CLI_H

#include "ataboy.h"

enum { EXIT_OK = 0, EXIT_DEVICE = 1, EXIT_USAGE = 2, EXIT_TRANSPORT = 3, EXIT_PERM = 4 };

// Human-readable renderings (human.c), same layout as the Python tool.
void human_status(const cJSON *j);
void human_log_entry(const cJSON *x, const char *prefix);
void human_ata_result(const cJSON *d);
void human_park(const cJSON *j);
void human_taskfile(const cJSON *j);
void human_keyvals(const cJSON *j, const char *skip);
void hexdump(const uint8_t *data, size_t n, uint64_t base);
// human_recover.c
void human_image(const cJSON *o);
void human_rescue(const cJSON *o);
void human_hashes(const cJSON *j);

// Boot ids seen by earlier runs (bootcache.c), so "status" reports a reboot
// that happened between two commands.
void bootcache_seed(ab_dev *dev);
void bootcache_save(ab_dev *dev);

#endif
