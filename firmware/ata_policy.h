#ifndef ATA_POLICY_H
#define ATA_POLICY_H

// Classification of raw ATA commands for the pass-through write-protect /
// arming policy.  Pure function, no hardware access (unit-tested on the host).
//
//   READ       allowed even while write protect is on: cannot change user data
//   WRITE      writes user data or drive logs: needs write protect off
//   DANGEROUS  can destroy data or brick/lock the drive, or is unknown:
//              needs write protect off AND a one-shot ARM

#include <stdint.h>

typedef enum {
    ATA_CLASS_READ = 0,
    ATA_CLASS_WRITE = 1,
    ATA_CLASS_DANGEROUS = 2,
} ata_class_t;

ata_class_t ata_classify(uint8_t command, uint8_t feature);
const char *ata_class_name(ata_class_t c);

#endif
