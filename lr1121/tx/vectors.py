#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Install the TX player's separate vector fragment; LR1121 transceiver 1.3 only.

CBC construction uses the existing MIT-attributed installer. Writes only the
256-byte application fragment at 0x95ff0. No bootloader update or erase opcode.
"""
import argparse
import json
from pathlib import Path
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'lr1121'))
import install as fw
ADDRESS = 0x95ff0
OFFSET = ADDRESS - fw.FLASH_BASE


def vectors():
    # Transceiver 1.3 handler-address map, not executable vendor code.
    native = [0x840f8] + [0x928f8]*15 + [
        0x8aa98, 0x8a4d0, 0x8a96c, 0x8a58c, 0x8a59c, 0x8a73c,
        *range(0x8a814, 0x8a854, 4),
        0x8aab0, 0x8aa94, 0x8a9f8, 0x8a934, 0x8aaac, 0x8aa14,
        0x8a930, 0x8a968, 0x8a900, 0x8a498, 0x8a598, 0x8a938,
        0x8a4b8, 0x8a49c, 0x8a4d4, 0x8a990, 0x8a854, 0x8a8fc,
    ] + [0x928f8]*4
    native[1:16] = [0x95090]*15
    native[16] = 0x95210
    return native


def identity(b):
    if b.version()['data'] != fw.APP:
        raise RuntimeError('Vector operation requires transceiver 1.3')
    return b._payload(b.read_command(bytes.fromhex('0125'), 8), 8).hex()


def validated(cipher, meta, uid):
    if (len(cipher) != 256 or meta.get('address') != ADDRESS or
            meta.get('offset') != OFFSET or meta.get('uid') != uid or
            meta.get('vectors') != vectors() or meta.get('verified') is not True or
            meta.get('cipher_sha256') != fw.digest(cipher)):
        raise ValueError('Vector fragment/manifest/device mismatch; no write sent')
    return cipher


def verify(b):
    b.reset()
    identity(b)
    fw.Core1121(b).initialize()
    fw.mask(b, 0x80f500, 0)
    fw.mask(b, 0x8004ac, 0x95010)
    fw.mask(b, 0x8004bc, 2)
    if fw.word(b, 0x80f500) != 0x46535435:
        raise RuntimeError('TX player ABI mismatch')
    actual = list(struct.unpack('>60I', fw.read_words(b, 0x80d000, 60)))
    if actual != vectors():
        raise RuntimeError('Installed vector readback mismatch; do not start TX')
    return dict(success=True, vector_base='0x96000')


def forge(b, out):
    b.speed(500000)
    b.reset()
    uid = identity(b)
    wanted = bytes(16) + struct.pack('<60I', *vectors())
    cipher = bytearray(256)
    previous = bytes([0xaa])*16
    try:
        for offset in range(240, 0, -16):
            cipher[offset:offset+16] = previous
            previous = fw.swap_words(bytes(x ^ y for x, y in
                                            zip(fw.oracle(b, previous), wanted[offset:offset+16])))
        cipher[:16] = previous
        iv = OFFSET.to_bytes(4, 'little') + bytes(12)
        plain = fw.oracle(b, cipher[:128], iv) + fw.oracle(b, cipher[128:], cipher[112:128])
        if plain[16:] != wanted[16:]:
            raise RuntimeError('Vector plaintext verification failed')
        (out/'vectors.encrypted.bin').write_bytes(cipher)
        fw.save(out/'vectors.json', dict(address=ADDRESS, offset=OFFSET, uid=uid,
                cipher_sha256=fw.digest(cipher), vectors=vectors(), verified=True))
    finally:
        b.reset()


def install(b, out, cipher, meta):
    b.speed(500000)
    b.reset()
    validated(cipher, meta, identity(b))
    report = dict(success=False)
    try:
        report['boot_before'] = fw.enter_bootloader(b)
        fw.checked_write(b, bytes.fromhex('8003') + OFFSET.to_bytes(4, 'big') + cipher)
        b.write_command(bytes.fromhex('800500'))
        time.sleep(.1)
        report['vector_readback'] = verify(b)
        report['boot_after'] = fw.enter_bootloader(b)
        b.write_command(bytes.fromhex('800500'))
        time.sleep(.1)
        report['success'] = report['boot_before'] == report['boot_after'] == fw.BOOT
        if not report['success']:
            raise RuntimeError('Bootloader identity mismatch')
    finally:
        try:
            b.reset()
        except Exception as exc:
            report['cleanup_error'] = repr(exc)
        fw.save(out/'vector-install.json', report)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action', choices=['forge', 'install', 'verify'])
    p.add_argument('--port')
    p.add_argument('--out', type=Path, default=ROOT/'build/lr1121-tx/vectors')
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    if a.action == 'install':
        cipher = (a.out/'vectors.encrypted.bin').read_bytes()
        meta = json.loads((a.out/'vectors.json').read_text())
        validated(cipher, meta, meta.get('uid'))  # Check local inputs before USB.
    with fw.Bridge(port=a.port, timeout_ms=10000, log=a.out/(a.action+'.jsonl')) as b:
        if a.action == 'forge':
            forge(b, a.out)
        elif a.action == 'install':
            install(b, a.out, cipher, meta)
        else:
            try:
                print(json.dumps(verify(b)))
            finally:
                b.reset()


if __name__ == '__main__':
    main()
