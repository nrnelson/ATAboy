# ATAboy automation API (protocol 1)

This firmware lets software control the ATAboy over USB without the serial
menu: everything the menu can do (detect, set geometry, mount, change
settings, run diagnostics) plus things it can't (raw ATA commands, READ LONG,
surface scans, a full error log, remote firmware updates).

There are three ways in. They're available whether or not a drive is
mounted; the USB disk just shows 0 bytes until one is:

| Interface | How | Use it for |
|---|---|---|
| **ATAboy API** over the **API serial port** | USB CDC serial port "ATAboy API" ([below](#api-serial-port)) | everything the API does; **no root / admin needed** |
| **ATAboy API** over SCSI | vendor opcode `0xC0` on the mass-storage interface | the same requests and replies |
| **SAT ATA PASS-THROUGH** | SCSI `0x85` (16) / `0xA1` (12) | raw ATA commands; makes `smartctl -d sat`, `hdparm`, `sg_sat_identify` work unchanged |

The `ataboy` host tool (`host/ataboy`) wraps all three: it uses the serial
port when it can open it and falls back to SCSI (`--transport
auto|serial|sgio`). The rest of this document is the wire format, for anyone
writing another client.

On Linux the SCSI opcodes are sent with the `SG_IO` ioctl on `/dev/sgN` or
`/dev/sdX`. Vendor and pass-through opcodes need `CAP_SYS_RAWIO` (root): they
are outside the kernel's whitelist for unprivileged SG_IO, and membership of
group `disk` is not enough. On Windows they need administrator rights. The
API serial port needs only read/write access to the tty.

## Quick start

```
ataboy status                       # one-call summary: drive, geometry, mount, config, errors
ataboy detect --mount               # probe + IDENTIFY + geometry (LBA if supported) + mount
ataboy config read_timeout_ms=15000 # let a failing drive finish its own retries
ataboy -j read 19969 --through-error --out sector.bin
ataboy readlong 19969 --vote 5 --image disk.img
ataboy verify 0 336260              # drive-internal READ VERIFY scan, lists bad LBAs
ataboy log                          # every failed command with ATA status/error/registers
ataboy image disk.img               # resumable imager with a ddrescue-compatible mapfile
ataboy rescue disk.img disk.img.map # retry just the failed sectors, patch image + map
ataboy whatfile disk.img --from-map disk.img.map  # which files/dirs/FAT the bad sectors hit
smartctl -d sat -a /dev/sdX         # standard tools via SAT pass-through
ataboy reboot --bootsel             # UF2 bootloader for a firmware update
```

`whatfile` works on an image file only (no device, no root). It reads FAT12/16/32
on MBR primary and logical partitions, or a bare FAT volume, and reports one line
per LBA: file (path, byte offset, whether the sector is only slack past EOF),
directory (and the entries stored in that sector), FAT copy, boot/reserved
sector, root directory, free or lost cluster, MBR/EBR/gap, or non-FAT
partition. Deleted entries are matched best effort (first cluster certain, the
rest assume the file was contiguous). LBAs come from the command line,
`--from-map` (every tried-and-failed sector of a ddrescue mapfile) or
`--from-json` (a list, or an object with `bad_lbas` such as `verify -j`
output). With `-j` the output adds a per-object summary (`files`: path ->
LBAs) and counts per kind.

`-j` / `ATABOY_JSON=1` gives JSON on stdout. Exit status: `0` ok, `1` the device
reported a failure (ATA error, bad argument, ...), `2` usage, `3` transport /
no device, `4` permission.

With more than one ATAboy attached, pick one by its USB serial number:
`ataboy list` shows them, `ataboy -s E66138935F4B2F2B status` (or
`ATABOY_SERIAL`) selects one. The serial number is the RP2350's unique board
id (16 hex digits, also INFO's `board_id`); firmware before it reported
`654321` on every unit.

## Request format (vendor opcode 0xC0)

Every request is a **16-byte CDB, data-in**. Ask for 4096 bytes; the device
returns fewer and the residue tells you how many.

| Byte | Meaning |
|---|---|
| 0 | `0xC0` |
| 1 | sub-operation (table below) |
| 2-3 | signature: `'A' 'B'` (0x41 0x42) basic request, or `'A' 'X'` (0x41 0x58) extended request (32-byte reply header, request id); any other value gets ILLEGAL REQUEST |
| 4-15 | arguments, big-endian, per sub-op. In an extended request to a state-changing op (INFO `rid_ops`), bytes 12-15 are the request id instead ([Request ids](#request-ids)) |

The allocation length must be at least 256 bytes, otherwise the command fails
with ILLEGAL REQUEST (24h/00h). Firmware without the extended header refuses
`'A' 'X'` the same way (ILLEGAL REQUEST 24h/00h, nothing run), so a client can
try an extended request first and fall back to basic ones.

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
| 13 | header length: `0` in a basic reply (16 bytes), `32` in an extended reply |
| 14-15 | reserved (0) |

An extended request (`'A' 'X'`) gets 16 more header bytes:

| Byte | Meaning |
|---|---|
| 16-19 | boot id: random, new on every power-up or reboot (RP2350 TRNG) |
| 20-23 | state counter: goes up on every state change since boot ([below](#boot-id-and-state-counter)) |
| 24-27 | request id echo (0 when none was taken) |
| 28 | flags: bit0 replayed (this is the stored reply of an earlier request with the same id; nothing ran), bit1 request id taken (the op is in `rid_ops`) |
| 29-31 | reserved (0) |

The payload follows the header (byte 16, or 32 in an extended reply). The
allocation length counts the whole reply, so an extended reply has 16 bytes
less room for payload; `max_read_sectors` is 7 either way. **Device-level outcomes come back in-band**: the SCSI
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
| 18 | `request_id_conflict` | the request id was already used for a different request ([Request ids](#request-ids)); nothing ran |

## Sub-operations

| Op | Name | Arguments (CDB byte offsets) | Reply |
|---|---|---|---|
| 00 | INFO | — | JSON: firmware identity, protocol, git hash, board id, limits, op table, features; `boot_id`, `state_counter`, `rid_ops` (ops that take a request id), `rid_cache` (replies kept), `transports` |
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
| 25 | CORRUPT | [4-7] LBA · [8] data bits to flip (1..255) · [9] ECC bytes (0 = auto) | binary result block (of the WRITE LONG). **Destructive test helper**: READ LONG, flip bits, WRITE LONG with the old ECC; needs write protect off and an ARM |
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
`read_timeout_ms`: one host READ(10) chunk can hold 8 sectors that each take
up to `read_timeout_ms`, so the host tool raises it to at least
8 × `read_timeout_ms` + 30 s whenever the timeout is set and on every
`detect` / `mount` (the timeout may come from flash or the serial menu). It
never lowers a larger value.
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

**VERIFY** issues READ VERIFY (0x40/0x41/0x42) in chunks of 256 sectors.
If a chunk fails without telling where (timeout: registers invalid), the
firmware re-verifies that chunk one sector at a time, so the reported
failing LBA is the real one. No data
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

### Testing recovery tools: CORRUPT

CORRUPT makes a damaged sector on purpose, so imaging and recovery tools can
be exercised repeatably on a scratch drive. On an early-90s IBM drive with 4
ECC bytes per sector, up to 5 scattered flipped bits were corrected silently
(no CORR bit), and 6 or more gave UNC. With read-through-error, the data then
differed from the original by exactly the flipped bits. Writing the sector
normally repairs it.

## Request ids

A state-changing request can be made run-once, so a client that lost the
reply can safely ask again. This matters on USB: very rarely a reply arrives
empty (seen about once in 500 calls right after a mount), and on the serial
port a reply can be cut off when a program is killed or a cable is pulled.

- Only in extended requests (`'A' 'X'`), only for the ops in INFO `rid_ops`:
  CONFIG_SET, CONFIG_SAVE, CONFIG_DEFAULTS, DETECT, GEOMETRY, MOUNT, UNMOUNT,
  RESET, CORRUPT, LOG_CLEAR, ARM, REBOOT. CDB bytes 12-15 carry the id
  (non-zero; 0 = no id); the op itself sees them as zero. These ops never use
  bytes 12-15 for arguments, and new state-changing ops won't either. For all
  other ops bytes 12-15 stay arguments (READ, READ_LONG and VERIFY use them):
  those ops only read, so asking again is safe anyway.
- The firmware keeps the replies to the last 4 requests that carried an id
  (in RAM; a reboot forgets them, and the boot id tells the client). The same
  16 CDB bytes again (same op, arguments and id) get the stored reply with
  extended flag bit0 *replayed*, and nothing runs. Header bytes 8-12 (uptime,
  flags) and the state counter are refreshed; the payload is what the first
  request produced.
- The same id with a different request gets `request_id_conflict` (18) and
  nothing runs. Use a fresh random id for every new request.
- A replay works across transports: an id first sent over SCSI can be
  retried on the serial port.

The host library does this automatically: every state-changing call gets a
random id, and after an empty or lost reply it sends exactly the same request
once more. Against firmware without the extended header it sends basic
requests and, as before, only re-sends the read-only ops.

## Boot id and state counter

Every extended reply carries the **boot id**, a random 32-bit value chosen at
power-up, and the **state counter**. A client that sees a different boot id
than in its previous reply knows the ATAboy rebooted (watchdog, power cycle,
REBOOT) and that everything in RAM went with it: unsaved settings, geometry,
mount, the error log, the replay store. The host library tracks the boot id
per USB serial number (`ATAboy.link_info()`, `Reply.rebooted`,
`ATAboy.on_reboot`); the CLI warns and remembers the last boot id between
runs in `~/.cache/ataboy/boot-ids.json`, so it also reports a power cycle
since the previous command.

The state counter starts at 0 at boot and goes up when anything a host would
call state changes: mount/unmount, write protect and every other setting,
geometry, the identified drive (detect, device change), and every API op that
changes state (`drive.api_gen`), even when it ends where it started. Changes
made from the setup menu or by auto-mount count too: the firmware compares a
fingerprint of that state on every reply and every 10 ms. A change undone
within one long USB command (for example toggled twice in the menu while a
10 s VERIFY runs) can be missed. A client can read the counter before and
after something and know whether anything changed in between. INFO also
reports both.

## API serial port

The ATAboy is a composite USB device: interface 0-1 the setup UI serial port
("ATAboy CDC"), interface 2 the disk ("ATAboy Storage"), and interfaces 3-4 a
second CDC ACM serial port, **"ATAboy API"**, with its own IAD (endpoints
84h, 05h, 85h). The setup UI and the disk are unchanged. bcdDevice is 0101h
on firmware with the API port, so hosts don't reuse a descriptor layout
cached for the old firmware. The baud rate and line settings are ignored.

The port carries exactly the requests of the 0xC0 vendor command and returns
exactly the bytes the SCSI path returns, framed with a length and a CRC:

**Request frame**

| Byte | Meaning |
|---|---|
| 0-1 | `A5 51` (`'Q'`) |
| 2 | frame version `1` |
| 3 | flags (0) |
| 4-5 | tag: any value; echoed in the response |
| 6-7 | allocation length, as in the SCSI command (`0` = 4096; at least 256; larger values are cut to 4096) |
| 8-9 | body length: 16 (the CDB); 16 + n with n bytes of data-out (no op takes data-out yet: refused, not run) |
| 10.. | the 16 CDB bytes, as in [Request format](#request-format-vendor-opcode-0xc0) |
| last 4 | CRC-32 (zlib/PNG polynomial, as `zlib.crc32`) of every byte before it, big-endian |

**Response frame**

| Byte | Meaning |
|---|---|
| 0-1 | `A5 52` (`'R'`) |
| 2 | frame version `1` |
| 3 | status: `0` ok, body = the API reply · `1` the SCSI path would have answered CHECK CONDITION (bad signature, allocation < 256), body = sense key, ASC, ASCQ · `2` the request failed its CRC check, **nothing ran**, send it again · `3` unsupported frame (data-out), nothing ran |
| 4-5 | tag of the request |
| 6-7 | 0 |
| 8-9 | body length (≤ 4096) |
| 10.. | body |
| last 4 | CRC-32 of every byte before it |

Rules:

- One request at a time. The firmware reads the next request only after the
  previous response has been handed to USB; a client sends, then reads until
  a response with its tag arrives. Discard responses with another tag (left
  over from an abandoned request) and any bytes before `A5 52`.
- Requests run on the same core and through the same dispatcher as the SCSI
  vendor command, between USB events: never in the middle of a mass-storage
  READ(10)/WRITE(10) (a transfer counts as finished at its completion, or
  after 500 ms without progress), never at the same time as an SCSI request,
  and sharing the IDE bus with the setup menu as before. Everything above
  (results, locks, write protect, ARM, request ids, boot id) is the same.
- A frame that stops arriving for 1 s is dropped. Opening or closing the port
  (DTR change) drops a half-received request and any response nobody has
  read; a request already received still runs.
- After REBOOT the firmware waits (up to 1 s) for the reply to leave before
  restarting; the port then disappears with the device.
- If no intact response arrives (timeout, CRC error), the request may or may
  not have run: re-send read-only ops freely and state-changing ops only with
  a request id (the host library does this). Status 2 means it did not run.

**Finding the port.** On Linux, the tty whose USB interface has the string
`ATAboy API` (`/sys/class/tty/ttyACM*/device/interface`) on a device with
VID:PID `cafe:4005`; the USB serial number (`../serial`) says which unit.
`ataboy list` shows it. The interface number is 3. Never rely on the
`ttyACMn` number. The setup UI is the port on interface 0.

**Permissions.** `/dev/ttyACM*` is usually `root:dialout 0660` (`uucp` on
Arch): add yourself to that group (`sudo usermod -aG dialout $USER`, then log
in again). Alternatively a udev rule gives the logged-in user access and a
stable name:

```
# /etc/udev/rules.d/70-ataboy.rules
SUBSYSTEM=="tty", ATTRS{idVendor}=="cafe", ATTRS{idProduct}=="4005", ENV{ID_USB_INTERFACE_NUM}=="03", \
  TAG+="uaccess", ENV{ID_MM_DEVICE_IGNORE}="1", SYMLINK+="ataboy-api-$env{ID_SERIAL_SHORT}"
```

`ID_MM_DEVICE_IGNORE` keeps ModemManager from probing the port with AT
commands (harmless, the firmware skips bytes that aren't a frame, but it
wastes time).

**Other systems.** macOS names the port `/dev/cu.usbmodem…`; the host library
works there with `--dev /dev/cu.usbmodem…` (termios), but has no automatic
discovery and no SG_IO. Windows: the port appears as a COM port with the
inbox `usbser.sys` driver (Windows 10 and later, no INF); a Windows transport
for the host library (Win32 serial API via ctypes, discovery by
`USB\VID_CAFE&PID_4005&MI_03` and the serial number) is future work.

**Throughput.** The port is full-speed USB CDC with 64-byte bulk packets:
roughly 0.5-1 MB/s of raw bandwidth, depending on how quickly the main loop
re-arms the endpoint. An API READ of 7 sectors is a ~3.6 KB reply, so
expect a few milliseconds of USB time per request plus the drive's time, in
the same range as the SCSI vendor command (which needs CBW/CSW round trips
of its own). Bulk imaging should keep using the USB disk (READ(10)), which
is unchanged.

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
- PIO data-out runs only if host data really arrived (the class buffer is
  stamped after every command; a stale buffer is refused with ILLEGAL REQUEST
  24h/00h). This guards against a host that sends a data-out CDB with a
  data-in transfer.
- While the drive is mounted, commands that would change translation, timing
  or power state underneath the host are refused with ILLEGAL REQUEST
  2Ch/00h: INITIALIZE DEVICE PARAMETERS (91h), SLEEP (E6h/99h), SET FEATURES
  set transfer mode (EFh/03h). Unmount first.

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
- Mount (and a write-protect change while mounted) raises UNIT ATTENTION
  28h/00h, delivered on the next TEST UNIT READY. TinyUSB answers most
  standard opcodes itself, so TUR is where the host's disk driver learns
  about it.
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
- The extended header, request ids and the API serial port are compatible
  additions to protocol 1: a client that sends basic `'A' 'B'` requests gets
  byte-for-byte the replies it always got (header byte 13 stays 0). Extended
  replies also carry protocol `1` in byte 2; check byte 13 for the header
  length. Firmware without them refuses `'A' 'X'` with ILLEGAL REQUEST
  24h/00h and has no "ATAboy API" port; INFO `features` lists `ext_header`,
  `request_id` and `serial_api` when present.
- INQUIRY product revision is `api1` for this firmware, so a host can tell API
  firmware from stock (`V0.6`) by `/sys/block/sdX/device/rev`.
