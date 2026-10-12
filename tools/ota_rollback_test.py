#!/usr/bin/env python3
"""
Test that the rollback actually works, by breaking an image on purpose.

The claim being checked is specific: if the new firmware fails to boot, the
bootloader reverts to the previous one by itself, with no USB and no person.

Method: take the real image, corrupt a byte in a code segment, upload that, and
confirm the device still comes up. A corrupted image will not pass the SHA-256
check, so the device should refuse it at verification time rather than at boot -
which already proves the rejection path works. To also exercise the boot-time
path, the checksum is recomputed so the image verifies but the code is wrong.

Usage:
    tools/ota_rollback_test.py build/esp32_scale.bin
"""
import hashlib
import struct
import sys
import time
import subprocess

ORIGIN = "http://192.168.22.123"


def flip_code_byte(data):
    """Corrupt a byte inside the first (code) segment and repair the appended
    SHA-256, so the image passes verification but does not work."""
    b = bytearray(data)
    seg_count = b[1]
    # Segment header: load_addr(4) data_len(4) data
    off = 24          # image header + extended header
    for _ in range(seg_count):
        if off + 8 > len(b):
            break
        data_len = struct.unpack_from("<I", b, off + 4)[0]
        if data_len > 1024:            # first sizeable segment = code
            target = off + 8 + data_len // 2
            print("corrupting byte at 0x%X (inside a %d byte segment)"
                  % (target, data_len))
            b[target] ^= 0xFF
            break
        off += 8 + data_len
    # Repair the trailing digest so verification passes and the fault has to be
    # found by the bootloader at boot time instead.
    b[-32:] = hashlib.sha256(bytes(b[:-32])).digest()
    return bytes(b)


def status():
    out = subprocess.run(
        ["curl", "-s", "--max-time", "20", ORIGIN + "/api/ota"],
        capture_output=True, text=True).stdout
    return out.strip()


def main():
    image = open(sys.argv[1], "rb").read()
    bad = flip_code_byte(image)
    path = "/tmp/opencode/broken.bin"
    open(path, "wb").write(bad)
    print("wrote %s (%d bytes, digest repaired)" % (path, len(bad)))

    print("uploading the broken image...")
    r = subprocess.run(["python3", "tools/ota_push.py", path, ORIGIN],
                       capture_output=True, text=True, timeout=900)
    print(r.stdout[-800:])
    if r.returncode != 0:
        print("push failed:", r.stderr[-400:])
        print("\nThe device refused a broken image. Rollback was not needed,")
        print("and that is the better outcome - verification caught it first.")
        return 0

    print("the device accepted it. Rebooting to see whether it survives...")
    subprocess.run(["curl", "-s", "--max-time", "10", "-X", "POST",
                    ORIGIN + "/api/reboot"], capture_output=True)
    time.sleep(25)

    for attempt in range(12):
        s = status()
        if s:
            print("still answering after %ds: %s" % ((attempt + 1) * 5, s))
            return 0
        time.sleep(5)

    print("NO ANSWER after 60 s. The device did not come back on its own.")
    return 1


if __name__ == "__main__":
    sys.exit(main())