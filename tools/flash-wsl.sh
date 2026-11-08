#!/usr/bin/env bash
# Flash ATAboy firmware from WSL2 without touching the hardware.
#
#   tools/flash-wsl.sh firmware/build/ATAboy.uf2
#
# 1. asks the running firmware to reboot into the RP2350 UF2 bootloader
#    (needs API firmware; otherwise hold BOOTSEL while plugging in USB),
# 2. waits for Windows to mount the "RP2350" drive and copies the UF2 onto it,
# 3. waits for the ATAboy (cafe:4005) to come back, re-attaches it to WSL with
#    usbipd and prints the new firmware identity.
#
# Requires usbipd-win with the ATAboy already bound once (usbipd bind).
# BOOTSEL_WAIT=N waits N seconds for the bootloader drive (default 60).
set -euo pipefail

uf2=${1:?usage: flash-wsl.sh FILE.uf2}
[[ -f $uf2 ]] || { echo "no such file: $uf2" >&2; exit 2; }
here=$(cd "$(dirname "$0")" && pwd)
ataboy=${ATABOY:-$here/../host/ataboy}
ps() { powershell.exe -NoProfile -NonInteractive -Command "$1" | tr -d '\r'; }
busid() { usbipd.exe list 2>/dev/null | tr -d '\r' | awk -v id="$1" '$2 == id { print $1; exit }'; }

if "$ataboy" --json list 2>/dev/null | grep -q '"block"'; then
    echo "requesting BOOTSEL..."
    "$ataboy" reboot --bootsel || echo "reboot request failed; press BOOTSEL manually"
else
    echo "no ATAboy attached to WSL; put it in BOOTSEL by hand (hold button, plug in USB)"
fi

echo -n "waiting for RP2350 drive"
letter=""
for _ in $(seq 1 "${BOOTSEL_WAIT:-60}"); do
    letter=$(ps "(Get-Volume -FileSystemLabel RP2350 -ErrorAction SilentlyContinue | Select-Object -First 1).DriveLetter")
    [[ -n $letter ]] && break
    echo -n .; sleep 1
done
echo
[[ -n $letter ]] || { echo "RP2350 drive did not appear" >&2; exit 3; }

win=$(wslpath -w "$(readlink -f "$uf2")")
echo "copying $(basename "$uf2") to ${letter}:"
ps "Copy-Item -LiteralPath '$win' -Destination '${letter}:\\'"

echo -n "waiting for ATAboy to re-enumerate"
bid=""
for _ in $(seq 1 60); do
    bid=$(busid cafe:4005)
    [[ -n $bid ]] && break
    echo -n .; sleep 1
done
echo
[[ -n $bid ]] || { echo "ATAboy did not come back (check usbipd list)" >&2; exit 3; }
sleep 1
usbipd.exe attach --wsl --busid "$bid" >/dev/null 2>&1 || true
"$ataboy" wait --timeout 30 && "$ataboy" info
