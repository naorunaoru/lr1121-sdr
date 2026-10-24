# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import Mock, patch
spec = importlib.util.spec_from_file_location('tx_vectors', Path(__file__).resolve().parents[1]/'tx/vectors.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


class VectorTests(unittest.TestCase):
    def test_layout_and_tested_handler_map(self):
        values = m.vectors()
        self.assertEqual(len(values), 60)
        self.assertEqual(values[16], 0x95210)
        self.assertEqual(values[1:16], [0x95090]*15)
        # Filled below from the independently retained tested vector map.
        self.assertEqual(hashlib.sha256(struct.pack('<60I', *values)).hexdigest(), EXPECTED_SHA)
        self.assertEqual(m.OFFSET, 0x11ff0)

    def test_fragment_validation(self):
        cipher = bytes(256)
        uid = '0123456789abcdef'
        meta = dict(address=m.ADDRESS, offset=m.OFFSET, uid=uid, vectors=m.vectors(),
                    verified=True, cipher_sha256=m.fw.digest(cipher))
        self.assertEqual(m.validated(cipher, meta, uid), cipher)
        for key in meta:
            with self.assertRaises(ValueError): m.validated(cipher, dict(meta, **{key:None}), uid)
        with self.assertRaises(ValueError): m.validated(cipher, meta, 'fedcba9876543210')
        with self.assertRaises(ValueError): m.validated(cipher[:-1], meta, uid)

    def test_identity_failure_never_enters_bootloader(self):
        b = Mock()
        b.version.return_value = {'data':'22030101'}
        with tempfile.TemporaryDirectory() as directory, patch.object(m.fw, 'enter_bootloader') as enter:
            with self.assertRaises(RuntimeError): m.install(b, Path(directory), bytes(256), {})
            enter.assert_not_called()
            b.write_command.assert_not_called()

    def test_report_stays_failed_if_post_verification_boot_check_fails(self):
        b = Mock()
        with tempfile.TemporaryDirectory() as directory, patch.object(m, 'identity', return_value='uid'), \
                patch.object(m, 'validated'), patch.object(m, 'verify', return_value={'success':True}), \
                patch.object(m.fw, 'checked_write'), patch.object(m.time, 'sleep'), \
                patch.object(m.fw, 'enter_bootloader', side_effect=[m.fw.BOOT, RuntimeError('lost')]):
            out = Path(directory)
            with self.assertRaises(RuntimeError): m.install(b, out, bytes(256), {})
            self.assertFalse(json.loads((out/'vector-install.json').read_text())['success'])


EXPECTED_SHA = 'd3f230a637775a3706fa403f949682768d6860245ae281c8422b692b67405476'
if __name__ == '__main__': unittest.main()
