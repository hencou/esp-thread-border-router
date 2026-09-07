#!/usr/bin/env python3
"""Pack the built images into a single OTA bundle for the Firmware page of the web GUI.

The bundle contains the application, the web GUI (web_storage) and the RCP firmware (rcp_fw), so
one upload updates everything that changes between releases. Layout:

    offset 0   magic "OTBRBNDL"          8 bytes
    offset 8   version (little endian)   uint32
    offset 12  entry count               uint32
    offset 16  entry[count]:
                   target partition      16 bytes, NUL terminated ("app" for the OTA slot)
                   size                  uint32
                   sha256                32 bytes
    payload    the images, in the order of the entries

Usage:
    ./tools/make_ota_bundle.py [--build-dir build] [-o esp_ot_br_ota.bin]
"""

import argparse
import hashlib
import os
import struct
import sys

MAGIC = b"OTBRBNDL"
VERSION = 1
TARGET_LEN = 16
ENTRY_STRUCT = struct.Struct("<%dsL32s" % TARGET_LEN)
HEADER_STRUCT = struct.Struct("<8sLL")

# (target partition, file name in the build directory, required)
IMAGES = [
    ("app", "esp_ot_br.bin", True),
    ("web_storage", "web_storage.bin", False),
    ("rcp_fw", "rcp_fw.bin", False),
]


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(65536), b""):
            digest.update(block)
    return digest.digest()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", default="build", help="directory with the built images")
    parser.add_argument("-o", "--output", default="esp_ot_br_ota.bin", help="bundle to write")
    args = parser.parse_args()

    entries = []
    for target, filename, required in IMAGES:
        path = os.path.join(args.build_dir, filename)
        if not os.path.isfile(path):
            if required:
                sys.exit("ERROR: %s is missing, run 'idf.py build' first" % path)
            print("Skipping %s: %s is missing" % (target, path))
            continue
        if len(target) >= TARGET_LEN:
            sys.exit("ERROR: partition name '%s' does not fit in %d bytes" % (target, TARGET_LEN))
        entries.append((target, path, os.path.getsize(path), sha256_of(path)))

    with open(args.output, "wb") as out:
        out.write(HEADER_STRUCT.pack(MAGIC, VERSION, len(entries)))
        for target, _, size, digest in entries:
            out.write(ENTRY_STRUCT.pack(target.encode(), size, digest))
        for target, path, size, _ in entries:
            with open(path, "rb") as handle:
                for block in iter(lambda: handle.read(65536), b""):
                    out.write(block)
            print("Added %-12s %8d bytes" % (target, size))

    print("\nBundle: %s (%d bytes)" % (args.output, os.path.getsize(args.output)))
    print("Upload it on the Firmware page of the border router web GUI.")


if __name__ == "__main__":
    main()
