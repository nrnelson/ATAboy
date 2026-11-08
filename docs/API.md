# ATAboy automation API (protocol 1)

This firmware lets software control the ATAboy over USB without the serial
menu: everything the menu can do (detect, set geometry, mount, change
settings, run diagnostics) plus things it can't (raw ATA commands, READ LONG,
surface scans, a full error log, remote firmware updates).

There are two ways in, both over the ATAboy's existing USB mass-storage
interface. They're available whether or not a drive is mounted; the USB disk
just shows 0 bytes until one is:

| Interface | SCSI opcode | Use it for |
|---|---|---|
| **ATAboy API** | `0xC0` (vendor) | state, configuration, detection, mounting, diagnostics, logs |
| **SAT ATA PASS-THROUGH** | `0x85` (16) / `0xA1` (12) | raw ATA commands; makes `smartctl -d sat`, `hdparm`, `sg_sat_identify` work unchanged |

The `ataboy` host tool (`host/ataboy`) wraps both. The rest of this document
is the wire format, for anyone writing another client.

On Linux both are sent with the `SG_IO` ioctl on `/dev/sgN` or `/dev/sdX`.
Vendor and pass-through opcodes need `CAP_SYS_RAWIO` (root).

## Quick start

```
ataboy status                       # one-call summary: drive, geometry, mount, config, errors
ataboy detect --mount               # probe + IDENTIFY + geometry (LBA if supported) + mount
ataboy config read_timeout_ms=15000 # let a failing drive finish its own retries
ataboy -j read 19969 --through-error --out sector.bin
ataboy readlong 19969 --vote 5 --image disk.img
ataboy verify 0 336260              # drive-internal READ VERIFY scan, lists bad LBAs
ataboy log                          # every failed command with ATA status/error/registers
ataboy rescue disk.img disk.map     # after ddrescue: retry just the bad sectors, patch image + map
smartctl -d sat -a /dev/sdX         # standard tools via SAT pass-through
ataboy reboot --bootsel             # UF2 bootloader for a firmware update
```

`-j` / `ATABOY_JSON=1` gives JSON on stdout. Exit status: `0` ok, `1` the device
reported a failure (ATA error, bad argument, ...), `2` usage, `3` transport /
no device, `4` permission.

## Request format (vendor opcode 0xC0)

Every request is a **16-byte CDB, data-in**. Ask for 4096 bytes; the device
returns fewer and the residue tells you how many.

| Byte | Meaning |
|---|---|
| 0 | `0xC0` |
| 1 | sub-operation (table below) |
| 2-3 | `'A' 'B'` (0x41 0x42) signature; any other value gets ILLEGAL REQUEST |
| 4-15 | arguments, big-endian, per sub-op |

The allocation length must be at least 256 bytes, otherwise the command fails
with ILLEGAL REQUEST (24h/00h).

## Reply format

Every reply starts with a 16-byte header:

| Byte | Meaning |
|---|---|
| 0-1 | `'A' 'B'` |
| 2 | protocol version (`1`) |
| 3 | sub-op echo |
| 4 | result code (table below) |
| 5 | content type: `0` none, `1` JSON (UTF-8), `2` binary |
| 6-7 | payload length (BE) |
| 8-11 | device uptime in ms (BE) |
| 12 | flags: bit0 mounted, bit1 write protect, bit2 drive identified, bit3 armed, bit4 geometry valid |
| 13-15 | reserved (0) |

The payload follows. **Device-level outcomes come back in-band**: the SCSI
command succeeds and the header says what happened. CHECK CONDITION is only
used for malformed CDBs (bad signature, allocation too small), so a client
never has to parse sense data to use the API.

Every JSON payload is an object whose `"result"` field repeats the result
name, plus `"message"` with a human-readable explanation when the result is
not `ok`. When a binary op fails before touching the drive (bad argument, no
geometry), the reply carries JSON instead of binary, so check the content type
before parsing.

### Result codes

| Code | Name | Meaning |
|---|---|---|
| 0 | `ok` | |
| 1 | `bad_subop` | unknown sub-operation |
| 2 | `bad_arg` | argument out of range |
| 3 | `no_drive` | nothing answered on the bus |
| 4 | `not_mounted` | |
| 5 | `write_protected` | |
| 6 | `not_armed` | |
| 7 | `ata_error` | the drive set ERR; see status/error registers |
| 8 | `timeout` | the drive was still BSY (or no DRQ) when the timeout ran out; the firmware issued SRST |
| 9 | `not_ready` | the drive never became ready to accept the command |
| 10 | `bus_fault` | a PIO burst timed out (IORDY stuck low) |
| 11 | `no_geometry` | detect or set geometry first |
| 12 | `mounted` | operation not allowed while mounted; unmount first |
| 13 | `identify_failed` | the drive is present but IDENTIFY failed |
| 14 | `unsupported` | the drive lacks the feature (e.g. LBA) |
| 15 | `protocol` | the drive's data phase didn't match (e.g. wrong READ LONG ECC length) |
| 16 | `alloc_too_small` | allocation length too small for this reply |

