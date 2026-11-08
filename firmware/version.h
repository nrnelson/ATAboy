#ifndef VERSION_H
#define VERSION_H

// Firmware identity.  FW_VERSION is what INFO reports; FW_INQUIRY_REV is the
// 4-character SCSI INQUIRY product revision the host sees in sysfs.
#define FW_BASE_VERSION   "0.6f3"
#define FW_VERSION        "0.6f3-api1"
#define FW_INQUIRY_REV    "api1"

// Version of the vendor-command protocol described in docs/API.md.  Bumped
// only for incompatible changes; new fields/sub-ops are added compatibly.
#define API_PROTO_VERSION 1

#ifndef FW_GIT_HASH
#define FW_GIT_HASH "unknown"
#endif

#endif
