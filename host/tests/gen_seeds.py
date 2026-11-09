#!/usr/bin/env python3
"""Write the fuzz seed corpus: small synthetic disk images (whatfile), API
replies and sense data, serial frames.

    python3 gen_seeds.py [CORPUS_DIR]      (default: corpus/ next to this file)
    python3 gen_seeds.py --golden [DIR]    expected whatfile output for the disk
                                           images (default: whatfile/), made by
                                           the Python reference tool ../ataboy

Disk images: everything is built byte by byte here (no mkfs, no real disks): FAT12/16/32
volumes with long names, subdirectories, deleted files and directories,
fragmented, cross-linked, looping, lost and bad cluster chains, an MBR with
primary, extended and logical partitions, a non-FAT partition and a gap.
The BPBs may claim more sectors than the file holds (reads past the end are
zeros), which keeps each image under 64 KiB while still being FAT16/FAT32 by
cluster count.  Deterministic: the same bytes on every run.
"""

import os
import struct
import sys
import zlib

SECTOR = 512


def lfn_checksum(raw11):
    s = 0
    for ch in raw11:
        s = (((s & 1) << 7) + (s >> 1) + ch) & 0xFF
    return s


def lfn_entries(name, raw11, deleted=False):
    """Long-name slots for name, in on-disk order (last part first)."""
    units = list(name.encode("utf-16-le"))
    words = [units[i] | (units[i + 1] << 8) for i in range(0, len(units), 2)]
    if len(words) % 13:
        words.append(0)
        while len(words) % 13:
            words.append(0xFFFF)
    parts = [words[i:i + 13] for i in range(0, len(words), 13)]
    chk = lfn_checksum(raw11)
    out = []
    for seq in range(len(parts), 0, -1):
        w = parts[seq - 1]
        e = bytearray(32)
        e[0] = 0xE5 if deleted else (seq | (0x40 if seq == len(parts) else 0))
        for i, at in enumerate([1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]):
            struct.pack_into("<H", e, at, w[i])
        e[11] = 0x0F
        e[13] = chk
        out.append(bytes(e))
    return out


def sfn(raw11, attr=0x20, cluster=0, size=0, nt=0, date=0x5A21, time=0x6000, fat32=False):
    e = bytearray(32)
    e[0:11] = raw11
    e[11] = attr
    e[12] = nt
    struct.pack_into("<HH", e, 22, time, date)
    struct.pack_into("<H", e, 26, cluster & 0xFFFF)
    if fat32:
        struct.pack_into("<H", e, 20, cluster >> 16)
    struct.pack_into("<I", e, 28, size)
    return bytes(e)


