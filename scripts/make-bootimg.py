#!/usr/bin/env python3
"""Android boot image v0 builder / verifier for Xiaomi Mi Pad 5 Pro (xiaomi-elish).

Header layout is the legacy v0 boot_img_hdr (1632 bytes), little-endian.
Nothing outside the Python standard library is used.

Addresses/pagesize come from pmaports device/testing/device-xiaomi-elish/deviceinfo:
    deviceinfo_flash_pagesize       = 4096
    deviceinfo_flash_offset_kernel  = 0x00008000
    deviceinfo_flash_offset_ramdisk = 0x01000000
    deviceinfo_flash_offset_tags    = 0x00000100
    deviceinfo_kernel_cmdline       = "quiet"

DTB handling (deviceinfo append_dtb=true): every --dtb is appended after the
kernel segment, each DTB starts on a 2048-byte boundary, and the whole
concatenation is reported as kernel_size, matching legacy mkbootimg.
"""

import argparse
import hashlib
import struct
import sys

MAGIC = b"ANDROID!"
HEADER_VERSION = 0
HEADER_FMT = "<8s10I16s512s32s1024s"
HEADER_SIZE = struct.calcsize(HEADER_FMT)  # 1632
DTB_ALIGN = 2048

DEFAULT_PAGE_SIZE = 4096
DEFAULT_KERNEL_ADDR = 0x00008000
DEFAULT_RAMDISK_ADDR = 0x01000000
DEFAULT_TAGS_ADDR = 0x00000100
# Device facts (docs/decisions.md D-013, measured 2026-10-24):
#   boot_b -> /dev/block/sde37, 393216 sectors * 512 = 192 MiB, so a boot image
#   larger than that can never be flashed; --max-size enforces it here.
#   The shipped cmdline follows the old vendor image and pins the rootfs by
#   PARTLABEL so it survives partition renumbering.
DEFAULT_CMDLINE = "quiet rootwait root=PARTLABEL=linux console=tty0 loglevel=4"
BOOT_PARTITION_LIMIT = 393216 * 512  # 201326592 bytes = 192 MiB
NAME_FIELD = 16
CMDLINE_FIELD = 512
EXTRA_CMDLINE_FIELD = 1024
ID_FIELD = 32


class ImageError(Exception):
    pass


def align_up(value, alignment):
    if alignment <= 0:
        raise ImageError("alignment must be positive")
    return (value + alignment - 1) // alignment * alignment


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def parse_int(text):
    return int(text, 0) if isinstance(text, str) else int(text)


def read_blob(path, what):
    try:
        with open(path, "rb") as handle:
            data = handle.read()
    except OSError as exc:
        raise ImageError("cannot read %s %r: %s" % (what, path, exc))
    if not data:
        raise ImageError("%s %r is empty" % (what, path))
    return data


def encode_fixed(text, width, field):
    raw = text.encode("utf-8")
    if len(raw) > width - 1:
        raise ImageError("%s too long: %d bytes, limit %d" % (field, len(raw), width - 1))
    return raw


def build_kernel_payload(kernel, dtbs, align=DTB_ALIGN):
    """Concatenate kernel and dtbs, each dtb starting on an aligned boundary."""
    payload = bytearray(kernel)
    for dtb in dtbs:
        pad = align_up(len(payload), align) - len(payload)
        payload.extend(b"\0" * pad)
        payload.extend(dtb)
    return bytes(payload)


def sha1_id(kernel_payload, ramdisk, second):
    digest = hashlib.sha1()
    digest.update(kernel_payload)
    digest.update(ramdisk)
    digest.update(second)
    return digest.digest()


def pack_header(kernel_payload, ramdisk, second, page_size, kernel_addr,
                ramdisk_addr, second_addr, tags_addr, cmdline, name,
                os_version, img_id):
    header = struct.pack(
        HEADER_FMT,
        MAGIC,
        len(kernel_payload),
        kernel_addr,
        len(ramdisk),
        ramdisk_addr,
        len(second),
        second_addr,
        tags_addr,
        page_size,
        HEADER_VERSION,
        os_version,
        encode_fixed(name, NAME_FIELD, "name"),
        encode_fixed(cmdline, CMDLINE_FIELD, "cmdline"),
        img_id.ljust(ID_FIELD, b"\0"),
        encode_fixed("", EXTRA_CMDLINE_FIELD, "extra_cmdline"),
    )
    if len(header) != HEADER_SIZE:
        raise ImageError("internal: header size %d != %d" % (len(header), HEADER_SIZE))
    return header


def render_image(header, kernel_payload, ramdisk, second, page_size):
    out = bytearray()
    out.extend(header + b"\0" * (page_size - len(header)))
    for blob in (kernel_payload, ramdisk, second):
        if not blob:
            continue
        out.extend(blob)
        out.extend(b"\0" * (align_up(len(blob), page_size) - len(blob)))
    if len(out) % page_size:
        raise ImageError("internal: image size not page aligned")
    return bytes(out)


