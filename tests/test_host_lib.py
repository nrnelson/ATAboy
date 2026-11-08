#!/usr/bin/env python3
"""Unit tests for the host library (host/ataboy) against a simulated device.

FakeDevice models the ATAboy API firmware at the SG_IO boundary: the module's
sgio() is replaced, vendor CDBs (0xC0) are decoded and answered with replies
built byte for byte per docs/API.md (16-byte header, JSON or 32-byte ATA
result block + data).  No hardware, no root.

    python3 tests/test_host_lib.py
"""

import importlib.machinery
import importlib.util
import json
import os
import struct
import sys
import contextlib
import io
import tempfile
import unittest

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

    def __init__(self, total=4096, ecc=4, read_timeout_ms=1000):
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
        self.settings = {"load": "ok", "field": None}
        self.owner = {"model": "FAKE DRIVE", "serial": "1"}
        self.model, self.serial = "FAKE DRIVE", "1"
        self.ready = True            # BSY clear + DRDY within the mount check's 3 s

    # -- transport ------------------------------------------------------------

    def sgio(self, fd, cdb, direction, data=None, length=0, timeout_ms=60000):
        cdb = bytes(cdb)
        if len(cdb) != 16 or cdb[0] != 0xC0 or cdb[2:4] != b"AB" or length < 256:
            raise ab.ScsiCheckCondition(bytes([0x70, 0, 5, 0, 0, 0, 0, 10, 0, 0, 0, 0, 0x24, 0]))
        assert direction == ab.SG_DXFER_FROM_DEV
        op, a = cdb[1], cdb[4:16]
        self.calls.append((op, a))
        if self.raise_on:
            exc = self.raise_on(op, a)
            if exc is not None:
                raise exc
        self.uptime += 10
        reply = self.dispatch(op, a, length)
        assert len(reply) <= length, "reply %d > allocation %d" % (len(reply), length)
        return reply, length - len(reply)

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
            self.record("read_diag", cmd, lba, count, i, res, fail, st, er, tfv, err_data)
            blk = self.result_block(lba, count, i, cmd, ide, st, er, err_data, tfv, fail)
            return self.reply(op, res, ab.CT_BINARY, blk + data)
        blk = self.result_block(lba, count, count, cmd, 0)
        return self.reply(op, 0, ab.CT_BINARY, blk + data)

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


def make_dev(fake, info_dev=None):
    dev = object.__new__(ab.ATAboy)
    dev.fd = -1
    dev.info_dev = info_dev
    dev.path = "/dev/fake"
    dev.empty_replies = 0
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

    def test_image_not_mounted_is_api_error(self):
        self.dev.info_dev = {"sysfs": self.dir, "block": "/dev/sdzz", "sg": None, "rev": "api1",
                             "scsi_timeout_s": 30}
        with self.assertRaises(ab.ApiError) as cm:
            self.dev.image(self.path("x.img"))
        self.assertEqual(cm.exception.reply.result_name, "not_mounted")



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
        ab.ATAboy = lambda path=None: self.dev
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
        ab.ATAboy = lambda path=None: self.dev
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