class Volume:
    """A FAT volume being built in memory (only the first 'keep' bytes are
    written out)."""

    def __init__(self, fat_type, total, fatsz, spc=1, rsv=1, nfats=2, rootent=64, media=0xF8,
                 label=b"TESTVOL    ", oem=b"MSWIN4.1", keep=48 * 1024):
        self.fat_type, self.spc, self.rsv, self.nfats = fat_type, spc, rsv, nfats
        self.rootent = 0 if fat_type == 32 else rootent
        self.fatsz = fatsz
        self.root_secs = (self.rootent * 32 + SECTOR - 1) // SECTOR
        self.data_start = rsv + nfats * fatsz + self.root_secs
        self.img = bytearray(keep)
        bs = bytearray(SECTOR)
        bs[0:3] = b"\xEB\x3C\x90"
        bs[3:11] = oem
        struct.pack_into("<HBHBHHBH", bs, 11, SECTOR, spc, rsv, nfats, self.rootent,
                         total if total < 65536 and fat_type != 32 else 0, media,
                         fatsz if fat_type != 32 else 0)
        struct.pack_into("<I", bs, 32, 0 if total < 65536 and fat_type != 32 else total)
        if fat_type == 32:
            struct.pack_into("<IHHIHH", bs, 36, fatsz, 0, 0, 2, 1, 6)
            bs[66] = 0x29
            bs[71:82] = label
            bs[82:90] = b"FAT32   "
        else:
            bs[38] = 0x29
            bs[43:54] = label
            bs[54:62] = b"FAT12   " if fat_type == 12 else b"FAT16   "
        bs[510:512] = b"\x55\xAA"
        self.img[0:SECTOR] = bs
        if fat_type == 32 and rsv > 6:
            self.img[6 * SECTOR:7 * SECTOR] = bs                     # backup boot sector
            self.img[SECTOR:SECTOR + 4] = b"RRaA"                    # FSInfo signature
        eoc = {12: 0xFFF, 16: 0xFFFF, 32: 0x0FFFFFFF}[fat_type]
        self.set_fat(0, (eoc & ~0xFF) | media)
        self.set_fat(1, eoc)
        self.eoc = eoc

    def put(self, off, data):
        end = min(len(self.img), off + len(data))
        if off < end:
            self.img[off:end] = data[:end - off]

    def set_fat(self, c, v):
        for copy in range(self.nfats):
            base = (self.rsv + copy * self.fatsz) * SECTOR
            if self.fat_type == 12:
                o = base + c + c // 2
                if o + 1 >= base + self.fatsz * SECTOR or o + 1 >= len(self.img):
                    continue
                cur = self.img[o] | (self.img[o + 1] << 8)
                cur = (cur & 0x000F) | (v << 4) if c & 1 else (cur & 0xF000) | v
                self.img[o:o + 2] = struct.pack("<H", cur & 0xFFFF)
            elif self.fat_type == 16:
                if (c + 1) * 2 <= self.fatsz * SECTOR:
                    self.put(base + 2 * c, struct.pack("<H", v))
            else:
                if (c + 1) * 4 <= self.fatsz * SECTOR:
                    self.put(base + 4 * c, struct.pack("<I", v))

    def chain(self, clusters, end=True):
        for a, b in zip(clusters, clusters[1:]):
            self.set_fat(a, b)
        if end:
            self.set_fat(clusters[-1], self.eoc)

    def cluster_off(self, c):
        return (self.data_start + (c - 2) * self.spc) * SECTOR

    def write_root(self, entries):
        self.put((self.rsv + self.nfats * self.fatsz) * SECTOR, b"".join(entries))

    def write_dir(self, clusters, entries):
        data = b"".join(entries)
        cb = self.spc * SECTOR
        for i, c in enumerate(clusters):
            self.put(self.cluster_off(c), data[i * cb:(i + 1) * cb])

    def fill(self, c, byte):
        self.put(self.cluster_off(c), bytes([byte]) * (self.spc * SECTOR))


def dot_entries(me, parent, fat32=False):
    return [sfn(b".          ", 0x10, me, fat32=fat32), sfn(b"..         ", 0x10, parent, fat32=fat32)]


