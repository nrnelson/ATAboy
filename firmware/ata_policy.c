#include "ata_policy.h"

ata_class_t ata_classify(uint8_t cmd, uint8_t feature) {
    // Ranges first
    if (cmd >= 0x10 && cmd <= 0x1F) return ATA_CLASS_READ;        // RECALIBRATE
    if (cmd >= 0x70 && cmd <= 0x7F) return ATA_CLASS_READ;        // SEEK

    switch (cmd) {
    // ---- read-only / state-only ----
    case 0x00:              // NOP
    case 0x03:              // CFA REQUEST EXTENDED ERROR
    case 0x08:              // DEVICE RESET
    case 0x0B:              // REQUEST SENSE DATA EXT
    case 0x20: case 0x21:   // READ SECTORS (w/ and w/o retry)
    case 0x22: case 0x23:   // READ LONG (w/ and w/o retry)
    case 0x24:              // READ SECTORS EXT
    case 0x25:              // READ DMA EXT
    case 0x26:              // READ DMA QUEUED EXT
    case 0x27:              // READ NATIVE MAX ADDRESS EXT
    case 0x29:              // READ MULTIPLE EXT
    case 0x2A: case 0x2B:   // READ STREAM (DMA) EXT
    case 0x2F:              // READ LOG EXT
    case 0x40: case 0x41:   // READ VERIFY SECTORS (w/ and w/o retry)
    case 0x42:              // READ VERIFY SECTORS EXT
    case 0x47:              // READ LOG DMA EXT
    case 0x4A:              // ZAC MANAGEMENT IN
    case 0x5C: case 0x5D:   // TRUSTED RECEIVE (DMA)
    case 0x60:              // READ FPDMA QUEUED
    case 0x87:              // CFA TRANSLATE SECTOR
    case 0x90:              // EXECUTE DEVICE DIAGNOSTIC
    case 0x91:              // INITIALIZE DEVICE PARAMETERS
    case 0x94: case 0x95: case 0x96: case 0x97: case 0x98: case 0x99:  // ATA-1 power commands
    case 0xA1:              // IDENTIFY PACKET DEVICE
    case 0xA2:              // SERVICE
    case 0xC4:              // READ MULTIPLE
    case 0xC6:              // SET MULTIPLE MODE
    case 0xC7:              // READ DMA QUEUED
    case 0xC8: case 0xC9:   // READ DMA (w/ and w/o retry)
    case 0xD1:              // CHECK MEDIA CARD TYPE
    case 0xDA:              // GET MEDIA STATUS
    case 0xDE: case 0xDF:   // MEDIA LOCK / UNLOCK
    case 0xE0: case 0xE1: case 0xE2: case 0xE3:  // STANDBY/IDLE (IMMEDIATE)
    case 0xE4:              // READ BUFFER
    case 0xE5:              // CHECK POWER MODE
    case 0xE6:              // SLEEP
    case 0xE7: case 0xEA:   // FLUSH CACHE (EXT)
    case 0xEC:              // IDENTIFY DEVICE
    case 0xED:              // MEDIA EJECT
    case 0xEE:              // IDENTIFY DEVICE DMA
    case 0xEF:              // SET FEATURES
    case 0xF5:              // SECURITY FREEZE LOCK (protective)
    case 0xF8:              // READ NATIVE MAX ADDRESS
        return ATA_CLASS_READ;

    case 0xB0:              // SMART
        switch (feature) {
        case 0xD0: case 0xD1:   // READ DATA / READ THRESHOLDS
        case 0xD2:              // ATTRIBUTE AUTOSAVE
        case 0xD3:              // SAVE ATTRIBUTE VALUES
        case 0xD4:              // EXECUTE OFF-LINE IMMEDIATE (self-tests)
        case 0xD5:              // READ LOG
        case 0xD8: case 0xD9:   // ENABLE / DISABLE OPERATIONS
        case 0xDA:              // RETURN STATUS
        case 0xDB:              // ENABLE/DISABLE AUTOMATIC OFF-LINE
            return ATA_CLASS_READ;
        case 0xD6:              // WRITE LOG
            return ATA_CLASS_WRITE;
        default:
            return ATA_CLASS_DANGEROUS;
        }

    case 0xB1:              // DEVICE CONFIGURATION OVERLAY
        if (feature == 0xC2 || feature == 0xC1) return ATA_CLASS_READ;  // IDENTIFY / FREEZE LOCK
        return ATA_CLASS_DANGEROUS;                                     // RESTORE / SET

    // ---- writes user data or logs ----
    case 0x30: case 0x31:   // WRITE SECTORS (w/ and w/o retry)
    case 0x34:              // WRITE SECTORS EXT
    case 0x35:              // WRITE DMA EXT
    case 0x36:              // WRITE DMA QUEUED EXT
    case 0x38:              // CFA WRITE SECTORS WITHOUT ERASE
    case 0x39:              // WRITE MULTIPLE EXT
    case 0x3A: case 0x3B:   // WRITE STREAM (DMA) EXT
    case 0x3C:              // WRITE VERIFY
    case 0x3D:              // WRITE DMA FUA EXT
    case 0x3E:              // WRITE DMA QUEUED FUA EXT
    case 0x3F:              // WRITE LOG EXT
    case 0x57:              // WRITE LOG DMA EXT
    case 0x61:              // WRITE FPDMA QUEUED
    case 0xC5:              // WRITE MULTIPLE
    case 0xCA: case 0xCB:   // WRITE DMA (w/ and w/o retry)
    case 0xCC:              // WRITE DMA QUEUED
    case 0xCD:              // CFA WRITE MULTIPLE WITHOUT ERASE
    case 0xCE:              // WRITE MULTIPLE FUA EXT
    case 0xE8:              // WRITE BUFFER
    case 0xEB:              // WRITE BUFFER DMA
        return ATA_CLASS_WRITE;

    // Everything else -- WRITE LONG, FORMAT TRACK, DOWNLOAD MICROCODE,
    // SECURITY (set password/unlock/erase/disable), SET MAX, TRIM, SANITIZE,
    // WRITE UNCORRECTABLE, vendor-specific opcodes and anything unknown.
    default:
        return ATA_CLASS_DANGEROUS;
    }
}

const char *ata_class_name(ata_class_t c) {
    switch (c) {
    case ATA_CLASS_READ:  return "read";
    case ATA_CLASS_WRITE: return "write";
    default:              return "dangerous";
    }
}
