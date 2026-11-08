#ifndef API_PROTO_H
#define API_PROTO_H

// The ATAboy API protocol as both ends see it (docs/API.md): sub-operation
// numbers, config keys, reply layout, result codes and their names.  Pure C
// with no SDK dependency: the firmware and the host library (host/) build
// this same file, so the two can't disagree about the protocol.

#include <stdint.h>
#include <stdbool.h>

#define API_OPCODE        0xC0     // vendor SCSI opcode (CDB byte 0)
#define API_PROTO_VERSION 1        // reply byte 2; bumped only for incompatible changes
#define API_CDB_LEN       16
#define API_MIN_ALLOC     256      // smaller allocation lengths get ILLEGAL REQUEST
#define API_MAX_REPLY     4096

// CDB bytes 2-3
#define API_SIG0          'A'
#define API_SIG_BASIC     'B'      // 16-byte reply header
#define API_SIG_EXT       'X'      // 32-byte reply header, run-once request id in CDB 12-15

// Sub-operations (CDB byte 1)
#define OP_INFO            0x00
#define OP_STATUS          0x01
#define OP_CONFIG_GET      0x02
#define OP_CONFIG_SET      0x03
#define OP_CONFIG_SAVE     0x04
#define OP_CONFIG_DEFAULTS 0x05
#define OP_DETECT          0x10
#define OP_GEOMETRY        0x11
#define OP_MOUNT           0x12
#define OP_UNMOUNT         0x13
#define OP_RESET           0x14
#define OP_PARK            0x15
#define OP_IDENTIFY        0x20
#define OP_TASKFILE        0x21
#define OP_READ            0x22
#define OP_READ_LONG       0x23
#define OP_VERIFY          0x24
#define OP_CORRUPT         0x25
#define OP_HASHES          0x26
#define OP_LOG             0x30
#define OP_LOG_CLEAR       0x31
#define OP_ARM             0x40
#define OP_REBOOT          0x41

// CONFIG_SET keys (CDB byte 4)
#define KEY_WRITE_PROTECT  1
#define KEY_AUTO_MOUNT     2
#define KEY_IORDY          3
#define KEY_INTRQ          4
#define KEY_READ_TIMEOUT   5
#define KEY_WRITE_TIMEOUT  6
#define KEY_ECC_LEN        7
#define KEY_NO_RETRY       8
#define KEY_DEVICE         9

// DETECT / GEOMETRY geometry modes (CDB byte 5)
#define APIGEO_AUTO        0
#define APIGEO_NORMAL      1
#define APIGEO_LARGE       2
#define APIGEO_LBA         3
#define APIGEO_MANUAL      4
#define APIGEO_KEEP        5

// Reply header
#define HDR_LEN            16
#define EXT_HDR_LEN        32
#define CT_NONE            0       // byte 5: content type
#define CT_JSON            1
#define CT_BINARY          2
#define HF_MOUNTED         0x01    // byte 12: flags
#define HF_WRITE_PROTECT   0x02
#define HF_IDENTIFIED      0x04
#define HF_ARMED           0x08
#define HF_GEOMETRY_VALID  0x10
#define XF_REPLAYED        0x01    // byte 28 (extended): stored reply, nothing ran
#define XF_RID             0x02    // the request id was taken

// Result codes (reply byte 4)
#define R_OK               0
#define R_BAD_SUBOP        1
#define R_BAD_ARG          2
#define R_NO_DRIVE         3
#define R_NOT_MOUNTED      4
#define R_WRITE_PROTECTED  5
#define R_NOT_ARMED        6
#define R_ATA_ERROR        7
#define R_TIMEOUT          8
#define R_NOT_READY        9
#define R_BUS_FAULT       10
#define R_NO_GEOMETRY     11
#define R_MOUNTED         12
#define R_IDENTIFY_FAILED 13
#define R_UNSUPPORTED     14
#define R_PROTOCOL        15
#define R_ALLOC_TOO_SMALL 16
#define R_GEO_MISMATCH    17   // configured geometry belongs to another drive
#define R_ID_CONFLICT     18   // run-once request id reused for a different request

// Binary ATA result block (READ, READ_LONG, VERIFY, CORRUPT): RES_LEN bytes
#define RES_LEN            32
#define RES_LBA            0    // u32 first LBA requested
#define RES_COUNT          4    // u16 sectors requested
#define RES_DONE           6    // u16 sectors transferred without error
#define RES_STATUS         8
#define RES_ERROR          9
#define RES_COMMAND        10
#define RES_FLAGS          11
#define RES_ELAPSED_US     12   // u32
#define RES_FAIL_LBA       16   // u32, FFFFFFFF on success
#define RES_TF             20   // 5 bytes: count, sector/lba lo, cyl lo, cyl hi, device/head
#define RES_ECC_LEN        25
#define RES_IDE_RESULT     26   // signed engine result (IDE_* below)
#define RES_DONE32         28   // u32, VERIFY sectors verified
#define RF_TIMED_OUT       0x01
#define RF_ERR_DATA        0x02
#define RF_TF_VALID        0x04
#define RF_RECOVERED       0x08
#define RF_BUS_FAULT       0x10
#define RF_NOT_READY       0x20
#define RF_PARTIAL         0x40

// Engine results in RES_IDE_RESULT (same values as firmware ide.h IDE_*)
#define APIIDE_OK          0
#define APIIDE_ATA_ERROR  -1
#define APIIDE_TIMEOUT    -2
#define APIIDE_NOT_READY  -3
#define APIIDE_BUS_FAULT  -4
#define APIIDE_PROTOCOL   -5

// HASHES reply payload: 32-byte header, then records of HASH_REC_LEN bytes
#define HASH_HDR_LEN       32
#define HASH_REC_LEN       44

// Names as they appear in JSON ("ok", "bad_arg", ...).  Never NULL.
const char *api_result_name(int result);
const char *api_ide_result_name(int ide_result);
const char *api_op_name(uint8_t op);            // "info", "status", ... or "unknown"
int         api_op_by_name(const char *name);   // -1 if unknown

// Ops that take a run-once request id in an extended request (INFO rid_ops).
// Their arguments never use CDB bytes 12-15.
bool api_op_takes_rid(uint8_t op);
// Read-only ops a client may simply re-send after an empty or lost reply.
bool api_op_read_only(uint8_t op);

// ATA status/error register names: ata_status_bits() / ata_error_bits() in json.h.

#endif