def populate(v):
    """Common tree: labels, long names, subdirectories, deleted objects and
    broken chains.  Returns nothing; the volume is modified in place."""
    f32 = v.fat_type == 32
    root = []
    root.append(sfn(b"TESTVOL    ", 0x08))
    lname = "A long file name.txt"
    raw = b"ALONGF~1TXT"
    root += lfn_entries(lname, raw)
    root.append(sfn(raw, 0x20, 3, 1500, fat32=f32))          # clusters 3,4,5 (+ slack)
    v.chain([3, 4, 5])
    root.append(sfn(b"README     ", 0x20, 6, 100, nt=0x18, fat32=f32))   # lower-case name + ext flags
    v.chain([6])
    root.append(sfn(b"SUBDIR     ", 0x10, 7, fat32=f32))
    v.chain([7, 12])
    raw = b"DELETE~1DAT"
    root += lfn_entries("deleted file.dat", raw, deleted=True)
    root.append(sfn(b"\xE5" + raw[1:], 0x20, 20, 1100, fat32=f32))       # deleted, first byte from LFN
    root.append(sfn(b"\xE5LDDIR     ", 0x10, 24, fat32=f32))            # deleted directory
    root.append(sfn(b"FRAG    BIN", 0x20, 8, 2048, fat32=f32))
    v.chain([8, 10, 9, 11])                                   # fragmented, out of order
    root.append(sfn(b"CROSS   BIN", 0x20, 10, 512, fat32=f32))  # cross-linked into FRAG
    root.append(sfn(b"LOOP    BIN", 0x20, 13, 4096, fat32=f32))
    v.chain([13, 14], end=False)
    v.set_fat(14, 13)                                         # loop
    root.append(sfn(b"\x05SIGMA  TXT", 0x20, 15, 10, fat32=f32))   # 0x05 = 0xE5 as first char
    v.chain([15])
    root.append(sfn(b"GARBAGE?BAD", 0x20, 16, 10, fat32=f32))       # implausible: skipped
    raw = b"NOTMIN~1TXT"
    stale = lfn_entries("not mine.txt", b"OTHER   TXT")         # checksum mismatch: short name wins
    root += stale
    root.append(sfn(raw, 0x20, 17, 600, fat32=f32))
    v.chain([17, 18])
    root.append(sfn(b"EMPTY      ", 0x20, 0, 0, fat32=f32))
    root.append(sfn(b"BADTIME TXT", 0x20, 19, 1, date=0x0000, time=0xFFFF, fat32=f32))
    v.chain([19])
    if f32:
        v.chain([2])
        v.write_dir([2], root)
    else:
        v.write_root(root)
    # SUBDIR: two clusters, nested directory and a file
    sub = dot_entries(7, 0, f32)
    sub += lfn_entries("Nested Folder", b"NESTED~1   ")
    sub.append(sfn(b"NESTED~1   ", 0x10, 21, fat32=f32))
    v.chain([21])
    for i in range(20):
        sub.append(sfn(("F%07d" % i).encode() + b"TXT", 0x20, 0, 0, fat32=f32))
    sub.append(sfn(b"INSUB   DAT", 0x20, 22, 700, fat32=f32))
    v.chain([22, 23])
    v.write_dir([7, 12], sub)
    nested = dot_entries(21, 7, f32)
    nested.append(sfn(b"DEEP    TXT", 0x20, 26, 5, fat32=f32))
    v.chain([26])
    nested.append(sfn(b"SELF       ", 0x10, 21, fat32=f32))         # points back at itself
    v.write_dir([21], nested)
    # deleted directory with a deleted-parent child
    old = dot_entries(24, 0, f32)
    old.append(sfn(b"CHILD   TXT", 0x20, 25, 300, fat32=f32))
    v.write_dir([24], old)
    for c in (3, 4, 5, 6, 8, 9, 10, 11, 20, 22, 23, 25, 26):
        v.fill(c, 0x40 + c)
    v.set_fat(27, v.eoc)                                      # lost cluster
    v.set_fat(28, {12: 0xFF7, 16: 0xFFF7, 32: 0x0FFFFFF7}[v.fat_type])  # bad cluster


def fat12_floppy():
    v = Volume(12, total=2880, fatsz=9, spc=1, rsv=1, nfats=2, rootent=224, media=0xF0, keep=48 * 1024)
    populate(v)
    return bytes(v.img)


def fat16_volume(keep):
    # 20000 sectors at 1 sector/cluster: ~19900 clusters -> FAT16
    v = Volume(16, total=20000, fatsz=8, spc=1, rsv=4, nfats=2, rootent=64, keep=keep)
    populate(v)
    return v


def fat32_volume(keep):
    # 70000 clusters -> FAT32, with a FAT of 8 sectors (entries past it read as 0)
    v = Volume(32, total=70100, fatsz=8, spc=1, rsv=8, nfats=2, label=b"FAT32 TEST ", keep=keep)
    populate(v)
    return v


def mbr_entry(boot, typ, start, count):
    return struct.pack("<B3sB3sII", boot, b"\xFE\xFF\xFF", typ, b"\xFE\xFF\xFF", start, count)


