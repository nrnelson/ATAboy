#!/usr/bin/env bash
# Hardware regression for the ATAboy automation-API firmware.
#
#   tools/hw-regress.sh [--ref IMAGE] [--scratch] [--reboot]
#
#   --ref IMAGE   known-good image of the attached drive; sector data is compared
#                 against it (only areas the test does not write)
#   --scratch     the attached drive is expendable: also test writes, WRITE LONG
#                 corruption, image + rescue of damaged sectors (restored after)
#   --reboot      also test reboot / re-enumeration (set ATABOY_REATTACH under WSL)
#
# Read-only by default.  Prints PASS/FAIL per check; exit status = failures.
set -uo pipefail

here=$(cd "$(dirname "$0")" && pwd)
A="$here/../host/ataboy"
ref="" scratch=0 reboot=0
while [[ $# -gt 0 ]]; do
    case $1 in
        --ref) ref=$2; shift 2 ;;
        --scratch) scratch=1; shift ;;
        --reboot) reboot=1; shift ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fails=0
pass() { printf 'PASS  %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*"; fails=$((fails + 1)); }
check() { local name=$1; shift; if "$@" >"$work/out" 2>&1; then pass "$name"; else fail "$name"; sed 's/^/      /' "$work/out" | head -5; fi; }
jq_() { python3 -c "import json,sys; d=json.load(sys.stdin); print($1)"; }

# --- identity ---------------------------------------------------------------
info=$("$A" -j info) || { echo "no ATAboy answering"; exit 1; }
echo "firmware: $(echo "$info" | jq_ 'd["fw"], d["git"]')"
check "status answers" "$A" -j status

# --- detect / mount -----------------------------------------------------------
if "$A" -j detect --mount >"$work/out" 2>&1; then pass "detect + mount"
else
    fail "detect + mount"; sed 's/^/      /' "$work/out" | head -5
    echo "no drive answering: attach and power a drive, then re-run"; exit $fails
fi
st=$("$A" -j status)
sectors=$(echo "$st" | jq_ 'd["geometry"]["sectors"]')
dev=$("$A" -j list | jq_ 'd[0]["block"]')
echo "drive: $(echo "$st" | jq_ 'd["drive"]["model"]'), $sectors sectors, $dev"
[[ $(cat /sys/block/$(basename "$dev")/size) == "$sectors" ]] && pass "host sees full capacity" || fail "host capacity mismatch"
[[ $(cat /sys/block/$(basename "$dev")/ro) == 1 ]] && pass "write protect visible to host (ro=1)" || fail "host does not see write protect"

# --- data path ------------------------------------------------------------------
n=$(( sectors < 20000 ? sectors : 20000 ))
sudo dd if="$dev" of="$work/usb.bin" bs=512 count=$n iflag=direct status=none \
    && pass "USB disk read $n sectors" || fail "USB disk read"
"$A" read 0 64 --out "$work/api.bin" >/dev/null && cmp -s -n $((64*512)) "$work/api.bin" "$work/usb.bin" \
    && pass "API read == USB disk read" || fail "API read differs from USB disk read"
if [[ -n $ref ]]; then
    cmp -s -n $((n*512)) "$work/usb.bin" "$ref" && pass "USB disk data == reference image" || fail "data differs from reference image"
fi
check "image 2000 sectors with mapfile" "$A" image "$work/img" --count 2000
cmp -s -n $((2000*512)) "$work/img" "$work/usb.bin" && pass "image data == USB disk data" || fail "image data differs"
out=$("$A" -j verify 0 "$n")
[[ $(echo "$out" | jq_ 'd["scanned"]') == "$n" ]] && pass "verify $n sectors ($(echo "$out" | jq_ 'len(d["bad"])') bad)" || fail "verify incomplete"
out=$("$A" -j verify 0 "$sectors" --budget 3)
[[ $(echo "$out" | jq_ 'd["seconds"] < 6') == True ]] && pass "verify honours time budget" || fail "verify overran budget"

# --- SAT pass-through -------------------------------------------------------------
command -v sg_sat_identify >/dev/null && check "sg_sat_identify" sudo sg_sat_identify --readonly "$dev"
command -v hdparm >/dev/null && check "hdparm -I" sudo hdparm -I "$dev"
command -v smartctl >/dev/null && check "smartctl -d sat -i" sudo smartctl -d sat -i "$dev"
out=$("$A" -j ata 0xE5)
[[ $(echo "$out" | jq_ 'd["ok"] and "status" in d') == True ]] && pass "non-data command returns registers" || fail "CHECK POWER MODE registers"
head -c 512 /dev/zero > "$work/z"
out=$("$A" -j ata 0x30 --count 1 --lba 1 --write "$work/z")
[[ $(echo "$out" | jq_ 'd.get("blocked")') == write_protected ]] && pass "write blocked by write protect" || fail "write not blocked: $out"
out=$("$A" -j ata 0x91 --count 1)
[[ $(echo "$out" | jq_ 'd.get("sense_key")') == 5 ]] && pass "INIT PARAMS refused while mounted" || fail "INIT PARAMS not refused while mounted: $out"

# --- error accounting -------------------------------------------------------------
"$A" clear --stats >/dev/null
sudo dd if="$dev" of=/dev/null bs=512 count=8 iflag=direct status=none
[[ $("$A" -j status | jq_ 'd["stats"]["read_cmds"]') -ge 1 ]] && pass "stats count USB reads" || fail "stats"

# --- scratch-only: writes, corruption, rescue ------------------------------------------
if (( scratch )); then
    lba=$(( sectors - 64 ))
    "$A" config write_protect=0 >/dev/null
    [[ $(cat /sys/block/$(basename "$dev")/ro) == 0 ]] && pass "host sees write protect off" || fail "host still read-only"
    sudo dd if="$dev" of="$work/save.bin" bs=512 skip=$lba count=16 iflag=direct status=none
    head -c 8192 /dev/urandom > "$work/rand"
    sudo dd if="$work/rand" of="$dev" bs=512 seek=$lba count=16 oflag=direct status=none \
        && sudo dd if="$dev" bs=512 skip=$lba count=16 iflag=direct status=none | cmp -s - "$work/rand" \
        && pass "USB write + readback" || fail "USB write/readback"
    head -c 512 /dev/urandom > "$work/r1"
    python3 - "$lba" "$st" > "$work/chs" <<'EOF'
import json, sys
lba = int(sys.argv[1]) + 20; g = json.loads(sys.argv[2])["geometry"]
if g["addressing"] == "chs":
    t, s = divmod(lba, g["spt"]); c, h = divmod(t, g["heads"]); print(s + 1 | (c << 8), h, 0)
else:
    print(lba & 0xFFFFFF, 0x40 | (lba >> 24), 1)
EOF
    read -r ptlba ptdev _ < "$work/chs"
    "$A" -j ata 0x30 --count 1 --lba "$ptlba" --device "$ptdev" --write "$work/r1" >/dev/null \
        && "$A" read $((lba + 20)) --out "$work/r1b" >/dev/null && cmp -s "$work/r1" "$work/r1b" \
        && pass "pass-through WRITE SECTORS + readback" || fail "pass-through write"
    out=$("$A" -j ata 0xFF)
    [[ $(echo "$out" | jq_ 'd.get("blocked")') == not_armed ]] && pass "dangerous opcode needs arm" || fail "unarmed dangerous: $out"
    "$A" clear --stats >/dev/null
    "$A" -j ata 0xFF --dangerous >/dev/null
    [[ $("$A" -j status | jq_ 'd["stats"]["passthru_cmds"]') == 1 ]] && pass "armed command executed exactly once" || fail "armed command count"
    "$A" corrupt $((lba + 4)) --bits 32 >/dev/null
    out=$("$A" -j read $((lba + 4)) --through-error --timeout 15000)
    [[ $(echo "$out" | jq_ 'd["chunks"][0]["error_bits"]') == UNC ]] && pass "corrupt makes UNC sector" || fail "corrupt: $out"
    "$A" image "$work/p.img" --start $((lba)) --count 16 >/dev/null
    grep -q -- "  -$" "$work/p.img.map" && pass "image maps the bad sector" || fail "image map"
    out=$("$A" -j rescue "$work/p.img" "$work/p.img.map" --vote 3)
    [[ $(echo "$out" | jq_ 'len(d["readlong"])') -ge 1 ]] && pass "rescue falls through to READ LONG" || fail "rescue: $out"
    "$A" clear >/dev/null
    sudo dd if="$dev" of=/dev/null bs=512 skip=$((lba + 4)) count=1 iflag=direct status=none 2>/dev/null
    [[ $("$A" -j log | jq_ 'd["total"]') == 1 ]] && pass "bad sector costs one drive attempt" || fail "host retried the bad sector"
    sudo dd if="$work/save.bin" of="$dev" bs=512 seek=$lba count=16 oflag=direct status=none
    sudo dd if="$dev" bs=512 skip=$lba count=16 iflag=direct status=none | cmp -s - "$work/save.bin" \
        && pass "scratch area restored" || fail "scratch restore"
    "$A" config write_protect=1 >/dev/null
fi

# --- reboot ------------------------------------------------------------------------------
if (( reboot )); then
    "$A" reboot >/dev/null && "$A" wait --timeout 30 >/dev/null && pass "reboot + re-enumerate" || fail "reboot"
fi

echo "---"
(( fails )) && echo "$fails check(s) FAILED" || echo "all checks passed"
exit $fails
