#!/usr/bin/env python3
"""
Put a bare-metal program in front of Android's kernel in a boot image (header v2, v3, or v4).

  chainboot.py <android-boot.img> <program.bin> <out.img> [payload.bin]

The bootloader decompresses the kernel and enters it at its load address. The new kernel is gzip of:
program.bin, padded to CHAIN_OFFSET, then Android's own (uncompressed) Image. The program runs
first and then moves the Image down to the base address and starts it (loader/fbdiag.c). An optional
payload (qcomloader's "XNUP") follows the Image, 16-aligned. The program's own arm64 header gets
the Image's length in res2 (0x28), the payload's offset in res3 (0x30) and a covering image_size.
Everything else - header fields, cmdline, ramdisk, dtb/signature - is copied from the Android image.
"""
import gzip
import hashlib
import os
import struct
import subprocess
import sys

CHAIN_OFFSET = 0x40000
BOOT_PARTITION_V2 = 32 << 20
BOOT_PARTITION_V34 = 96 << 20


def pad(b, n):
    return b + b'\0' * (-len(b) % n)


def lz4_decompress_block(src, out):
    i, n = 0, len(src)
    while i < n:
        tok = src[i]; i += 1
        lit = tok >> 4
        if lit == 15:
            while src[i] == 255:
                lit += 255; i += 1
            lit += src[i]; i += 1
        out.extend(src[i:i + lit]); i += lit
        if i >= n:
            break
        off = src[i] | (src[i + 1] << 8); i += 2
        if off == 0 or off > len(out):
            raise ValueError(f'invalid lz4 offset {off}')
        mlen = (tok & 0xf) + 4
        if mlen == 19:
            while src[i] == 255:
                mlen += 255; i += 1
            mlen += src[i]; i += 1
        start = len(out) - off
        if off >= mlen:
            out.extend(out[start:start + mlen])
        else:
            for j in range(mlen):
                out.append(out[start + j])


def lz4_decompress(data):
    magic = data[:4]
    out = bytearray()
    if magic == b'\x02\x21\x4c\x18':
        pos, n = 4, len(data)
        while pos + 4 <= n:
            bsize = struct.unpack_from('<I', data, pos)[0]
            pos += 4
            if bsize == 0 or bsize == 0x184c2102:
                continue
            lz4_decompress_block(data[pos:pos + bsize], out)
            pos += bsize
        return bytes(out)
    if magic == b'\x04\x22\x4d\x18':
        flg, bd = data[4], data[5]
        c_size = (flg >> 3) & 1
        b_csum = (flg >> 4) & 1
        d_id = flg & 1
        pos = 6 + (8 if c_size else 0) + (4 if d_id else 0) + 1
        n = len(data)
        while pos + 4 <= n:
            bsize = struct.unpack_from('<I', data, pos)[0]
            pos += 4
            if bsize == 0:
                break
            raw = bool(bsize & 0x80000000)
            bsize &= 0x7fffffff
            if raw:
                out.extend(data[pos:pos + bsize])
            else:
                lz4_decompress_block(data[pos:pos + bsize], out)
            pos += bsize + (4 if b_csum else 0)
        return bytes(out)
    return subprocess.check_output(['lz4', '-d', '-c'], input=data)


def decompress_kernel(kernel):
    if kernel[:2] == b'\x1f\x8b':
        return gzip.decompress(kernel)
    if kernel[:4] in (b'\x02\x21\x4c\x18', b'\x04\x22\x4d\x18'):
        try:
            return lz4_decompress(kernel)
        except Exception:
            return subprocess.check_output(['lz4', '-d', '-c'], input=kernel)
    return kernel