def mbr_disk():
    """MBR: p1 FAT16 at 4 (gap 1-3), p2 extended with two logical drives
    (FAT12 and Linux), p3 Linux, image truncated."""
    size = 60 * 1024
    img = bytearray(size)
    mbr = bytearray(SECTOR)
    mbr[446:462] = mbr_entry(0x80, 0x06, 4, 36)               # partition smaller than its FS
    mbr[462:478] = mbr_entry(0, 0x05, 40, 30000)
    mbr[478:494] = mbr_entry(0, 0x83, 90000, 1000)
    mbr[510:512] = b"\x55\xAA"
    img[0:SECTOR] = mbr
    v = fat16_volume(keep=36 * SECTOR)
    img[4 * SECTOR:4 * SECTOR + len(v.img)] = v.img
    # EBR 1 at 40: logical FAT12 at 41, next EBR at 40 + 60
    ebr = bytearray(SECTOR)
    ebr[446:462] = mbr_entry(0, 0x01, 1, 50)
    ebr[462:478] = mbr_entry(0, 0x05, 60, 100)
    ebr[510:512] = b"\x55\xAA"
    img[40 * SECTOR:41 * SECTOR] = ebr
    small = Volume(12, total=50, fatsz=1, spc=1, rsv=1, nfats=1, rootent=16, keep=10 * SECTOR,
                   label=b"LOGICAL    ")
    small.write_root([sfn(b"HELLO   TXT", 0x20, 2, 20)])
    small.chain([2])
    img[41 * SECTOR:41 * SECTOR + len(small.img)] = small.img
    # EBR 2 at 100: Linux logical at 105, chain loops back to EBR 1
    ebr2 = bytearray(SECTOR)
    ebr2[446:462] = mbr_entry(0, 0x83, 5, 10)
    ebr2[462:478] = mbr_entry(0, 0x05, 0, 0)
    ebr2[510:512] = b"\x55\xAA"
    img[100 * SECTOR:101 * SECTOR] = ebr2
    return bytes(img)


def fat32_disk():
    img = bytearray(60 * 1024)
    mbr = bytearray(SECTOR)
    mbr[446:462] = mbr_entry(0x80, 0x0C, 2, 70100)
    mbr[462:478] = mbr_entry(0, 0x83, 70102, 10)              # Linux type holding... nothing
    mbr[510:512] = b"\x55\xAA"
    img[0:SECTOR] = mbr
    v = fat32_volume(keep=len(img) - 2 * SECTOR)
    img[2 * SECTOR:] = v.img
    return bytes(img)


def wrong_type_disk():
    """A FAT12 volume in a partition typed Linux, and a FAT-typed partition
    without a boot sector."""
    img = bytearray(16 * 1024)
    mbr = bytearray(SECTOR)
    mbr[446:462] = mbr_entry(0, 0x83, 1, 20)
    mbr[462:478] = mbr_entry(0, 0x06, 21, 8)
    mbr[510:512] = b"\x55\xAA"
    img[0:SECTOR] = mbr
    small = Volume(12, total=20, fatsz=1, spc=2, rsv=1, nfats=2, rootent=16, keep=20 * SECTOR, label=b"NO NAME    ")
    small.write_root([sfn(b"A       TXT", 0x20, 2, 1500)])
    small.chain([2, 3])
    img[SECTOR:SECTOR + 20 * SECTOR] = small.img[:20 * SECTOR]
    return bytes(img)


def reply(op, result, ctype, payload, ext=False, boot_id=0x12345678):
    hdr = bytearray(32 if ext else 16)
    hdr[0:2] = b"AB"
    hdr[2], hdr[3], hdr[4], hdr[5] = 1, op, result, ctype
    struct.pack_into(">HI", hdr, 6, len(payload), 123456)
    hdr[13] = len(hdr)
    if ext:
        struct.pack_into(">III", hdr, 16, boot_id, 7, 0xCAFEF00D)
        hdr[28] = 0x02
    return bytes(hdr) + payload


def ata_sense(key, asc, ascq, status, error, ext):
    s = bytearray(22)
    s[0], s[1], s[2], s[3], s[7] = 0x72, key, asc, ascq, 14
    s[8:22] = bytes([0x09, 0x0C, 1 if ext else 0, error, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                     0xE0, status])
    return bytes(s)