## Sub-operations

| Op | Name | Arguments (CDB byte offsets) | Reply |
|---|---|---|---|
| 00 | INFO | — | JSON: firmware identity, protocol, git hash, board id, limits, op table, features |
| 01 | STATUS | — | JSON: everything (below) |
| 02 | CONFIG_GET | — | JSON `{"config":{...}}` |
| 03 | CONFIG_SET | [4] key, [8-11] value | JSON config (not saved) |
| 04 | CONFIG_SAVE | — | JSON config; writes flash |
| 05 | CONFIG_DEFAULTS | — | JSON config (not saved; refused while mounted) |
| 10 | DETECT | [4] device 0 auto / 1 master / 2 slave · [5] geometry 0 auto / 1 normal / 2 large / 3 lba / 4 manual / 5 keep · [6-7] cyls · [8] heads · [9] spt · [10] flags: bit0 skip probe, bit1 mount, bit2 force | JSON status |
| 11 | GEOMETRY | [5] mode 1 normal / 2 large / 3 lba / 4 manual · [6-7] cyls · [8] heads · [9] spt | JSON status |
| 12 | MOUNT | — | JSON status |
| 13 | UNMOUNT | — | JSON status |
| 14 | RESET | [4] 0 soft (SRST) / 1 hard (RESET line, recalibrate) | JSON status |
| 20 | IDENTIFY | [4] bit0 cached (no bus access) | binary: 512 bytes of IDENTIFY DEVICE data |
| 21 | TASKFILE | — | JSON registers (`valid` false while BSY) |
| 22 | READ | [4-7] LBA · [8-9] count (1..`max_read_sectors`) · [10] flags: bit0 no retry (0x21), bit1 read-through-error · [12-15] per-sector timeout ms (0 = config) | binary: result block + data |
| 23 | READ_LONG | [4-7] LBA · [8] ECC bytes (0 = auto) · [10] flags: bit0 no retry (0x23) · [12-15] timeout ms | binary: result block + 512 data + ECC bytes |
| 24 | VERIFY | [4-7] LBA · [8-11] count · [12-15] per-command timeout ms | binary: result block (no data) |
| 30 | LOG | [4-7] start index (0 = oldest retained) | JSON page of the error log |
| 31 | LOG_CLEAR | [4] bit0 clear log, bit1 reset statistics | JSON |
| 40 | ARM | [4-7] `"ARM!"` | JSON; arms one DANGEROUS pass-through command for 10 s |
| 41 | REBOOT | [4] 0 restart / 1 BOOTSEL · [8-11] `"BOOT"` | JSON; reboots ~200 ms later |

### STATUS

```json
{
  "result": "ok", "fw": "0.6f3-api1", "uptime_ms": 81234,
  "mounted": true, "write_protect": true, "armed": false,
  "drive": {"present": true, "identified": true, "forced": false, "device": "master",
            "model": "IBM-H2172-A2", "serial": "...", "firmware": "...",
            "default_chs": [989, 10, 34], "lba": false, "lba28_sectors": 0,
            "lba48": false, "lba48_sectors": 0, "ecc_bytes": 4, "ata_major": 0},
  "geometry": {"valid": true, "mode": "normal", "addressing": "chs",
               "cyls": 989, "heads": 10, "spt": 34, "sectors": 336260},
  "bus": {"status": 80, "status_bits": "DRDY|DSC", "pio_faults": 0},
  "config": {"write_protect": true, "auto_mount": false, "iordy": false, "intrq": false,
             "read_timeout_ms": 15000, "write_timeout_ms": 1000, "ecc_len": 0,
             "ecc_len_effective": 4, "no_retry": false, "device": "master"},
  "stats": {"read_cmds": 1200, "read_sectors": 9600, "read_errors": 3, "write_cmds": 0,
            "write_sectors": 0, "write_errors": 0, "timeouts": 0, "recoveries": 0,
            "passthru_cmds": 4, "passthru_errors": 0, "passthru_blocked": 0, "api_cmds": 17},
  "errors": {"total": 3, "retained": 3, "last": { "...": "log entry, see LOG" }}
}
```

