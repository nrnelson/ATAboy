#!/usr/bin/env python3
"""Unit tests for the host library (host/ataboy) against a simulated device.

FakeDevice models the ATAboy API firmware at the SG_IO boundary: the module's
sgio() is replaced, vendor CDBs (0xC0) are decoded and answered with replies
built byte for byte per docs/API.md (16-byte header, JSON or 32-byte ATA
result block + data).  No hardware, no root.

    python3 tests/test_host_lib.py
"""

import hashlib
import binascii
import importlib.machinery
import importlib.util
import json
import os
import socket
import struct
import sys
import contextlib
import io
import tempfile
import threading
import unittest
from unittest import mock

sys.dont_write_bytecode = True          # no __pycache__ next to host/ataboy

HERE = os.path.dirname(os.path.abspath(__file__))
HOST = os.path.join(HERE, "..", "host", "ataboy")


def load_host():
    loader = importlib.machinery.SourceFileLoader("ataboy_host", HOST)
    spec = importlib.util.spec_from_loader("ataboy_host", loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


ab = load_host()

HDR = struct.Struct(">2sBBBBHIB3x")
RES = struct.Struct(">IHHBBBBII5sBbxI")
R = {name: i for i, name in enumerate(ab.RESULTS)}
IDE = {"ok": 0, "ata_error": -1, "timeout": -2}

# Bad-sector modes
UNC = "unc"              # every read fails (UNC); READ LONG returns the raw data
NORETRY = "noretry"      # fails only without drive retries (0x21 / 0x41)
DEAD = "dead"            # reads and READ LONG fail, no data at all
TIMEOUT = "timeout"      # drive stays BSY: timeout, registers invalid


def sector(lba):
    """Deterministic contents of sector 'lba'."""
    return struct.pack(">I", lba) * 128


def ecc_of(lba, n):
    return bytes([(lba * 7 + i) & 0xFF for i in range(n)])


class FakeDevice:
    """A Python model of the API firmware, answering at the sgio() boundary."""

    def __init__(self, total=4096, ecc=4, read_timeout_ms=1000, ext=False):
        self.total = total
        self.ecc = ecc
        self.bad = {}                # lba -> mode
        self.fail_lba_override = {}  # lba -> fail_lba value the "drive" reports
        self.readlong_flaky = set()  # lbas whose READ LONG byte 0 flips on every other read
        self.verify_zero_progress = False
        self.config = {"write_protect": True, "auto_mount": False, "iordy": False, "intrq": False,
                       "read_timeout_ms": read_timeout_ms, "write_timeout_ms": 1000, "ecc_len": 0,
                       "ecc_len_effective": ecc, "no_retry": False, "device": "master"}
        self.mounted = False
        self.identified = True
        self.geometry_valid = True
        self.uptime = 1000
        self.log = []
        self.calls = []              # (op, args) of every API call
        self.readlong_count = {}
        self.raise_on = None         # callable(op, args) -> exception to raise, or None
        self.hash_records = []       # (seq, lba, source, flags, digest) of every sector read
        self.hash_newest = 0
        self.hash_capacity = 1024
        self.settings = {"load": "ok", "field": None}
        self.owner = {"model": "FAKE DRIVE", "serial": "1"}
        self.model, self.serial = "FAKE DRIVE", "1"
        self.ready = True            # BSY clear + DRDY within the mount check's 3 s
        # Extended header / request ids (firmware with "ext_header"); off = older firmware
        self.ext = ext
        self.boot_id = 0x5EED0001
        self.state_counter = 0
        self.rid_store = []          # [(rid, cdb, reply)], newest last, at most RID_CACHE
        self.replays = 0

    RID_CACHE = 4
    RID_OPS = {0x03, 0x04, 0x05, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x25, 0x31, 0x40, 0x41}

    def reboot(self):
        """Power cycle: new boot id, replay store and state gone."""
        self.boot_id += 1
        self.rid_store = []
        self.state_counter = 0
        self.mounted = False
        self.uptime = 500

    # -- transport ------------------------------------------------------------

    def sgio(self, fd, cdb, direction, data=None, length=0, timeout_ms=60000):
        assert direction == ab.SG_DXFER_FROM_DEV
        reply = self.handle(bytes(cdb), length)
        return reply, length - len(reply)

    def handle(self, cdb, length):
        """One API request, as api_dispatch() handles it: reply bytes, or
        ScsiCheckCondition for a malformed CDB."""
        sig_ok = cdb[2:4] == b"AB" or (self.ext and cdb[2:4] == b"AX")
        if len(cdb) != 16 or cdb[0] != 0xC0 or not sig_ok or length < 256:
            raise ab.ScsiCheckCondition(bytes([0x70, 0, 5, 0, 0, 0, 0, 10, 0, 0, 0, 0, 0x24, 0]))
        if cdb[3] == ord("B"):
            return self.execute(cdb[1], cdb[4:16], length)
        op, a, rid, xf = cdb[1], cdb[4:16], 0, 0
        if op in self.RID_OPS:
            rid, = struct.unpack(">I", a[8:12])
            a, xf = a[:8] + bytes(4), 2
        for r_id, r_cdb, r_reply in self.rid_store:
            if rid and r_id == rid:
                if r_cdb != cdb:
                    self.uptime += 10
                    return self.wrap(self.jreply(op, R["request_id_conflict"], message="request id reused"), rid, xf)
                self.replays += 1
                out = bytearray(r_reply)
                out[28] |= 1
                return bytes(out)
        reply = self.wrap(self.execute(op, a, length - 16), rid, xf)
        if rid:
            self.rid_store = (self.rid_store + [(rid, cdb, reply)])[-self.RID_CACHE:]
        return reply

    def wrap(self, basic, rid, xf):
        hdr = bytearray(basic[:16])
        hdr[13] = 32
        return bytes(hdr) + struct.pack(">IIIB3x", self.boot_id, self.state_counter, rid, xf) + basic[16:]

    def execute(self, op, a, length):
        self.calls.append((op, a))
        if self.raise_on:
            exc = self.raise_on(op, a)
            if exc is not None:
                raise exc
        self.uptime += 10
        if op in self.RID_OPS:
            self.state_counter += 1
        reply = self.dispatch(op, a, length)
        assert len(reply) <= length, "reply %d > allocation %d" % (len(reply), length)
        return reply

    # -- reply builders -------------------------------------------------------

    def flags(self):
        return ((1 if self.mounted else 0) | (2 if self.config["write_protect"] else 0) |
                (4 if self.identified else 0) | (16 if self.geometry_valid else 0))

    def reply(self, op, result, ctype, payload=b""):
        return HDR.pack(b"AB", 1, op, result, ctype, len(payload), self.uptime, self.flags()) + payload

    def jreply(self, op, result, obj=None, message=None):
        o = {"result": ab.RESULTS[result]}
        if message:
            o["message"] = message
        o.update(obj or {})
        return self.reply(op, result, ab.CT_JSON, json.dumps(o).encode())

    def result_block(self, lba, count, done, command, ide, status=0x50, error=0, err_data=False,
                     tf_valid=True, fail_lba=None, ecc_len=0, done32=0, partial=False):
        fl = ((0x01 | 0x08) if ide == IDE["timeout"] else 0) | (0x02 if err_data else 0) | \
             (0x04 if tf_valid else 0) | (0x40 if partial else 0)
        if ide == 0:
            fail = 0xFFFFFFFF
        else:
            fail = fail_lba if fail_lba is not None else lba + done
        tf = bytes([1, fail & 0xFF, (fail >> 8) & 0xFF, (fail >> 16) & 0xFF, 0xE0 | ((fail >> 24) & 0xF)]) \
            if tf_valid else bytes([status] * 5)
        return RES.pack(lba, min(count, 0xFFFF), min(done, 0xFFFF), status, error, command, fl,
                        1234, fail, tf, ecc_len, ide, done32)

    def record(self, op_name, cmd, lba, count, done, result, fail_lba, status, error, tf_valid, err_data):
        self.log.append({"seq": len(self.log) + 1, "t_ms": self.uptime, "op": op_name, "cmd": cmd,
                         "result": ab.RESULTS[result], "lba": lba, "count": count, "done": done,
                         "fail_lba": fail_lba, "status": status, "status_bits": ab.bits(status, ab.ST_BITS),
                         "error": error, "error_bits": ab.bits(error, ab.ER_BITS), "tf_valid": tf_valid,
                         "err_data": err_data, "recovered": False, "elapsed_ms": 1, "timeout_ms": 1000,
                         "tf": None})

    def status_json(self):
        return {"fw": "fake-api1", "uptime_ms": self.uptime, "mounted": self.mounted,
                "write_protect": self.config["write_protect"], "armed": False,
                "settings": dict(self.settings),
                "drive": {"present": True, "identified": self.identified, "forced": False,
                          "device": "master", "model": self.model, "serial": self.serial, "firmware": "1",
                          "default_chs": [0, 0, 0], "lba": True, "lba28_sectors": min(self.total, (1 << 28) - 1),
                          "lba48": self.total >= (1 << 28), "lba48_sectors": self.total,
                          "ecc_bytes": self.ecc, "ata_major": 0},
                "geometry": {"valid": self.geometry_valid, "mode": "lba", "addressing": "lba",
                             "cyls": 0, "heads": 0, "spt": 0, "sectors": self.total,
                             "owner": dict(self.owner), "identity": self.identity()},
                "bus": {"status": 0x50, "status_bits": "DRDY|DSC", "pio_faults": 0},
                "config": dict(self.config),
                "stats": {k: 0 for k in ("read_cmds", "read_sectors", "read_errors", "write_cmds",
                                         "write_sectors", "write_errors", "timeouts", "recoveries",
                                         "passthru_cmds", "passthru_errors", "passthru_blocked",
                                         "api_cmds")},
                "errors": {"total": len(self.log), "retained": len(self.log),
                                        "last": self.log[-1] if self.log else None}}

    # -- firmware model -------------------------------------------------------

    def identity(self):
        if not self.identified or not (self.owner["model"] or self.owner["serial"]):
            return "unknown"
        return "match" if (self.owner["model"], self.owner["serial"]) == (self.model, self.serial) else "mismatch"

    def mount(self, force):
        """Firmware mount checks (drive_mount_ex): owner mismatch refuses unless forced."""
        if not self.geometry_valid:
            return R["no_geometry"], "detect or set geometry first"
        if not force and not self.mounted and not self.ready:
            return R["not_ready"], "drive not ready (BSY, or no DRDY) after 3 s"
        if not force and not self.mounted and self.identity() == "mismatch":
            return R["geometry_mismatch"], "the geometry was set up for another drive"
        self.mounted = True
        return 0, None

    def fails(self, lba, no_retry):
        mode = self.bad.get(lba)
        if mode is None:
            return None
        if mode == NORETRY and not no_retry:
            return None
        return mode

    def dispatch(self, op, a, alloc):
        if op == 0x00:
            return self.jreply(op, 0, {"proto": 1, "fw": "fake-api1", "git": "0", "built": "now",
                                       "board": "fake", "board_id": "0",
                                       "max_read_sectors": (4096 - 48) // 512})
        if op in (0x01, 0x10, 0x11, 0x12, 0x13, 0x14):
            res, msg = 0, None
            if op == 0x12:
                res, msg = self.mount(bool(a[0] & 1))
            elif op == 0x13:
                self.mounted = False
            elif op == 0x10:
                if a[1] != 5:                    # any geometry but "keep": owner = this drive
                    self.owner = {"model": self.model, "serial": self.serial}
                if a[6] & 2:
                    res, msg = self.mount(bool(a[6] & 8))
            return self.jreply(op, res, self.status_json(), msg)
        if op == 0x02:
            return self.jreply(op, 0, {"config": dict(self.config)})
        if op == 0x15:
            return self.op_park(op, a)
        if op == 0x03:
            key, value = struct.unpack(">B3xI", a[:8])
            names = {v[0]: (k, v[1]) for k, v in ab.CONFIG_KEYS.items()}
            if key not in names:
                return self.jreply(op, R["bad_arg"], {"config": dict(self.config)}, "unknown config key")
            name, kind = names[key]
            if name.endswith("timeout_ms") and not 50 <= value <= 600000:
                return self.jreply(op, R["bad_arg"], {"config": dict(self.config)},
                                   "timeout must be 50..600000 ms")
            self.config[name] = bool(value) if kind == "bool" else \
                (["master", "slave"][value] if kind == "device" else value)
            return self.jreply(op, 0, {"config": dict(self.config)})
        if op == 0x22:
            return self.op_read(op, a, alloc)
        if op == 0x23:
            return self.op_read_long(op, a)
        if op == 0x24:
            return self.op_verify(op, a)
        if op == 0x26:
            return self.op_hashes(op, a, alloc)
        if op == 0x30:
            start, = struct.unpack(">I", a[:4])
            page = self.log[start:start + 5]
            nxt = start + len(page) if start + len(page) < len(self.log) else None
            return self.jreply(op, 0, {"total": len(self.log), "retained": len(self.log), "start": start,
                                       "returned": len(page), "next": nxt, "entries": page})
        return self.jreply(op, R["bad_subop"], message="unknown sub-operation")

    park_e0_aborts = False
    park_seek_fails = False

    def op_park(self, op, a):
        if self.mounted:
            return self.jreply(op, R["mounted"], self.status_json(), "unmount first")
        seek, cyl = a[0] & 1, struct.unpack(">H", a[2:4])[0]
        self.park_calls = getattr(self, "park_calls", []) + [(seek, cyl)]

        def cmd(command, ok, err=0x04):
            return {"command": command, "result": "ok" if ok else "ata_error",
                    "status": 0x50 if ok else 0x51, "status_bits": "DRDY|DSC" if ok else "DRDY|DSC|ERR",
                    "error": 0 if ok else err, "error_bits": "" if ok else "ABRT", "elapsed_ms": 5}
        o = {"seek": None, "fallback": self.park_e0_aborts}
        res = 0
        if seek:
            o["seek"] = dict(cmd(0x70, not self.park_seek_fails), cylinder=cyl)
            if self.park_seek_fails:
                res = R["ata_error"]
        o["standby"] = cmd(0x94 if self.park_e0_aborts else 0xE0, True)
        if self.park_e0_aborts:
            o.update(e0_status=0x51, e0_error=0x04)
        return self.jreply(op, res, o, "heads parked" if res == 0 else "SEEK to the landing zone failed")

    def op_read(self, op, a, alloc):
        lba, count, flags, tmo = struct.unpack(">IHBxI", a)
        maxn = (alloc - 16 - 32) // 512
        if count == 0 or count > maxn:
            return self.jreply(op, R["bad_arg"], message="count must be 1..max_read_sectors")
        if lba + count > self.total:
            return self.jreply(op, R["bad_arg"], message="beyond end of drive")
        no_retry = bool(flags & 1) or self.config["no_retry"]
        cmd = 0x21 if no_retry else 0x20
        data = b""
        for i in range(count):
            s = lba + i
            mode = self.fails(s, no_retry)
            if mode is None:
                data += sector(s)
                self.hash_sector(s, sector(s))
                continue
            if mode == TIMEOUT:
                res, ide, st, er, tfv, err_data = R["timeout"], IDE["timeout"], 0x80, 0, False, False
                fail = lba + i
            else:
                res, ide, st, er, tfv = R["ata_error"], IDE["ata_error"], 0x59, 0x40, True
                err_data = bool(flags & 2) and mode != DEAD
                fail = self.fail_lba_override.get(s, s)
                if err_data:
                    data += bytes(b ^ 0xFF for b in sector(s))     # the drive's best effort
                    self.hash_sector(s, bytes(b ^ 0xFF for b in sector(s)), flags=1)
            self.record("read_diag", cmd, lba, count, i, res, fail, st, er, tfv, err_data)
            blk = self.result_block(lba, count, i, cmd, ide, st, er, err_data, tfv, fail)
            return self.reply(op, res, ab.CT_BINARY, blk + data)
        blk = self.result_block(lba, count, count, cmd, 0)
        return self.reply(op, 0, ab.CT_BINARY, blk + data)

    def hash_sector(self, lba, data, source=2, flags=0):
        self.hash_newest += 1
        self.hash_records.append((self.hash_newest, lba, source, flags, hashlib.sha256(data).digest()))
        del self.hash_records[:-self.hash_capacity]

    def op_hashes(self, op, a, alloc):
        """HASHES (0x26): 32-byte header + 44-byte records, as docs/API.md."""
        start, flags = struct.unpack(">IB", a[:5])
        if flags & 1:
            self.hash_records, self.hash_newest = [], 0
        oldest = self.hash_records[0][0] if self.hash_records else 0
        start = max(start, oldest)
        fit = (alloc - 16 - 32) // 44
        recs = [r for r in self.hash_records if r[0] >= start][:fit]
        nxt = recs[-1][0] + 1 if recs else (start if start > self.hash_newest else self.hash_newest + 1)
        body = b"".join(struct.pack(">IIBBxx32s", *r) for r in recs)
        busy = 10 * self.hash_newest
        hdr = struct.pack(">IIIHHHBxQ4x", self.hash_newest, oldest, nxt, len(recs), 44,
                          self.hash_capacity, 1, busy)
        return self.reply(op, 0, ab.CT_BINARY, hdr + body)

    def op_read_long(self, op, a):
        lba, ecc, flags, tmo = struct.unpack(">IBxBxI", a)
        ecc = ecc or self.ecc
        if lba >= self.total:
            return self.jreply(op, R["bad_arg"], message="beyond end of drive")
        if lba >= 1 << 28:
            return self.jreply(op, R["bad_arg"], message="READ LONG needs a CHS/LBA28 address")
        cmd = 0x23 if flags & 1 else 0x22
        mode = self.bad.get(lba)
        if mode in (DEAD, TIMEOUT):
            res, ide = (R["timeout"], IDE["timeout"]) if mode == TIMEOUT else (R["ata_error"], IDE["ata_error"])
            self.record("read_long", cmd, lba, 1, 0, res, lba, 0x51, 0x10, mode != TIMEOUT, False)
            blk = self.result_block(lba, 1, 0, cmd, ide, 0x51, 0x10, False, mode != TIMEOUT, lba, ecc)
            return self.reply(op, res, ab.CT_BINARY, blk)
        n = self.readlong_count[lba] = self.readlong_count.get(lba, 0) + 1
        data = bytearray(sector(lba))
        if lba in self.readlong_flaky and n % 2 == 0:
            data[0] ^= 0x55
        blk = self.result_block(lba, 1, 1, cmd, 0, ecc_len=ecc)
        return self.reply(op, 0, ab.CT_BINARY, blk + bytes(data) + ecc_of(lba, ecc))

    def op_verify(self, op, a):
        """Firmware 3a9ee69+: a chunk that fails without valid registers (timeout)
        is re-verified sector by sector, so fail_lba is the real bad sector."""
        lba, count, tmo = struct.unpack(">III", a)
        if count == 0:
            return self.jreply(op, R["bad_arg"], message="count must be > 0")
        if lba + count > self.total:
            return self.jreply(op, R["bad_arg"], message="beyond end of drive")
        cmd = 0x41 if self.config["no_retry"] else 0x40
        if self.verify_zero_progress:
            blk = self.result_block(lba, count, 0, cmd, 0, done32=0)
            return self.reply(op, 0, ab.CT_BINARY, blk)
        done = 0
        while done < count:
            n = min(256, count - done)
            bad = [s for s in range(lba + done, lba + done + n) if self.fails(s, self.config["no_retry"])]
            if not bad:
                done += n
                continue
            b = bad[0]
            if self.bad[b] == TIMEOUT:
                res, ide, st, er, tfv = R["timeout"], IDE["timeout"], 0x80, 0, False
            else:
                res, ide, st, er, tfv = R["ata_error"], IDE["ata_error"], 0x51, 0x40, True
            done = b - lba
            self.record("verify", cmd, lba + done, n, 0, res, lba + done, st, er, tfv, False)
            blk = self.result_block(lba, count, done, cmd, ide, st, er, False, tfv, lba + done, done32=done)
            return self.reply(op, res, ab.CT_BINARY, blk)
        blk = self.result_block(lba, count, done, cmd, 0, done32=done)
        return self.reply(op, 0, ab.CT_BINARY, blk)


def make_dev(fake, info_dev=None, port=None):
    dev = object.__new__(ab.ATAboy)
    dev._init_state()
    dev.fd = -1
    dev.info_dev = info_dev
    dev.path = "/dev/fake"
    if port is not None:
        dev.port, dev.transport = port, "serial"
    return dev


class HostTestCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        self.fake = FakeDevice()
        self._orig_sgio = ab.sgio
        ab.sgio = self.fake.sgio
        self.dev = make_dev(self.fake)

    def tearDown(self):
        ab.sgio = self._orig_sgio
        self.tmp.cleanup()

    def path(self, name):
        return os.path.join(self.dir, name)

    def write_map(self, name, blocks, header=("# Mapfile. Created by GNU ddrescue version 1.27",
                                             "# current_pos  current_status  current_pass")):
        p = self.path(name)
        with open(p, "w") as f:
            for h in header:
                f.write(h + "\n")
            f.write("0x00000000     +               1\n")
            f.write("#      pos        size  status\n")
            for pos, size, st in blocks:
                f.write("0x%08X  0x%08X  %s\n" % (pos, size, st))
        return p

    def blank_image(self, name, sectors):
        p = self.path(name)
        with open(p, "wb") as f:
            f.write(bytes(sectors * 512))
        return p


# ---------------------------------------------------------------------------

class TestDecoding(unittest.TestCase):
    def test_reply_json(self):
        payload = b'{"result":"bad_arg","message":"nope"}'
        raw = HDR.pack(b"AB", 1, 0x22, 2, 1, len(payload), 4242, 0x13) + payload + b"\0" * 7
        r = ab.Reply(raw)
        self.assertEqual((r.proto, r.op, r.result, r.ctype, r.uptime_ms), (1, 0x22, 2, 1, 4242))
        self.assertFalse(r.ok)
        self.assertEqual(r.result_name, "bad_arg")
        self.assertEqual(r.json["message"], "nope")
        self.assertEqual(r.flag_dict, {"mounted": True, "write_protect": True, "identified": False,
                                       "armed": False, "geometry_valid": True})
        self.assertEqual(str(ab.ApiError(r)), "bad_arg: nope")
        self.assertEqual(ab.Reply(HDR.pack(b"AB", 1, 0, 99, 0, 0, 0, 0)).result_name, "result_99")

    def test_reply_errors(self):
        with self.assertRaises(ab.TransportError):
            ab.Reply(b"AB\x01")
        with self.assertRaises(ab.TransportError):
            ab.Reply(HDR.pack(b"XY", 1, 0, 0, 0, 0, 0, 0))
        with self.assertRaises(ab.TransportError):              # truncated payload
            ab.Reply(HDR.pack(b"AB", 1, 0, 0, 2, 100, 0, 0) + b"x" * 10)
        with self.assertRaises(ab.TransportError):              # malformed JSON
            ab.Reply(HDR.pack(b"AB", 1, 0, 0, 1, 3, 0, 0) + b"{x}")

    def test_local_reply_for_not_mounted(self):
        r = ab._local_reply(ab.OPS["status"], ab.RESULTS.index("not_mounted"), "mount first")
        self.assertEqual(r.result_name, "not_mounted")
        self.assertEqual(r.json["message"], "mount first")

    def test_ata_result(self):
        tf = bytes([1, 2, 3, 4, 0xE0])
        blk = RES.pack(19968, 8, 1, 0x59, 0x40, 0x20, 0x02 | 0x04, 4290000, 19969, tf, 0, -1, 0)
        r = ab.AtaResult(blk + sector(19968) + sector(19969), R["ata_error"])
        self.assertEqual((r.lba, r.count, r.done, r.status, r.error, r.command), (19968, 8, 1, 0x59, 0x40, 0x20))
        self.assertTrue(r.err_data and r.tf_valid)
        self.assertFalse(r.timed_out or r.recovered or r.partial or r.ok)
        self.assertEqual(r.fail_lba, 19969)
        self.assertEqual(r.ide_result, "ata_error")
        self.assertEqual(r.result, "ata_error")
        self.assertEqual(len(r.data), 1024)
        d = r.to_dict()
        self.assertEqual(d["status_bits"], "DRDY|DSC|DRQ|ERR")
        self.assertEqual(d["error_bits"], "UNC")
        self.assertEqual(d["tf"], list(tf))
        self.assertEqual(d["elapsed_ms"], 4290.0)
        ok = ab.AtaResult(RES.pack(0, 1, 1, 0x50, 0, 0x20, 0x04, 5, 0xFFFFFFFF, tf, 0, 0, 0), 0)
        self.assertIsNone(ok.fail_lba)
        self.assertTrue(ok.ok)
        with self.assertRaises(ab.TransportError):
            ab.AtaResult(b"\0" * 20, 0)

    def test_ata_result_verify_count(self):
        blk = RES.pack(0, 0xFFFF, 0xFFFF, 0x50, 0, 0x40, 0x40, 5, 0xFFFFFFFF, bytes(5), 0, 0, 300000)
        self.assertEqual(ab.AtaResult(blk, 0).done, 300000)
        blk0 = RES.pack(0, 0xFFFF, 0xFFFF, 0, 0, 0, 0, 5, 70000, bytes(5), 0, -5, 70000)
        self.assertEqual(ab.AtaResult(blk0, R["protocol"]).done, 0xFFFF)
        self.assertEqual(ab.AtaResult(blk0, R["protocol"], verify=True).done, 70000)

    def test_fake_device_byte_layout(self):
        f = FakeDevice()
        cdb = bytes([0xC0, 0x22, 0x41, 0x42]) + struct.pack(">IHBxI", 5, 2, 0, 0)
        raw, resid = f.sgio(-1, cdb, ab.SG_DXFER_FROM_DEV, length=4096)
        self.assertEqual(raw[:2], b"AB")
        self.assertEqual(raw[5], 2)                             # binary
        self.assertEqual(struct.unpack(">H", raw[6:8])[0], 32 + 1024)
        self.assertEqual(raw[16:20], struct.pack(">I", 5))
        self.assertEqual(raw[16 + 32:], sector(5) + sector(6))
        self.assertEqual(resid, 4096 - len(raw))


class TestReadRange(HostTestCase):
    def test_clean_read(self):
        out = self.path("out.bin")
        res = self.dev.read_range(10, 20, out=out)
        self.assertEqual(res["result"], "ok")
        self.assertEqual(res["ok_sectors"], 20)
        self.assertIsNone(res["next_lba"])
        with open(out, "rb") as f:
            self.assertEqual(f.read(), b"".join(sector(s) for s in range(10, 30)))

    def test_bad_sector_stop(self):
        self.fake.bad[15] = UNC
        out = self.path("out.bin")
        res = self.dev.read_range(10, 20, out=out)
        self.assertEqual(res["bad_lbas"], [15])
        self.assertEqual(res["next_lba"], 15)
        self.assertEqual(res["ok_sectors"], 5)
        with open(out, "rb") as f:
            self.assertEqual(f.read(), b"".join(sector(s) for s in range(10, 15)))

    def test_bad_sector_keep_going_image(self):
        self.fake.bad[15] = UNC
        self.fake.bad[17] = DEAD
        img = self.blank_image("img.bin", 40)
        res = self.dev.read_range(10, 20, out=img, image=True, keep_going=True, through_error=True)
        self.assertEqual(res["bad_lbas"], [15, 17])
        self.assertEqual(res["ok_sectors"], 18)
        self.assertIsNone(res["next_lba"])
        with open(img, "rb") as f:
            data = f.read()
        self.assertEqual(len(data), 40 * 512)
        for s in range(40):
            got = data[s * 512:(s + 1) * 512]
            if s == 15:
                self.assertEqual(got, bytes(b ^ 0xFF for b in sector(15)))     # read-through-error data
            elif s == 17 or not 10 <= s < 30:
                self.assertEqual(got, bytes(512))
            else:
                self.assertEqual(got, sector(s), "sector %d" % s)

    def test_keep_data(self):
        self.fake.bad[12] = UNC
        res = self.dev.read_range(10, 4, keep_going=True, keep_data=True)
        self.assertEqual(res["data"], sector(10) + sector(11) + sector(13))

    def test_fail_lba_clamped(self):
        self.fake.bad[15] = UNC
        self.fake.fail_lba_override[15] = 999          # garbage from the registers
        res = self.dev.read_range(10, 20, keep_going=True)
        self.assertEqual(res["bad_lbas"], [15])
        self.assertEqual(res["ok_sectors"], 19)
        self.fake.fail_lba_override[15] = 3            # before the request
        res = self.dev.read_range(10, 20)
        self.assertEqual(res["bad_lbas"], [15])
        self.assertEqual(res["next_lba"], 15)

    def test_timeout_without_registers(self):
        self.fake.bad[11] = TIMEOUT
        res = self.dev.read_range(10, 5, keep_going=True)
        self.assertEqual(res["bad_lbas"], [11])
        self.assertTrue(any(c["timed_out"] for c in res["chunks"]))


class TestScan(HostTestCase):
    def test_bad_list(self):
        self.fake.bad.update({5: UNC, 300: DEAD, 301: UNC, 1000: NORETRY})
        res = self.dev.scan(0, 2000)
        self.assertEqual(res["bad_lbas"], [5, 300, 301])
        self.assertTrue(res["complete"])
        self.assertEqual(res["scanned"], 2000)
        self.assertEqual(res["result"], "errors")
        self.fake.config["no_retry"] = True
        self.assertEqual(self.dev.scan(900, 200)["bad_lbas"], [1000])

    def test_max_errors(self):
        self.fake.bad.update({5: UNC, 6: UNC, 7: UNC})
        res = self.dev.scan(0, 100, max_errors=2)
        self.assertEqual(res["bad_lbas"], [5, 6])
        self.assertEqual(res["next_lba"], 7)

    def test_timeout_reports_real_sector(self):
        # The firmware localises a timed-out chunk sector by sector, so only the
        # slow sector itself is reported, not everything from the chunk start.
        self.fake.bad[700] = TIMEOUT
        res = self.dev.scan(0, 1000)
        self.assertEqual(res["bad_lbas"], [700])
        self.assertTrue(res["complete"])
        self.assertEqual(res["scanned"], 1000)

    def test_zero_progress_does_not_hang(self):
        self.fake.verify_zero_progress = True
        res = self.dev.scan(0, 1000)
        self.assertEqual(res["scanned"], 0)
        self.assertEqual(res["next_lba"], 0)
        self.assertFalse(res["complete"])
        self.assertEqual(res["requests"], 1)


class TestMapfile(HostTestCase):
    def test_round_trip_mark_merge(self):
        p = self.write_map("m.map", [(0, 0x2000, "+"), (0x2000, 0x400, "-"), (0x2400, 0x200, "*"),
                                     (0x2600, 0x200, "/"), (0x2800, 0x800, "?"), (0x3000, 0x1000, "+")])
        m = ab.Mapfile.load(p)
        self.assertEqual(m.header[0], "# Mapfile. Created by GNU ddrescue version 1.27")
        self.assertEqual(m.bad_sectors(), [16, 17, 18, 19])
        self.assertEqual(m.bad_sectors(include_untried=True), [16, 17, 18, 19, 20, 21, 22, 23])
        self.assertEqual(m.bad_sectors(limit=2), [16, 17])
        self.assertEqual(m.status_at(20), "?")
        self.assertIsNone(m.status_at(10000))
        m.mark(16, 1, "+")                      # merges with the '+' block before it
        m.mark(17, 1, "+")
        self.assertEqual(m.blocks[0], [0, 0x2400, "+"])
        m.mark(21, 1, "-")                      # splits '?'
        m.mark(18, 2, "+")
        m.mark(20, 4, "+")                      # everything recovered: one block
        self.assertEqual(m.blocks, [[0, 0x4000, "+"]])
        m.save()
        m2 = ab.Mapfile.load(p)
        self.assertEqual(m2.blocks, [[0, 0x4000, "+"]])
        self.assertEqual(m2.header, m.header)
        self.assertEqual(m2.cur_line, m.cur_line)
        self.assertEqual(m2.summary(), {"+": 0x4000})
        self.assertFalse(os.path.exists(p + ".tmp"))

    def test_split_middle(self):
        p = self.write_map("m.map", [(0, 0x1000, "?")])
        m = ab.Mapfile.load(p)
        m.mark(3, 2, "-")
        self.assertEqual(m.blocks, [[0, 0x600, "?"], [0x600, 0x400, "-"], [0xA00, 0x600, "?"]])
        m.save()
        self.assertEqual(ab.Mapfile.load(p).blocks, m.blocks)

    def test_not_a_mapfile(self):
        p = self.path("x.map")
        with open(p, "w") as f:
            f.write("# only comments\n")
        with self.assertRaises(ValueError):
            ab.Mapfile.load(p)


class TestRescue(HostTestCase):
    SECTORS = 64

    def setUp(self):
        super().setUp()
        self.img = self.blank_image("disk.img", self.SECTORS)
        # sectors 0..9 good in the image; everything from 10 on was tried and failed
        with open(self.img, "r+b") as f:
            f.write(b"".join(sector(s) for s in range(10)))
        self.map = self.write_map("disk.map", [(0, 10 * 512, "+"), (10 * 512, 4 * 512, "-"),
                                               (14 * 512, (self.SECTORS - 14) * 512, "+")])

    def image_sector(self, lba):
        with open(self.img, "rb") as f:
            f.seek(lba * 512)
            return f.read(512)

    def test_all_passes(self):
        # 10 recovers in pass 1, 11 only with retries, 12 only via READ LONG, 13 never
        self.fake.bad.update({11: NORETRY, 12: UNC, 13: DEAD})
        self.fake.readlong_flaky.add(12)
        res = self.dev.rescue(self.img, self.map, votes=5)
        self.assertEqual(res["candidates"], 4)
        self.assertEqual(res["recovered_no_retry"], [10])
        self.assertEqual(res["recovered_retry"], [11])
        self.assertEqual([e["lba"] for e in res["readlong"]], [12])
        self.assertEqual(res["still_bad"], [12, 13])
        self.assertEqual(res["result"], "partial")
        self.assertTrue(res["complete"])
        self.assertEqual(res["recovered"], 2)
        for s in (10, 11, 12):
            self.assertEqual(self.image_sector(s), sector(s))
        self.assertEqual(self.image_sector(13), bytes(512))
        m = ab.Mapfile.load(self.map)
        self.assertEqual(m.blocks, [[0, 12 * 512, "+"], [12 * 512, 2 * 512, "-"],
                                    [14 * 512, (self.SECTORS - 14) * 512, "+"]])
        with open(self.img + ".readlong.json") as f:
            side = json.load(f)
        self.assertEqual(list(side), ["12"])
        self.assertEqual(side["12"]["ecc_hex"], ecc_of(12, 4).hex())
        self.assertEqual(side["12"]["votes"], 5)
        self.assertEqual(side["12"]["unstable_bytes"], 1)
        self.assertEqual(self.fake.readlong_count[12], 5)
        # the no-retry pass really used 0x21, the retry pass 0x20
        reads = [struct.unpack(">IHBxI", a) for op, a in self.fake.calls if op == 0x22]
        self.assertIn((11, 1, 1, 5000), reads)
        self.assertIn((11, 1, 0, 15000), reads)

    def test_no_readlong(self):
        self.fake.bad.update({10: UNC, 11: UNC, 12: UNC, 13: UNC})
        res = self.dev.rescue(self.img, self.map, readlong=False)
        self.assertEqual(res["still_bad"], [10, 11, 12, 13])
        self.assertFalse(os.path.exists(self.img + ".readlong.json"))
        self.assertNotIn(0x23, [op for op, _ in self.fake.calls])

    def test_sidecar_merges_existing(self):
        with open(self.img + ".readlong.json", "w") as f:
            json.dump({"40": {"lba": 40, "ecc_hex": "00"}}, f)
        self.fake.bad.update({10: UNC})
        self.dev.rescue(self.img, self.map, votes=1)
        with open(self.img + ".readlong.json") as f:
            self.assertEqual(sorted(json.load(f)), ["10", "40"])

    def test_map_checkpoints_during_a_pass(self):
        # interval 0: the map is saved after every recovered sector, so a crash
        # mid-pass (simulated by reading the map from inside the device) loses nothing
        seen = []

        def peek(op, a):
            if op == 0x22 and struct.unpack(">I", a[0:4])[0] == 11:
                seen.append(ab.Mapfile.load(self.map).status_at(10))
            return None
        self.fake.raise_on = peek
        res = self.dev.rescue(self.img, self.map, readlong=False, save_interval_s=0)
        self.assertEqual(seen, ["+"])                    # sector 10 already on disk as finished
        self.assertGreaterEqual(res["map_saves"], 4)

    def test_default_interval_does_not_save_per_sector(self):
        res = self.dev.rescue(self.img, self.map, readlong=False)
        self.assertEqual(res["map_saves"], 3)            # end of pass 1, pass 2, final

    def test_ctrl_c_saves_and_reports(self):
        self.fake.bad.update({11: NORETRY, 12: NORETRY, 13: NORETRY})

        def stop(op, a):
            if op == 0x22 and not a[6] & 1:             # first retry-pass read
                return KeyboardInterrupt()
            return None
        self.fake.raise_on = stop
        res = self.dev.rescue(self.img, self.map, readlong=False)
        self.assertEqual(res["result"], "interrupted")
        self.assertFalse(res["complete"])
        self.assertEqual(res["recovered_no_retry"], [10])
        m = ab.Mapfile.load(self.map)
        self.assertEqual(m.status_at(10), "+")
        self.assertEqual(self.image_sector(10), sector(10))

    def test_exception_mid_run_saves_map(self):
        self.fake.bad.update({11: NORETRY, 12: NORETRY, 13: NORETRY})

        def boom(op, a):
            if op == 0x22 and not a[6] & 1:             # first retry-pass read
                return ab.TransportError("device went away")
            return None
        self.fake.raise_on = boom
        with self.assertRaises(ab.TransportError):
            self.dev.rescue(self.img, self.map)
        m = ab.Mapfile.load(self.map)
        self.assertEqual(m.status_at(10), "+")           # pass-1 recovery is in the map
        self.assertEqual(m.status_at(11), "-")
        self.assertEqual(self.image_sector(10), sector(10))

    def test_exception_in_readlong_pass_saves_sidecar(self):
        self.fake.bad.update({10: UNC, 11: UNC, 12: UNC, 13: UNC})
        seen = []

        def boom(op, a):
            if op == 0x23:
                lba = struct.unpack(">I", a[:4])[0]
                if lba == 12:
                    return ab.ScsiCheckCondition(bytes([0x70, 0, 4] + [0] * 15))
                seen.append(lba)
            return None
        self.fake.raise_on = boom
        with self.assertRaises(ab.ScsiCheckCondition):
            self.dev.rescue(self.img, self.map, votes=1)
        with open(self.img + ".readlong.json") as f:
            self.assertEqual(sorted(json.load(f)), ["10", "11"])
        self.assertTrue(os.path.exists(self.map))
        self.assertEqual(ab.Mapfile.load(self.map).bad_sectors(), [10, 11, 12, 13])

    def test_api_error_per_sector(self):
        # The drive is shorter than the map claims: LBAs 11..13 are beyond its
        # end, so every read of them is a JSON bad_arg.  The run must go on.
        self.fake.total = 11
        res = self.dev.rescue(self.img, self.map)
        self.assertEqual(res["recovered_no_retry"], [10])
        self.assertEqual(res["still_bad"], [11, 12, 13])
        self.assertEqual(sorted({e["lba"] for e in res["errors"]}), [11, 12, 13])
        self.assertEqual(sorted({e["pass"] for e in res["errors"]}), ["no-retry", "readlong", "retry"])
        self.assertTrue(all("beyond end of drive" in e["error"] for e in res["errors"]))
        m = ab.Mapfile.load(self.map)
        self.assertEqual(m.status_at(10), "+")
        self.assertEqual(m.bad_sectors(), [11, 12, 13])

    def test_readlong_skipped_above_lba28(self):
        big = (1 << 28) + 5
        self.fake.total = big + 100
        self.fake.bad[big] = UNC
        img = self.blank_image("big.img", 1)
        mp = self.write_map("big.map", [(0, big * 512, "+"), (big * 512, 512, "-")])
        res = self.dev.rescue(img, mp)
        self.assertEqual(res["still_bad"], [big])
        self.assertEqual(res["readlong_skipped"][0]["lba"], big)
        self.assertNotIn(0x23, [op for op, _ in self.fake.calls])
        self.assertEqual(res["errors"], [])

    def test_untried_marked_bad_after_failure(self):
        mp = self.write_map("u.map", [(0, 10 * 512, "+"), (10 * 512, 2 * 512, "?"),
                                      (12 * 512, (self.SECTORS - 12) * 512, "+")])
        self.fake.bad[11] = DEAD
        res = self.dev.rescue(self.img, mp, include_untried=True)
        self.assertEqual(res["recovered_no_retry"], [10])
        self.assertEqual(res["still_bad"], [11])
        m = ab.Mapfile.load(mp)
        self.assertEqual(m.status_at(10), "+")
        self.assertEqual(m.status_at(11), "-")
        self.assertEqual(ab.Mapfile.load(mp).bad_sectors(), [11])

    def test_time_budget(self):
        self.fake.bad.update({10: UNC, 11: UNC})
        res = self.dev.rescue(self.img, self.map, time_budget_s=1e-9)
        self.assertFalse(res["complete"])
        self.assertEqual(res["still_bad"], [10, 11, 12, 13])
        self.assertEqual([op for op, _ in self.fake.calls if op in (0x22, 0x23)], [])


class TestKernelTimeout(HostTestCase):
    def setUp(self):
        super().setUp()
        self.sysfs = self.path("sysfs")
        os.mkdir(self.sysfs)
        self.set_timeout(30)
        self.dev.info_dev = {"sysfs": self.sysfs, "block": "/dev/sdzz", "sg": None, "rev": "api1",
                             "scsi_timeout_s": 30}

    def set_timeout(self, v):
        with open(os.path.join(self.sysfs, "timeout"), "w") as f:
            f.write("%d\n" % v)

    def timeout(self):
        with open(os.path.join(self.sysfs, "timeout")) as f:
            return int(f.read())

    def test_needed(self):
        self.assertEqual(ab.ATAboy.kernel_timeout_needed(15000), 150)
        self.assertEqual(ab.ATAboy.kernel_timeout_needed(1000), 38)
        self.assertEqual(ab.ATAboy.kernel_timeout_needed(50), 31)

    def test_set_config_raises(self):
        self.dev.set_config("read_timeout_ms", "15000")
        self.assertEqual(self.fake.config["read_timeout_ms"], 15000)
        self.assertEqual(self.timeout(), 150)
        self.assertEqual(self.dev.info_dev["scsi_timeout_s"], 150)

    def test_never_lowers(self):
        self.set_timeout(500)
        self.assertEqual(self.dev.ensure_kernel_timeout(15000), 500)
        self.assertEqual(self.timeout(), 500)

    def test_detect_and_mount_sync(self):
        self.fake.config["read_timeout_ms"] = 20000          # e.g. loaded from flash
        self.dev.detect()
        self.assertEqual(self.timeout(), 190)
        self.fake.config["read_timeout_ms"] = 60000          # e.g. changed in the serial menu
        self.dev.mount(rescan=False)
        self.assertEqual(self.timeout(), 510)
        self.fake.config["read_timeout_ms"] = 1000
        self.dev.mount(rescan=False)
        self.assertEqual(self.timeout(), 510)

    def test_no_info_dev(self):
        self.dev.info_dev = None
        self.assertIsNone(self.dev.ensure_kernel_timeout(15000))
        self.dev.detect()                                    # must not fail


class TestMisc(HostTestCase):
    def test_whatfile_lbas(self):
        p = self.path("l.json")
        with open(p, "w") as f:
            json.dump([1, "0x10", {"lba": 5}], f)
        self.assertEqual(ab.whatfile_lbas([7], json_file=p), [7, 1, 16, 5])
        with open(p, "w") as f:
            json.dump({"result": "errors", "bad_lbas": [300, 301]}, f)
        self.assertEqual(ab.whatfile_lbas(json_file=p), [300, 301])
        mp = self.write_map("w.map", [(0, 0x400, "+"), (0x400, 0x200, "-"), (0x600, 0x200, "?")])
        self.assertEqual(ab.whatfile_lbas(mapfile=mp, json_file=p), [2, 300, 301])
        with open(p, "w") as f:
            json.dump({"bad_lbas": 5}, f)
        with self.assertRaises(ValueError):
            ab.whatfile_lbas(json_file=p)

    def test_read_long_vote(self):
        self.fake.readlong_flaky.add(3)
        payload, info = self.dev.read_long_vote(3, votes=3)
        self.assertEqual(payload, sector(3) + ecc_of(3, 4))
        self.assertEqual(info["vote"], {"reads": 3, "unstable_bytes": 1})
        self.fake.bad[4] = DEAD
        payload, info = self.dev.read_long_vote(4, votes=3)
        self.assertIsNone(payload)
        self.assertEqual(info["result"], "ata_error")

    def test_log_paging(self):
        self.fake.bad[1] = UNC
        for _ in range(12):
            self.dev.read(1)
        j = self.dev.log()
        self.assertEqual(j["retained"], 12)
        self.assertEqual([e["seq"] for e in j["entries"]], list(range(1, 13)))

    def test_read_bad_arg_raises(self):
        with self.assertRaises(ab.ApiError) as cm:
            self.dev.read(self.fake.total, 1)
        self.assertEqual(cm.exception.reply.result_name, "bad_arg")

    def test_image_not_mounted_reads_through_api(self):
        # Not mounted (or no access to /dev/sdX, e.g. serial transport as a
        # normal user): image falls back to API READs, 7 sectors at a time.
        self.dev.info_dev = {"sysfs": self.dir, "block": "/dev/sdzz", "sg": None, "rev": "api1",
                             "scsi_timeout_s": 30}
        self.fake.bad[20] = UNC
        out = self.path("x.img")
        res = self.dev.image(out, count=30)
        self.assertEqual(res["via"], "api")
        self.assertEqual(res["bad_lbas"], [20])
        self.assertEqual(res["imaged_sectors"], 29)
        with open(out, "rb") as f:
            for lba in range(30):
                f.seek(lba * 512)
                self.assertEqual(f.read(512), bytes(512) if lba == 20 else sector(lba))
        reads = [struct.unpack(">IH", a[:6]) for op, a in self.fake.calls if op == ab.OPS["read"]]
        self.assertTrue(all(n <= 7 for _, n in reads))
        self.assertEqual(ab.Mapfile.load(out + ".map").status_at(20 * 512 // 512), "-")

    def test_image_without_geometry_is_api_error(self):
        self.fake.geometry_valid = False
        with self.assertRaises(ab.ApiError) as cm:
            self.dev.image(self.path("x.img"))
        self.assertEqual(cm.exception.reply.result_name, "no_geometry")



class TestHashes(HostTestCase):
    def test_records_match_data(self):
        self.dev.read(10, 4)
        j = self.dev.hashes()
        self.assertTrue(j["active"])
        self.assertEqual(j["newest"], 4)
        self.assertEqual([x["lba"] for x in j["records"]], [10, 11, 12, 13])
        self.assertEqual([x["seq"] for x in j["records"]], [1, 2, 3, 4])
        for x in j["records"]:
            self.assertEqual(x["sha256"], hashlib.sha256(sector(x["lba"])).hexdigest())
            self.assertEqual(x["source"], "api_read")
            self.assertFalse(x["err_data"])
        self.assertEqual(j["us_per_sector"], 10.0)

    def test_paging_and_filters(self):
        for lba in range(0, 300, 7):
            self.dev.read(lba, 7)               # 301 records: several 92-record pages
        j = self.dev.hashes()
        self.assertEqual(len(j["records"]), 301)
        self.assertEqual([x["seq"] for x in j["records"]], list(range(1, 302)))
        pages = sum(1 for op, _ in self.fake.calls if op == ab.OPS["hashes"])
        self.assertEqual(pages, 4)
        self.assertEqual(len(self.dev.hashes(since=290)["records"]), 12)
        only = self.dev.hashes(lba=20, count=3)["records"]
        self.assertEqual([x["lba"] for x in only], [20, 21, 22])

    def test_ring_wraps(self):
        self.fake.hash_capacity = 50
        for lba in range(0, 70, 7):
            self.dev.read(lba, 7)
        j = self.dev.hashes()
        self.assertEqual(j["newest"], 70)
        self.assertEqual(j["oldest"], 21)
        self.assertEqual(j["records"][0]["lba"], 20)

    def test_err_data_flag_and_clear(self):
        self.fake.bad[5] = UNC
        self.dev.read(3, 4, through_error=True)
        recs = self.dev.hashes()["records"]
        self.assertEqual([(x["lba"], x["err_data"]) for x in recs], [(3, False), (4, False), (5, True)])
        j = self.dev.hashes(clear=True)
        self.assertTrue(j["cleared"])
        self.assertEqual(self.dev.hashes()["newest"], 0)

    def test_compare_with_image(self):
        self.dev.read(0, 6)
        img = self.blank_image("d.img", 5)
        with open(img, "r+b") as f:
            for lba in range(5):
                f.seek(lba * 512)
                f.write(sector(lba) if lba != 2 else bytes(512))
        ok, bad, missing = self.dev.compare_hashes(self.dev.hashes()["records"], img)
        self.assertEqual(ok, 4)
        self.assertEqual([x["lba"] for x in bad], [2])
        self.assertEqual([x["lba"] for x in missing], [5])

    def test_cli(self):
        self.dev.read(1, 2)                             # (sector 0 is all zeros)
        args = ab.build_parser().parse_args(["hashes", "-j", "--compare", self.blank_image("z.img", 3)])
        import io
        import contextlib
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = ab.cmd_hashes(args, self.dev)
        self.assertEqual(rc, ab.EXIT_DEVICE)            # zeros in the image differ
        j = json.loads(out.getvalue())
        self.assertEqual(j["compare"]["match"], 0)
        self.assertEqual(len(j["compare"]["differ"]), 2)
def fake_sysfs(root, units):
    """A minimal /sys for find_devices(): units = [(serial, disk, tty or None)]."""
    def put(path, text=""):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(text + "\n")
    for n, (serial, disk, tty) in enumerate(units):
        usb = os.path.join(root, "devices", "pci0", "usb1", "1-%d" % (n + 1))
        put(usb + "/idVendor", "cafe")
        put(usb + "/idProduct", "4005")
        put(usb + "/serial", serial)
        if disk:
            scsi = os.path.join(usb, "1-%d:1.2" % (n + 1), "host%d" % n, "target%d:0:0" % n, "%d:0:0:0" % n)
            put(scsi + "/vendor", "ATAboy  ")
            put(scsi + "/rev", "api1")
            put(scsi + "/timeout", "30")
            os.makedirs(scsi + "/scsi_generic/sg%d" % n)
            blk = os.path.join(root, "block", disk)
            put(blk + "/size", "336260")
            os.symlink(scsi, blk + "/device")
        if tty:
            itf = os.path.join(usb, "1-%d:1.3" % (n + 1))
            put(itf + "/interface", "ATAboy API")
            cls = os.path.join(root, "class", "tty", tty)
            os.makedirs(cls)
            os.symlink(itf, cls + "/device")
        console = os.path.join(usb, "1-%d:1.0" % (n + 1))      # the setup-UI port is not the API
        put(console + "/interface", "ATAboy CDC")
        cls = os.path.join(root, "class", "tty", "ttyACM%d" % (10 + n))
        os.makedirs(cls)
        os.symlink(console, cls + "/device")


class TestDiscovery(HostTestCase):
    def test_serials_and_ports(self):
        root = self.path("sys")
        fake_sysfs(root, [("E66138935F4B2F2B", "sdb", "ttyACM1"), ("0123456789ABCDEF", "sdc", None)])
        devs = ab.find_devices(sysfs=root)
        self.assertEqual([(d["block"], d["sg"], d["tty"], d["serial"]) for d in devs],
                         [("/dev/sdb", "/dev/sg0", "/dev/ttyACM1", "E66138935F4B2F2B"),
                          ("/dev/sdc", "/dev/sg1", None, "0123456789ABCDEF")])
        self.assertEqual(ab.select_device(devs, "0123456789abcdef")["block"], "/dev/sdc")
        self.assertEqual(ab.select_device(devs)["block"], "/dev/sdb")
        with self.assertRaises(ab.TransportError):
            ab.select_device(devs, "nope")
        with self.assertRaises(ab.TransportError):
            ab.select_device([])

    def test_port_without_disk(self):
        root = self.path("sys")
        fake_sysfs(root, [("AAAA", None, "ttyACM3")])
        devs = ab.find_devices(sysfs=root)
        self.assertEqual(len(devs), 1)
        self.assertEqual((devs[0]["block"], devs[0]["tty"], devs[0]["serial"]), (None, "/dev/ttyACM3", "AAAA"))

    def test_cli_serial_option(self):
        args = ab.build_parser().parse_args(["status", "--serial", "AAAA"])
        self.assertEqual(args.serial, "AAAA")
        args = ab.build_parser().parse_args(["-s", "BBBB", "status"])
        self.assertEqual(args.serial, "BBBB")


class EmptyReplyTest(HostTestCase):
    def blank_first(self, ops):
        real, state = self.fake.sgio, {"n": 0}
        def sgio(fd, cdb, direction, data=None, length=0, timeout_ms=60000):
            if cdb[1] in ops and state["n"] == 0:
                state["n"] += 1
                self.fake.calls.append((cdb[1], bytes(cdb[4:16])))
                return b"", length
            return real(fd, cdb, direction, data, length, timeout_ms)
        ab.sgio = sgio

    def test_read_only_op_is_resent(self):
        self.blank_first({ab.OPS["status"]})
        r = self.dev.call("status")
        self.assertTrue(r.ok)
        self.assertEqual(self.dev.empty_replies, 1)

    def test_state_changing_op_is_not_resent(self):
        self.dev.ext = False                    # firmware without request ids
        self.blank_first({ab.OPS["mount"]})
        with self.assertRaises(ab.TransportError):
            self.dev.call("mount")
        self.assertEqual(self.dev.empty_replies, 0)
        self.assertEqual(sum(1 for op, _ in self.fake.calls if op == ab.OPS["mount"]), 1)

class TestGeometryOwner(HostTestCase):
    def swap_drive(self):
        self.fake.model, self.fake.serial = "WDC WD10EZEX-00BN5A0", "WD-WCC3F0000000"

    def test_mount_refused_after_swap(self):
        self.swap_drive()
        r = self.dev.mount(rescan=False)
        self.assertEqual(r.result_name, "geometry_mismatch")
        self.assertFalse(self.fake.mounted)
        self.assertEqual(r.json["geometry"]["identity"], "mismatch")
        self.assertEqual(self.fake.calls[-1], (ab.OPS["mount"], bytes(12)))

    def test_force_mount(self):
        self.swap_drive()
        r = self.dev.mount(rescan=False, force=True)
        self.assertTrue(r.ok)
        self.assertEqual(self.fake.calls[-1][1][0], 1)
        self.assertTrue(self.fake.mounted)

    def test_detect_sets_owner_keep_does_not(self):
        self.swap_drive()
        r = self.dev.detect(geometry="keep", mount=True)
        self.assertEqual(r.result_name, "geometry_mismatch")
        r = self.dev.detect(geometry="keep", force_mount=True)
        self.assertTrue(r.ok)
        self.assertEqual(self.fake.calls[-1][1][6], 2 | 8)
        self.fake.mounted = False
        r = self.dev.detect(mount=True)
        self.assertTrue(r.ok)
        self.assertEqual(r.json["geometry"]["owner"]["model"], "WDC WD10EZEX-00BN5A0")

    def test_forced_manual_detect_and_mount_cdb(self):
        # A drive that doesn't answer IDENTIFY: manual CHS + force + mount in one DETECT.
        r = self.dev.detect(geometry="manual", chs=(615, 4, 17), force=True, mount=True)
        self.assertTrue(r.ok)
        a = [args for op, args in self.fake.calls if op == 0x10][-1]
        self.assertEqual(a[:7], bytes([0, 4]) + struct.pack(">H", 615) + bytes([4, 17, 2 | 4]))

    def test_not_ready_drive_is_not_mounted(self):
        self.fake.ready = False
        r = self.dev.mount(rescan=False)
        self.assertEqual(r.result_name, "not_ready")
        self.assertFalse(self.fake.mounted)
        self.assertTrue(self.dev.mount(rescan=False, force=True).ok)

    def test_human_status_shows_mismatch(self):
        self.swap_drive()
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            ab.human_status(self.dev.status())
        self.assertIn("MISMATCH: set up for FAKE DRIVE (serial 1)", buf.getvalue())

    def test_cli_mount_force_exit_codes(self):
        self.swap_drive()
        orig = ab.ATAboy
        ab.ATAboy = lambda path=None, **kw: self.dev
        self.dev.close = lambda: None
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(ab.main(["--no-sudo", "-j", "mount", "--no-rescan"]), ab.EXIT_DEVICE)
                self.assertEqual(ab.main(["--no-sudo", "-j", "mount", "--no-rescan", "--force"]), ab.EXIT_OK)
        finally:
            ab.ATAboy = orig


class TestPark(HostTestCase):
    def test_standby_only(self):
        r = self.dev.park()
        self.assertTrue(r.ok)
        self.assertEqual(self.fake.calls[-1], (0x15, bytes(12)))
        self.assertIsNone(r.json["seek"])
        self.assertEqual(r.json["standby"]["command"], 0xE0)

    def test_landing_zone_and_fallback(self):
        self.fake.park_e0_aborts = True
        r = self.dev.park(cylinder=1023)
        self.assertTrue(r.ok)
        self.assertEqual(self.fake.calls[-1][1][:4], bytes([1, 0, 0x03, 0xFF]))
        self.assertEqual(r.json["seek"]["cylinder"], 1023)
        self.assertEqual(r.json["standby"]["command"], 0x94)
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            ab.human_park(r.json)
        self.assertIn("sent the ATA-1 STANDBY IMMEDIATE 94h", buf.getvalue())
        self.assertIn("cylinder 1023", buf.getvalue())

    def test_refused_while_mounted_and_bad_cylinder(self):
        self.fake.mounted = True
        self.assertEqual(self.dev.park().result_name, "mounted")
        with self.assertRaises(ValueError):
            self.dev.park(cylinder=70000)

    def test_cli(self):
        self.fake.park_seek_fails = True
        orig = ab.ATAboy
        ab.ATAboy = lambda path=None, **kw: self.dev
        self.dev.close = lambda: None
        try:
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(ab.main(["--no-sudo", "park", "--landing-zone", "0x3ff"]), ab.EXIT_DEVICE)
            self.assertIn("SEEK to the landing zone failed", out.getvalue())
            self.assertEqual(self.fake.park_calls[-1], (1, 1023))
            self.fake.park_seek_fails = False
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(ab.main(["--no-sudo", "-j", "park"]), ab.EXIT_OK)
        finally:
            ab.ATAboy = orig


class TestSettingsStatus(HostTestCase):
    def human(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            ab.human_status(self.dev.status())
        return buf.getvalue()

    def test_ok_settings_are_quiet(self):
        self.assertNotIn("settings", self.human())
        self.fake.settings = {"load": "legacy", "field": None}
        self.assertNotIn("REJECTED", self.human())

    def test_rejected_settings_are_reported(self):
        self.fake.settings = {"load": "invalid", "field": "chs"}
        self.assertIn("SAVED SETTINGS REJECTED (invalid, field chs): defaults loaded", self.human())
        self.fake.settings = {"load": "crc_error", "field": None}
        self.assertIn("REJECTED (crc_error)", self.human())

    def test_old_firmware_without_settings(self):
        self.fake.status_json = lambda orig=self.fake.status_json: {
            k: v for k, v in orig().items() if k != "settings"}
        self.assertIn("firmware", self.human())
class FakeSerialFirmware(threading.Thread):
    """The firmware side of the API serial port (api_serial.c + apiframe.c),
    on one end of a socketpair: parses request frames, runs them through a
    FakeDevice and answers with response frames.  Hooks inject faults."""

    def __init__(self, fake, sock):
        super().__init__(daemon=True)
        self.fake, self.sock = fake, sock
        self.buf = b""
        self.corrupt_next = 0        # flip a bit in the next N responses
        self.drop_next = 0           # run the next N requests but send nothing back
        self.bad_frame_next = 0      # pretend the next N requests arrived corrupted (not run)
        self.junk_before = b""       # sent before every response
        self.stale_before = False    # send a response with an old tag first
        self.frames = 0

    def run(self):
        try:
            while True:
                chunk = self.sock.recv(65536)
                if not chunk:
                    return
                self.buf += chunk
                while self.parse_one():
                    pass
        except OSError:
            return

    def parse_one(self):
        i = self.buf.find(b"\xa5Q")
        if i < 0:
            self.buf = self.buf[-1:]
            return False
        self.buf = self.buf[i:]
        if len(self.buf) < 10:
            return False
        _, _, ver, _, tag, alloc, n = struct.unpack(">BBBBHHH", self.buf[:10])
        if len(self.buf) < 10 + n + 4:
            return False
        f, self.buf = self.buf[:10 + n + 4], self.buf[10 + n + 4:]
        self.frames += 1
        if binascii.crc32(f[:-4]) & 0xFFFFFFFF != struct.unpack(">I", f[-4:])[0] or ver != 1:
            self.send(ab.frame_response(tag, ab.ST_BAD_FRAME))
            return True
        if self.bad_frame_next:
            self.bad_frame_next -= 1
            self.send(ab.frame_response(tag, ab.ST_BAD_FRAME))
            return True
        try:
            body, status = self.fake.handle(f[10:10 + 16], alloc or 4096), ab.ST_OK
        except ab.ScsiCheckCondition as e:
            body, status = bytes([e.key, e.asc, e.ascq]), ab.ST_CHECK
        out = ab.frame_response(tag, status, body)
        if self.drop_next:
            self.drop_next -= 1
            return True
        if self.corrupt_next:
            self.corrupt_next -= 1
            out = bytearray(out)
            out[12] ^= 0x10
            out = bytes(out)
        if self.stale_before:
            self.send(ab.frame_response((tag - 1) & 0xFFFF, ab.ST_OK, body))
        self.send(self.junk_before + out)
        return True

    def send(self, data):
        self.sock.sendall(data)


class SerialTestCase(HostTestCase):
    """The host library over the serial framing, against the simulated firmware."""

    def setUp(self):
        super().setUp()
        ab.sgio = self._no_sgio
        self.fake.ext = True
        a, b = socket.socketpair()
        a.setblocking(False)
        self.host_sock, self.fw_sock = a, b
        self.fw = FakeSerialFirmware(self.fake, b)
        self.fw.start()
        self.port = ab.SerialTransport(fd=a.fileno())
        self.dev = make_dev(self.fake, port=self.port)
        self.dev.info_dev = {"serial": "TESTUNIT%d" % id(self), "block": None, "sg": None,
                             "tty": "/dev/fake-tty", "sysfs": None}

    def tearDown(self):
        self.host_sock.close()
        self.fw_sock.close()
        self.fw.join(1)
        super().tearDown()

    @staticmethod
    def _no_sgio(*a, **k):
        raise AssertionError("SG_IO used on the serial path")

    def count(self, op):
        return sum(1 for o, _ in self.fake.calls if o == ab.OPS[op])


class TestSerialTransport(SerialTestCase):
    def test_frames_round_trip(self):
        f = ab.frame_request(0x1234, bytes(range(16)), alloc=512)
        self.assertEqual(f[:10], bytes([0xA5, ord("Q"), 1, 0, 0x12, 0x34, 0x02, 0x00, 0, 16]))
        self.assertEqual(struct.unpack(">I", f[-4:])[0], binascii.crc32(f[:-4]) & 0xFFFFFFFF)
        self.assertEqual(ab.frame_request(1, bytes(16), alloc=8192)[6:8], b"\0\0")   # 0 = 4096

    def test_status_and_read_over_serial(self):
        st = self.dev.status()
        self.assertEqual(st["geometry"]["sectors"], self.fake.total)
        r = self.dev.read(10, 7)
        self.assertTrue(r.ok)
        self.assertEqual(r.data, b"".join(sector(10 + i) for i in range(7)))
        self.assertTrue(self.dev.ext)
        self.assertEqual(self.dev.boot_id, self.fake.boot_id)

    def test_check_condition_is_relayed(self):
        with self.assertRaises(ab.ScsiCheckCondition) as cm:
            self.dev.call("status", alloc=100)
        self.assertEqual((cm.exception.key, cm.exception.asc), (5, 0x24))

    def test_junk_and_stale_frames_skipped(self):
        self.fw.junk_before = b"AT\r\n\xa5\xa5R\x07"
        self.fw.stale_before = True
        for _ in range(3):
            self.assertTrue(self.dev.call("status").ok)
        self.assertEqual(self.port.stale_frames, 3)

    def test_corrupt_reply_resends_read_only(self):
        self.fw.corrupt_next = 1
        self.assertTrue(self.dev.call("status").ok)
        self.assertEqual(self.dev.empty_replies, 1)
        self.assertEqual(self.count("status"), 2)

    def test_corrupt_reply_state_change_runs_once(self):
        self.fw.corrupt_next = 1
        r = self.dev.mount(rescan=False)
        self.assertTrue(r.ok and r.replayed and r.rid_taken)
        self.assertEqual(self.count("mount"), 1)
        self.assertEqual(self.fake.replays, 1)

    def test_lost_reply_state_change_runs_once(self):
        self.fw.drop_next = 1
        self.port.SLACK_S = 0.0
        r = self.dev.call("unmount", timeout_ms=200)
        self.assertTrue(r.ok and r.replayed)
        self.assertEqual(self.count("unmount"), 1)

    def test_request_corrupted_on_the_way_is_resent(self):
        self.fw.bad_frame_next = 1
        self.dev.ext = None
        self.assertTrue(self.dev.call("mount").ok)
        self.assertEqual(self.count("mount"), 1)
        self.assertEqual(self.fake.replays, 0)

    def test_two_losses_give_up(self):
        self.fw.drop_next = 2
        self.port.SLACK_S = 0.0
        with self.assertRaises(ab.ReplyLost):
            self.dev.call("status", timeout_ms=100)


class TestRequestIds(HostTestCase):
    def setUp(self):
        super().setUp()
        self.fake.ext = True

    def test_ids_only_on_state_changing_ops(self):
        seen = []
        real = self.fake.handle
        def handle(cdb, length):
            seen.append(bytes(cdb))
            return real(cdb, length)
        self.fake.handle = handle
        self.dev.status()
        self.dev.set_config("no_retry", 1)
        self.dev.set_config("no_retry", 1)
        self.assertEqual([c[2:4] for c in seen], [b"AX"] * 3)
        self.assertEqual(seen[0][12:16], bytes(4))
        ids = [struct.unpack(">I", c[12:16])[0] for c in seen[1:]]
        self.assertTrue(all(ids) and ids[0] != ids[1])          # a fresh id per call
        self.assertEqual(self.fake.calls[-1][1][8:12], bytes(4))   # the op never sees the id

    def test_empty_reply_state_change_resent_with_same_id(self):
        state = {"n": 0, "cdbs": []}
        real = self.fake.handle
        def handle(cdb, length):
            state["cdbs"].append(bytes(cdb))
            reply = real(cdb, length)
            if cdb[1] == ab.OPS["mount"] and state["n"] == 0:
                state["n"] += 1
                return b""                     # it ran, but the reply got lost
            return reply
        self.fake.handle = handle
        r = self.dev.mount(rescan=False)
        self.assertTrue(r.ok and r.replayed)
        self.assertEqual(state["cdbs"][0], state["cdbs"][1])
        self.assertEqual(sum(1 for o, _ in self.fake.calls if o == ab.OPS["mount"]), 1)
        self.assertEqual(self.dev.empty_replies, 1)

    def test_conflict(self):
        cdb = bytes([0xC0, 0x12, ord("A"), ord("X")]) + bytes(8) + b"\x00\x00\x00\x09"
        self.assertEqual(ab.Reply(self.fake.handle(cdb, 4096)).result_name, "ok")
        other = bytes([0xC0, 0x13, ord("A"), ord("X")]) + bytes(8) + b"\x00\x00\x00\x09"
        r = ab.Reply(self.fake.handle(other, 4096))
        self.assertEqual(r.result_name, "request_id_conflict")
        self.assertTrue(r.ext and r.request_id == 9)

    def test_old_firmware_falls_back_to_basic(self):
        self.fake.ext = False
        r = self.dev.mount(rescan=False)
        self.assertTrue(r.ok and not r.ext and r.boot_id is None)
        self.assertIs(self.dev.ext, False)
        self.dev.status()
        self.assertEqual(sum(1 for o, _ in self.fake.calls if o == ab.OPS["mount"]), 1)

    def test_reboot_detected(self):
        seen = []
        self.dev.info_dev = {"serial": "REBOOTUNIT"}
        self.dev.on_reboot = seen.append
        r1 = self.dev.call("status")
        self.assertFalse(r1.rebooted)
        self.fake.reboot()
        r2 = self.dev.call("status")
        self.assertTrue(r2.rebooted)
        self.assertEqual(self.dev.reboots, 1)
        self.assertEqual(seen[0]["old_boot_id"], r1.boot_id)
        self.assertEqual(seen[0]["new_boot_id"], r2.boot_id)
        # a new instance in the same process notices a reboot since the last one too
        other = make_dev(self.fake)
        other.info_dev = {"serial": "REBOOTUNIT"}
        self.fake.reboot()
        self.assertTrue(other.call("status").rebooted)

    def test_state_counter(self):
        a = self.dev.call("status").state_counter
        self.dev.mount(rescan=False)
        b = self.dev.call("status").state_counter
        self.assertGreater(b, a)
        self.assertEqual(self.dev.link_info()["state_counter"], b)


class TestTransportChoice(unittest.TestCase):
    """ATAboy() picks serial when the API port opens, else SG_IO."""

    def setUp(self):
        self._orig = (ab.find_devices, ab.SerialTransport, ab.ATAboy._open_sg)
        self.dev_entry = {"block": "/dev/sdz", "sg": "/dev/sg9", "tty": "/dev/ttyACM7", "serial": "U1"}
        ab.find_devices = lambda *a, **k: [dict(self.dev_entry)]
        ab.ATAboy._open_sg = staticmethod(lambda path: 99)
        self.serial_error = None
        test = self

        class Port:
            def __init__(self, path):
                if test.serial_error:
                    raise test.serial_error
                self.path = path

            def close(self):
                pass
        ab.SerialTransport = Port

    def tearDown(self):
        ab.find_devices, ab.SerialTransport, ab.ATAboy._open_sg = self._orig

    def test_auto_prefers_serial(self):
        d = ab.ATAboy()
        self.assertEqual((d.transport, d.path, d.fd), ("serial", "/dev/ttyACM7", None))

    def test_auto_falls_back_when_no_permission(self):
        self.serial_error = PermissionError("denied")
        d = ab.ATAboy()
        self.assertEqual((d.transport, d.path, d.fd), ("sgio", "/dev/sg9", 99))

    def test_serial_insists(self):
        self.serial_error = PermissionError("denied")
        with self.assertRaises(PermissionError):
            ab.ATAboy(transport="serial")
        self.dev_entry["tty"] = None
        self.serial_error = None
        with self.assertRaises(ab.TransportError):
            ab.ATAboy(transport="serial")

    def test_no_sudo_when_serial_usable(self):
        with tempfile.NamedTemporaryFile() as tty:
            self.dev_entry["tty"] = tty.name
            parse = ab.build_parser().parse_args
            self.assertTrue(ab._serial_usable(parse(["status"])))
            self.assertFalse(ab._serial_usable(parse(["--transport", "sgio", "status"])))
            self.assertFalse(ab._serial_usable(parse(["ata", "0xEC", "--read", "1"])))   # SAT needs SG_IO
            self.assertTrue(ab._serial_usable(parse(["image", "x.img"])))   # /dev/sdz unreadable: API reads, no sudo
            os.chmod(tty.name, 0)
            if os.geteuid() != 0:
                self.assertFalse(ab._serial_usable(parse(["status"])))
        self.dev_entry["tty"] = None
        self.assertFalse(ab._serial_usable(ab.build_parser().parse_args(["status"])))

    def test_give_back_only_under_sudo(self):
        calls = []
        with mock.patch.object(ab.os, "chown", lambda p, u, g: calls.append((p, u, g))), \
             mock.patch.object(ab.os, "geteuid", lambda: 0), \
             mock.patch.dict(ab.os.environ, {"SUDO_UID": "1000", "SUDO_GID": "1001"}):
            ab.give_back("f")
        self.assertEqual(calls, [("f", 1000, 1001)])
        with mock.patch.object(ab.os, "chown", lambda p, u, g: calls.append((p, u, g))), \
             mock.patch.object(ab.os, "geteuid", lambda: 1000):
            ab.give_back("g")
        self.assertEqual(len(calls), 1)

    def test_sgio_and_explicit_paths(self):
        self.assertEqual(ab.ATAboy(transport="sgio").transport, "sgio")
        self.assertEqual(ab.ATAboy("/dev/sg9").transport, "sgio")
        self.assertEqual(ab.ATAboy("/dev/ttyACM7").transport, "serial")
        with self.assertRaises(ab.TransportError):
            ab.ATAboy("/dev/ttyACM7", transport="sgio")


if __name__ == "__main__":
    unittest.main(verbosity=2)