def cmd_build(args):
    page_size = parse_int(args.page_size)
    if page_size < 2048 or page_size & (page_size - 1):
        raise ImageError("page size must be a power of two >= 2048, got %d" % page_size)

    kernel = read_blob(args.kernel, "kernel")
    ramdisk = read_blob(args.ramdisk, "ramdisk")
    if not ramdisk.startswith(b"\x1f\x8b"):
        print("warning: ramdisk does not start with gzip magic (0x1f8b)", file=sys.stderr)
    dtbs = [read_blob(path, "dtb") for path in (args.dtb or [])]

    kernel_payload = build_kernel_payload(kernel, dtbs)
    second = b""
    if args.second:
        second = read_blob(args.second, "second")
    img_id = sha1_id(kernel_payload, ramdisk, second)
    header = pack_header(
        kernel_payload, ramdisk, second, page_size,
        parse_int(args.kernel_addr), parse_int(args.ramdisk_addr),
        parse_int(args.second_addr),
        parse_int(args.tags_addr), args.cmdline, args.name,
        parse_int(args.os_version), img_id,
    )
    image = render_image(header, kernel_payload, ramdisk, second, page_size)

    max_size = parse_int(args.max_size)
    if max_size > 0 and len(image) > max_size:
        raise ImageError(
            "boot image is %d bytes, which exceeds the %d-byte (%d MiB) boot_b partition: "
            "compress the initramfs (gzip -9) or drop a dtb, then retry"
            % (len(image), max_size, max_size // (1024 * 1024)))

    with open(args.out, "wb") as handle:
        handle.write(image)

    print("build.out = %s" % args.out)
    print("build.page_size = %d" % page_size)
    print("build.kernel_input_size = %d" % len(kernel))
    print("build.dtb_count = %d" % len(dtbs))
    print("build.dtb_sizes = %s" % (",".join(str(len(d)) for d in dtbs) or "-"))
    print("build.kernel_segment_size = %d" % len(kernel_payload))
    print("build.ramdisk_size = %d" % len(ramdisk))
    print("build.image_size = %d" % len(image))
    print("build.size_limit = %d" % max_size)
    print("build.size_used_pct = %.1f" % (100.0 * len(image) / max_size if max_size else 0.0))
    print("sha256.kernel_input = %s" % sha256_bytes(kernel))
    print("sha256.kernel_segment = %s" % sha256_bytes(kernel_payload))
    print("sha256.ramdisk = %s" % sha256_bytes(ramdisk))
    print("sha256.image = %s" % sha256_bytes(image))
    for index, dtb in enumerate(dtbs, 1):
        print("sha256.dtb%d = %s" % (index, sha256_bytes(dtb)))
    return 0


def parse_image(data):
    if len(data) < HEADER_SIZE:
        raise ImageError("file too small for v0 header: %d bytes" % len(data))
    fields = struct.unpack(HEADER_FMT, data[:HEADER_SIZE])
    names = (
        "magic", "kernel_size", "kernel_addr", "ramdisk_size", "ramdisk_addr",
        "second_size", "second_addr", "tags_addr", "page_size",
        "header_version", "os_version", "name", "cmdline", "id", "extra_cmdline",
    )
    header = dict(zip(names, fields))
    if header["magic"] != MAGIC:
        raise ImageError("bad magic %r, expected %r" % (header["magic"], MAGIC))
    if header["page_size"] < 2048 or header["page_size"] & (header["page_size"] - 1):
        raise ImageError("page_size %d is not a power of two >= 2048" % header["page_size"])

    page = header["page_size"]
    kernel_off = page
    ramdisk_off = align_up(kernel_off + header["kernel_size"], page)
    second_off = align_up(ramdisk_off + header["ramdisk_size"], page)
    expected_size = align_up(second_off + header["second_size"], page)
    segments = {
        "kernel": (kernel_off, header["kernel_size"]),
        "ramdisk": (ramdisk_off, header["ramdisk_size"]),
        "second": (second_off, header["second_size"]),
    }
    return header, segments, expected_size


def check_padding(data, header, segments):
    problems = []
    page = header["page_size"]
    if data[HEADER_SIZE:page].strip(b"\0"):
        problems.append("header page padding is not zero")
    bounds = [
        (segments["kernel"][0] + header["kernel_size"], segments["ramdisk"][0]),
        (segments["ramdisk"][0] + header["ramdisk_size"], segments["second"][0]),
        (segments["second"][0] + header["second_size"], len(data)),
    ]
    for start, end in bounds:
        if end > len(data):
            problems.append("segment extends past end of file (padding %d..%d)" % (start, end))
            continue
        if data[start:end].strip(b"\0"):
            problems.append("inter-section padding %d..%d is not zero" % (start, end))
    return problems


def cmd_verify(args):
    with open(args.input, "rb") as handle:
        data = handle.read()
    header, segments, expected_size = parse_image(data)
    problems = []
    if header["header_version"] != HEADER_VERSION:
        problems.append("header_version is %d, this tool only writes v0" % header["header_version"])
    if len(data) != expected_size:
        problems.append("file size %d != page-aligned expected size %d" % (len(data), expected_size))
    max_size = parse_int(args.max_size)
    if max_size > 0 and len(data) > max_size:
        problems.append("file size %d exceeds the boot_b partition limit of %d bytes (%d MiB)"
                        % (len(data), max_size, max_size // (1024 * 1024)))
    problems.extend(check_padding(data, header, segments))

    cmdline = header["cmdline"].split(b"\0", 1)[0].decode("utf-8", "replace")
    extra = header["extra_cmdline"].split(b"\0", 1)[0].decode("utf-8", "replace")
    name = header["name"].split(b"\0", 1)[0].decode("utf-8", "replace")
    stored_id = header["id"]

    kernel_off, kernel_size = segments["kernel"]
    ramdisk_off, ramdisk_size = segments["ramdisk"]
    second_off, second_size = segments["second"]
    kernel_bytes = data[kernel_off:kernel_off + kernel_size]
    ramdisk_bytes = data[ramdisk_off:ramdisk_off + ramdisk_size]
    second_bytes = data[second_off:second_off + second_size]
    recomputed = sha1_id(kernel_bytes, ramdisk_bytes, second_bytes).ljust(ID_FIELD, b"\0")
    if recomputed != stored_id:
        problems.append("header id mismatch: stored %s recomputed %s"
                        % (stored_id.hex(), recomputed.hex()))

    print("header.magic = %s" % header["magic"].decode("ascii"))
    print("header.kernel_size = %d" % header["kernel_size"])
    print("header.kernel_addr = 0x%08x" % header["kernel_addr"])
    print("header.ramdisk_size = %d" % header["ramdisk_size"])
    print("header.ramdisk_addr = 0x%08x" % header["ramdisk_addr"])
    print("header.second_size = %d" % header["second_size"])
    print("header.second_addr = 0x%08x" % header["second_addr"])
    print("header.tags_addr = 0x%08x" % header["tags_addr"])
    print("header.page_size = %d" % header["page_size"])
    print("header.header_version = %d" % header["header_version"])
    print("header.os_version = 0x%08x" % header["os_version"])
    print("header.name = %s" % name)
    print("header.cmdline = %s" % cmdline)
    print("header.extra_cmdline = %s" % (extra or "-"))
    print("header.id = %s" % stored_id.hex())
    print("layout.kernel_offset = %d" % kernel_off)
    print("layout.ramdisk_offset = %d" % ramdisk_off)
    print("layout.second_offset = %d" % second_off)
    print("layout.file_size = %d" % len(data))
    print("layout.size_limit = %d" % parse_int(args.max_size))
    print("layout.file_size_page_aligned = %s" % ("yes" if len(data) % header["page_size"] == 0 else "no"))
    print("sha256.kernel_segment = %s" % sha256_bytes(kernel_bytes))
    print("sha256.ramdisk_segment = %s" % sha256_bytes(ramdisk_bytes))
    if second_size:
        print("sha256.second_segment = %s" % sha256_bytes(second_bytes))
    print("sha256.file = %s" % sha256_bytes(data))
    if problems:
        for problem in problems:
            print("verify.error = %s" % problem, file=sys.stderr)
        print("verify.status = FAIL")
        return 3
    print("verify.status = OK")
    return 0


def main(argv):
    parser = argparse.ArgumentParser(
        description="Build or verify an Android boot image v0 (xiaomi-elish).",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--kernel")
    parser.add_argument("--ramdisk")
    parser.add_argument("--dtb", action="append", default=[],
                        help="DTB appended after the kernel segment at 2048-byte alignment (repeatable)")
    parser.add_argument("--second")
    parser.add_argument("--cmdline", default=DEFAULT_CMDLINE)
    parser.add_argument("--name", default="")
    parser.add_argument("--page-size", default=DEFAULT_PAGE_SIZE)
    parser.add_argument("--kernel-addr", default=DEFAULT_KERNEL_ADDR)
    parser.add_argument("--ramdisk-addr", default=DEFAULT_RAMDISK_ADDR)
    parser.add_argument("--second-addr", default=0)
    parser.add_argument("--tags-addr", default=DEFAULT_TAGS_ADDR)
    parser.add_argument("--os-version", default=0)
    parser.add_argument("--max-size", default=BOOT_PARTITION_LIMIT,
                        help="hard cap in bytes; boot_b is 192 MiB (393216 sectors), 0 disables")
    parser.add_argument("--out")
    parser.add_argument("--verify", action="store_true", help="parse --input and dump every header field")
    parser.add_argument("--input")
    args = parser.parse_args(argv)

    try:
        if args.verify:
            if not args.input:
                parser.error("--verify requires --input")
            return cmd_verify(args)
        for required in ("kernel", "ramdisk", "out"):
            if not getattr(args, required):
                parser.error("--%s is required when building" % required)
        return cmd_build(args)
    except ImageError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