def main():
    src, prog_path, out = sys.argv[1:4]
    payload = open(sys.argv[4], 'rb').read() if len(sys.argv) > 4 else b''
    d = open(src, 'rb').read()
    assert d[:8] == b'ANDROID!', 'not an Android boot image'
    hver = struct.unpack_from('<I', d, 40)[0]
    assert hver in (2, 3, 4), f'header v{hver}, expected 2, 3, or 4'

    if hver == 2:
        ksize, _, rsize, _, ssize, _, _, page, _ = struct.unpack_from('<9I', d, 8)
        rdo_size = struct.unpack_from('<I', d, 1632)[0]
        dtb_size = struct.unpack_from('<I', d, 1648)[0]
    else:
        page = 4096
        ksize, rsize, os_version, hdr_size = struct.unpack_from('<4I', d, 8)
        sig_size = struct.unpack_from('<I', d, 1580)[0] if (hver == 4 and len(d) >= 1584) else 0

    def pages(n):
        return (n + page - 1) // page * page

    off = page
    kernel = d[off:off + ksize]; off += pages(ksize)
    ramdisk = d[off:off + rsize]; off += pages(rsize)
    if hver == 2:
        second = d[off:off + ssize]; off += pages(ssize)
        rdo = d[off:off + rdo_size]; off += pages(rdo_size)
        dtb = d[off:off + dtb_size]
    else:
        sig = d[off:off + sig_size] if sig_size else b''

    image = decompress_kernel(kernel)
    assert image[0x38:0x3c] == b'ARM\x64', 'Android kernel is not an arm64 Image'
    linux_image_size = struct.unpack_from('<Q', image, 0x10)[0]

    prog = bytearray(open(prog_path, 'rb').read())
    assert prog[0x38:0x3c] == b'ARM\x64', 'program has no arm64 header'
    assert len(prog) <= CHAIN_OFFSET, 'program too large'
    payload_off = CHAIN_OFFSET + len(pad(image, 16)) if payload else 0
    total = max(CHAIN_OFFSET + linux_image_size, payload_off + len(payload))
    struct.pack_into('<Q', prog, 0x10, (total + 0xfffff) & ~0xfffff)
    struct.pack_into('<Q', prog, 0x28, len(image))
    struct.pack_into('<Q', prog, 0x30, payload_off)
    blob = bytes(prog) + b'\0' * (CHAIN_OFFSET - len(prog)) + image
    if payload:
        blob = pad(blob, 16) + payload
    new_kernel = gzip.compress(blob, 9, mtime=0)

    hdr = bytearray(d[:page])
    struct.pack_into('<I', hdr, 8, len(new_kernel))
    if hver == 2:
        sha = hashlib.sha1()
        for part in (new_kernel, ramdisk, second, rdo, dtb):
            sha.update(part)
            sha.update(struct.pack('<I', len(part)))
        hdr[576:608] = pad(sha.digest(), 32)
        img = (bytes(hdr) + pad(new_kernel, page) + pad(ramdisk, page) +
               pad(second, page) + pad(rdo, page) + pad(dtb, page))
        max_boot = int(os.environ.get('HAKIOS_BOOT_SIZE', BOOT_PARTITION_V2))
    else:
        img = (bytes(hdr) + pad(new_kernel, page) + pad(ramdisk, page) +
               (pad(sig, page) if sig_size else b''))
        max_boot = int(os.environ.get('HAKIOS_BOOT_SIZE', BOOT_PARTITION_V34))

    # boot_a on the first phone is 32 MB (96 MB on GKI v3/v4): an image any larger is refused by
    # fastboot, and a script that sends fastboot's output nowhere would carry on as if it had been flashed
    if len(img) > max_boot:
        raise SystemExit(f'{out}: {len(img)} bytes, more than the {max_boot} of the boot partition '
                         f'({len(img) - max_boot} too many): the payload has to shrink')
    open(out, 'wb').write(img)
    print(f'{out}: {len(img)} bytes (program {len(prog)}, Image {len(image)}, payload {len(payload)}, kernel gz {len(new_kernel)})')


if __name__ == '__main__':
    main()