Register values are plain integers; `*_bits` fields decode them
(`"DRDY|DSC|DRQ|ERR"`, `"UNC"`).

### Config keys (CONFIG_SET)

| Key | Name | Values | Default |
|---|---|---|---|
| 1 | `write_protect` | 0/1. Blocks USB writes and write-class pass-through. Changing it while mounted raises UNIT ATTENTION so the host re-reads the WP bit. | 1 |
| 2 | `auto_mount` | 0/1 | 0 |
| 3 | `iordy` | 0/1, IORDY flow control | 0 |
| 4 | `intrq` | 0/1 | 0 |
| 5 | `read_timeout_ms` | 50..600000, per-sector DRQ wait | 1000 |
| 6 | `write_timeout_ms` | 50..600000 | 1000 |
| 7 | `ecc_len` | 0..64, READ LONG ECC bytes, 0 = IDENTIFY word 22 (else 4) | 0 |
| 8 | `no_retry` | 0/1, use READ SECTORS without retries (0x21) for CHS/LBA28 reads, including the USB disk | 0 |
| 9 | `device` | 0 master / 1 slave (refused while mounted) | 0 |

Settings live in RAM until CONFIG_SAVE. The saved block extends the stock
firmware's, so settings saved by stock v0.6f3 still load (the new fields take
their defaults).

**Long timeouts and the host.** While the firmware waits on a sector, the USB
command is outstanding. The Linux SCSI timeout for `/dev/sdX`
(`/sys/block/sdX/device/timeout`, default 30 s) must be longer than
`read_timeout_ms`; `ataboy config read_timeout_ms=N` raises it automatically.
The serial menu also freezes during a long wait, because both share core 0's
USB task.

### Binary result block (READ, READ_LONG, VERIFY)

32 bytes, big-endian, followed by data:

| Byte | Meaning |
|---|---|
| 0-3 | first LBA requested |
| 4-5 | sectors requested |
| 6-7 | sectors transferred without error |
| 8 | final ATA status |
| 9 | ATA error register (0 unless ERR) |
| 10 | ATA command issued |
| 11 | flags: bit0 timed out · bit1 data returned despite ERR · bit2 task-file registers valid · bit3 SRST issued afterwards · bit4 bus fault · bit5 not ready · bit6 partial (VERIFY time budget) |
| 12-15 | elapsed µs |
| 16-19 | failing LBA (decoded from the registers when valid, else first LBA + done; `FFFFFFFF` on success) |
| 20-24 | result registers: count, LBA low / sector, LBA mid / cyl low, LBA high / cyl high, device/head |
| 25 | ECC bytes returned (READ LONG) |
| 26 | engine result (signed): 0 ok, -1 ATA error, -2 timeout, -3 not ready, -4 bus fault, -5 protocol |
| 27 | reserved |
| 28-31 | VERIFY: sectors verified (32-bit) |

Data: `(done + (flag bit1 ? 1 : 0)) × 512` bytes. For READ LONG it's
`512 + ECC` bytes whenever data was transferred.

**Read-through-error** (READ flag bit1): when a sector fails with ERR and
DRQ both set (typically UNC), the drive is offering its best-effort data. With
this flag the firmware transfers that block too and sets result flag bit1, so
you get the sector's contents *and* the error.

**VERIFY** issues READ VERIFY (0x40/0x41/0x42) in chunks of 256 sectors. No data
crosses USB, so scanning is limited by the drive, not the 12 Mbit/s link. One
request stops at the first error or after about 10 s (flag bit6, *partial*);
continue from `lba + done`.

### LOG

```json
{"result": "ok", "total": 7, "retained": 7, "start": 0, "returned": 7, "next": null,
 "entries": [{"seq": 1, "t_ms": 51234, "op": "read", "cmd": 32, "result": "ata_error",
              "lba": 19968, "count": 8, "done": 1, "fail_lba": 19969,
              "status": 89, "status_bits": "DRDY|DSC|DRQ|ERR", "error": 64, "error_bits": "UNC",
              "tf_valid": true, "err_data": false, "recovered": false,
              "elapsed_ms": 4290, "timeout_ms": 15000, "tf": [1, 12, 58, 0, 167]}]}
```

The ring holds the last 64 failures. A page holds as many entries as fit in
4 KiB; fetch `start = next` until `next` is null. `op` is `read` (USB disk),
`write`, `read_diag` (API READ), `read_long`, `verify`, `passthrough` or `identify`.
`tf` is null when the drive was still BSY. While BSY is set, every register
reads back as STATUS, so those values would be garbage.

## SAT ATA PASS-THROUGH

