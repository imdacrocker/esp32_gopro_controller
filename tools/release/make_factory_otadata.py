#!/usr/bin/env python3
"""Generate an ota_data_initial.bin that selects ota_0 on first boot.

ESP-IDF generates an all-0xFF ota_data_initial.bin by default. When otadata
is blank AND a factory partition exists, the bootloader logs "Defaulting
to factory image" and boots factory — which in this project is the recovery
app, not the wireless app users expect to land in.

This helper overwrites that file with a valid otadata entry pointing at
ota_0 (wireless app). Recovery remains reachable via the bootloader's normal
fallback path and via /api/ota/reboot-recovery.

Sector layout per IDF (4 KB each, two redundant copies):
  bytes [0:4]    ota_seq        — LE uint32; chosen slot = (seq-1) % num_ota
  bytes [4:24]   seq_label      — diagnostic only, left 0xFF
  bytes [24:28]  ota_state      — 0xFFFFFFFF (UNDEFINED) boots normally
  bytes [28:32]  crc            — crc32(packed_seq, init=0xFFFFFFFF)

We use UNDEFINED rather than ESP_OTA_IMG_VALID (2) so the first boot does
not arm the PENDING_VERIFY rollback dance — there's no prior commit to
verify on a freshly-provisioned board.
"""

import binascii
import struct
import sys

OTADATA_SIZE = 0x2000


def main() -> int:
    if len(sys.argv) != 2:
        sys.stderr.write(f"usage: {sys.argv[0]} <output-path>\n")
        return 2

    buf = bytearray(b"\xFF" * OTADATA_SIZE)
    seq = 1  # selects ota_0
    struct.pack_into("<I", buf, 0, seq)
    crc = binascii.crc32(struct.pack("<I", seq), 0xFFFFFFFF) & 0xFFFFFFFF
    struct.pack_into("<I", buf, 28, crc)
    # Second 4 KB sector left 0xFF (older copy, invalid — ignored).

    with open(sys.argv[1], "wb") as f:
        f.write(buf)
    return 0


if __name__ == "__main__":
    sys.exit(main())