def replies():
    ident = bytearray(512)
    ident[54:94] = b"IBM-H2172 SYNTHETIC MODEL               "[:40]
    struct.pack_into("<H", ident, 98, 0x0200)
    return {
        "json_status.bin": reply(0x01, 0, 1, b'{"result": "ok", "mounted": false, "drive": {"model": "X"}}'),
        "json_ext.bin": reply(0x00, 0, 1, b'{"fw": "1.0", "proto": 1, "rid_ops": [3, 4]}', ext=True),
        "binary_read.bin": reply(0x22, 7, 2, bytes(range(32)) + bytes(64), ext=True),
        "sense_recovered.bin": ata_sense(1, 0x00, 0x1D, 0x50, 0x00, False),
        "sense_aborted_ext.bin": ata_sense(0x0B, 0x00, 0x1D, 0x51, 0x04, True),
        "sense_fixed.bin": bytes([0x70, 0, 0x07, 0, 0, 0, 0, 10, 0, 0, 0, 0, 0x27, 0x00]),
        "identify.bin": bytes(ident),
    }


def frames():
    def frame(typ, status, tag, alloc, body):
        f = struct.pack(">BBBBHHH", 0xA5, typ, 1, status, tag, alloc, len(body)) + body
        return f + struct.pack(">I", zlib.crc32(f) & 0xFFFFFFFF)
    cdb = bytes([0xC0, 0x01, ord("A"), ord("X")]) + bytes(12)
    rsp = reply(0x01, 0, 1, b'{"result": "ok"}')
    return {
        "request.bin": frame(ord("Q"), 0, 1, 0, cdb),
        "request_dataout.bin": frame(ord("Q"), 0, 2, 512, cdb + bytes(512)),
        "response_ok.bin": frame(ord("R"), 0, 1, 0, rsp),
        "response_check.bin": frame(ord("R"), 1, 3, 0, bytes([5, 0x24, 0])),
        "noise_then_response.bin": b"\xA5\xA5R\x00garbage" + frame(ord("R"), 0, 9, 0, rsp)
                                   + frame(ord("R"), 0, 10, 0, rsp)[:-3],
        "bad_crc.bin": frame(ord("Q"), 0, 4, 0, cdb)[:-1] + b"\x00",
    }


def write_all(d, files):
    os.makedirs(d, exist_ok=True)
    for name, data in files.items():
        assert len(data) < 64 * 1024, name
        with open(os.path.join(d, name), "wb") as f:
            f.write(data)


GOLDEN_EXTRA = [100000, 1 << 33, -1]


def golden_lbas(path):
    return list(range(os.path.getsize(path) // SECTOR + 2)) + GOLDEN_EXTRA


def golden(out):
    """test_fat compares the C library with these (JSON) and ctest the CLI's
    human output (.txt)."""
    import contextlib
    import importlib.machinery
    import importlib.util
    import io
    import json
    here = os.path.dirname(os.path.abspath(__file__))
    loader = importlib.machinery.SourceFileLoader("ataboy_ref", os.path.join(here, "..", "ataboy"))
    spec = importlib.util.spec_from_loader("ataboy_ref", loader)
    ab = importlib.util.module_from_spec(spec)
    loader.exec_module(ab)
    os.makedirs(out, exist_ok=True)
    corpus = os.path.join(here, "corpus", "fuzz_fat")
    for name in sorted(os.listdir(corpus)):
        cwd = os.getcwd()
        os.chdir(corpus)
        try:
            o = ab.whatfile(name, golden_lbas(name))
        finally:
            os.chdir(cwd)
        with open(os.path.join(out, name + ".json"), "w") as f:
            f.write(json.dumps(o) + "\n")
        txt = io.StringIO()
        with contextlib.redirect_stdout(txt):
            ab.human_whatfile(o)
        with open(os.path.join(out, name + ".txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write(txt.getvalue())


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--golden":
        here = os.path.dirname(os.path.abspath(__file__))
        golden(sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, "whatfile"))
        return
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "corpus")
    write_all(os.path.join(out, "fuzz_reply"), replies())
    write_all(os.path.join(out, "fuzz_frame"), frames())
    images = {
        "fat12_floppy.img": fat12_floppy(),
        "fat16_mbr_logical.img": mbr_disk(),
        "fat32_mbr.img": fat32_disk(),
        "fat16_superfloppy.img": bytes(fat16_volume(keep=40 * 1024).img),
        "wrong_types.img": wrong_type_disk(),
    }
    write_all(os.path.join(out, "fuzz_fat"), images)


if __name__ == "__main__":
    main()