ATA PASS-THROUGH (16) `0x85` and (12) `0xA1` are implemented per SAT for
protocols 0 (hard reset), 1 (SRST), 3 (non-data), 4 (PIO data-in), 5 (PIO
data-out), 8 (device diagnostic), 9 (device reset) and 15 (return response
information). DMA protocols are rejected with ILLEGAL REQUEST; the ATAboy
only does PIO.

- Transfer length comes from the host's data length: whole 512-byte sectors,
  4 KiB (8 sectors) at most. T_DIR must agree with the protocol.
- The DEV bit of the device register is supplied by the firmware (selected
  master/slave); the LBA bit and head bits come from the CDB.
- With CK_COND on a non-data command, the result registers come back as
  descriptor sense (key RECOVERED ERROR, 00h/1Dh) with an ATA Status Return
  descriptor. ATA errors give ABORTED COMMAND 00h/1Dh with the descriptor;
  timeouts, protocol mismatches and bus faults give ABORTED COMMAND 3Eh/02h
  with the descriptor. Linux never retries ABORTED COMMAND for SG_IO
  pass-through, but it does retry HARDWARE ERROR on USB storage, which
  would issue the ATA command twice.
- Sense data is 22 bytes, so Linux usb-storage switches to 96-byte
  auto-sense after the first one.

### Safety policy

Every pass-through command is classified (`firmware/ata_policy.c`):

| Class | Examples | Rule |
|---|---|---|
| read | IDENTIFY, READ SECTORS/EXT, READ LONG, READ VERIFY, SMART read/self-test/status, SET FEATURES, power management, READ NATIVE MAX | always allowed |
| write | WRITE SECTORS/EXT/MULTIPLE/DMA, WRITE LOG, SMART WRITE LOG, WRITE BUFFER, any PIO data-out | needs write protect **off**, else DATA PROTECT 27h/00h |
| dangerous | WRITE LONG, FORMAT TRACK, DOWNLOAD MICROCODE, SECURITY set/unlock/erase/disable, SET MAX, TRIM, SANITIZE, WRITE UNCORRECTABLE, DCO set/restore, vendor-specific and **unknown** opcodes | needs write protect off **and** an ARM within the last 10 s (consumed by one command), else DATA PROTECT 20h/02h |

## Mass-storage behaviour

- READ10 failures return real sense instead of TinyUSB's "medium not present":
  fixed format, INFORMATION = failing LBA, bytes 8-9 = ATA status/error,
  10-11 = elapsed ms, 14 = ATA command, 15 bit0 = timed out, 18 = engine
  result, 19 = bit0 registers valid / bit1 SRST, 20-21 = sectors done.
  The key is always MEDIUM ERROR with ASC 11h (UNC, timeout as 11h/01h,
  not ready, bus fault, ABRT), 13h (AMNF) or 14h (IDNF): those are the only
  codes the Linux SCSI layer completes without retrying. Anything else, even
  HARDWARE ERROR on USB storage, makes Linux re-read a sector the drive has
  just spent seconds failing on. Failed writes use 03/0C/00.
- Mount and unmount raise UNIT ATTENTION 28h/00h on the next ordinary command.
  The vendor and pass-through opcodes don't consume it, so the host's disk
  driver still sees it. `ataboy mount` / `unmount` also ask the kernel to
  rescan.
- With `no_retry` set, the USB disk reads with READ SECTORS without retries
  (0x21), a fast first pass for imaging old drives with `ddrescue`. On an
  early-90s IBM drive an unreadable sector failed in 20-50 ms this way instead
  of 4.3 s with the drive's own retries.

## Firmware updates

`ataboy reboot` returns once the old instance has disconnected; follow it with
`ataboy wait`. Under WSL2 the re-enumerated device has to be re-attached with
usbipd: `ataboy wait --exec 'usbipd.exe attach --wsl --busid 1-3'` (or set
`ATABOY_REATTACH`) runs that command while the device is missing.

`REBOOT` with mode 1 enters the RP2350 ROM bootloader (UF2 drive `RP2350` plus
PICOBOOT). Copy a `.uf2` onto the drive, or use `picotool load`, and the ATAboy
restarts on the new firmware. `tools/flash-wsl.sh` does the whole cycle under
WSL2/usbipd. The bootloader is in mask ROM, so a bad image is always
recoverable by holding BOOTSEL while plugging in USB.

## Compatibility

- `INFO.proto` is bumped only for incompatible changes. New fields and
  sub-ops are added compatibly; clients should ignore unknown JSON fields.
- INQUIRY product revision is `api1` for this firmware, so a host can tell API
  firmware from stock (`V0.6`) by `/sys/block/sdX/device/rev`.
