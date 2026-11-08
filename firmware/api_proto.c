// Protocol names and op properties shared by the firmware and the host
// library (api_proto.h).

#include "api_proto.h"
#include <string.h>

const char *api_result_name(int r) {
    static const char *const names[] = {
        "ok", "bad_subop", "bad_arg", "no_drive", "not_mounted", "write_protected",
        "not_armed", "ata_error", "timeout", "not_ready", "bus_fault", "no_geometry",
        "mounted", "identify_failed", "unsupported", "protocol", "alloc_too_small",
        "geometry_mismatch", "request_id_conflict",
    };
    return (r >= 0 && r < (int)(sizeof(names) / sizeof(names[0]))) ? names[r] : "unknown";
}

const char *api_ide_result_name(int r) {
    switch (r) {
    case APIIDE_OK:        return "ok";
    case APIIDE_ATA_ERROR: return "ata_error";
    case APIIDE_TIMEOUT:   return "timeout";
    case APIIDE_NOT_READY: return "not_ready";
    case APIIDE_BUS_FAULT: return "bus_fault";
    default:               return "protocol";
    }
}

static const struct { uint8_t op; const char *name; } ops[] = {
    {OP_INFO, "info"}, {OP_STATUS, "status"}, {OP_CONFIG_GET, "config_get"},
    {OP_CONFIG_SET, "config_set"}, {OP_CONFIG_SAVE, "config_save"},
    {OP_CONFIG_DEFAULTS, "config_defaults"}, {OP_DETECT, "detect"}, {OP_GEOMETRY, "geometry"},
    {OP_MOUNT, "mount"}, {OP_UNMOUNT, "unmount"}, {OP_RESET, "reset"}, {OP_PARK, "park"},
    {OP_IDENTIFY, "identify"}, {OP_TASKFILE, "taskfile"}, {OP_READ, "read"},
    {OP_READ_LONG, "read_long"}, {OP_VERIFY, "verify"}, {OP_CORRUPT, "corrupt"},
    {OP_HASHES, "hashes"}, {OP_LOG, "log"}, {OP_LOG_CLEAR, "log_clear"}, {OP_ARM, "arm"},
    {OP_REBOOT, "reboot"},
};

const char *api_op_name(uint8_t op) {
    for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
        if (ops[i].op == op) return ops[i].name;
    return "unknown";
}

int api_op_by_name(const char *name) {
    for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
        if (strcmp(ops[i].name, name) == 0) return ops[i].op;
    return -1;
}

bool api_op_takes_rid(uint8_t op) {
    switch (op) {
    case OP_CONFIG_SET: case OP_CONFIG_SAVE: case OP_CONFIG_DEFAULTS:
    case OP_DETECT: case OP_GEOMETRY: case OP_MOUNT: case OP_UNMOUNT: case OP_RESET:
    case OP_CORRUPT: case OP_LOG_CLEAR: case OP_ARM: case OP_REBOOT: case OP_PARK:
        return true;
    default:
        return false;
    }
}

bool api_op_read_only(uint8_t op) {
    switch (op) {
    case OP_INFO: case OP_STATUS: case OP_CONFIG_GET: case OP_IDENTIFY: case OP_TASKFILE:
    case OP_READ: case OP_READ_LONG: case OP_VERIFY: case OP_LOG: case OP_HASHES:
        return true;
    default:
        return false;
    }
}
