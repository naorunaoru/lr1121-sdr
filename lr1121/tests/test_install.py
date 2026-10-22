import importlib.util
import json
from pathlib import Path
import sys
import unittest
import tempfile
from unittest.mock import Mock, patch
spec=importlib.util.spec_from_file_location('installer',Path(__file__).resolve().parents[1]/'install.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

class ImageGuards(unittest.TestCase):
    def test_stock_digest(self):
        self.assertEqual(len(m.stock_image(m.VENDOR)),66592)
    def test_layout(self):
        for size in (0,4081):
            with self.assertRaises(ValueError):m.payload_layout(bytes(size))
        code=bytearray(512);code[240]=1
        with self.assertRaises(ValueError):m.payload_layout(code)
        self.assertEqual(len(m.payload_layout(bytes(4080))),4096)
    def test_manifest_and_prefix(self):
        original=m.stock_image(m.VENDOR);code=bytes(260)
        image=original+bytes([0xaa])*(m.BASE-m.FLASH_BASE-len(original))+bytes(len(m.payload_layout(code)))
        meta=dict(verified=True,entry=hex(m.ENTRY),vendor_sha256=m.digest(original),
                  payload_sha256=m.digest(code),image_sha256=m.digest(image),bytes=len(image))
        self.assertEqual(m.validated_custom(original,image,meta,code),image)
        for k in meta:
            bad=dict(meta);bad[k]=None
            with self.assertRaises(ValueError):m.validated_custom(original,image,bad,code)
        with self.assertRaises(ValueError):m.validated_custom(original,b'X'+image[1:],meta,code)
        with self.assertRaises(ValueError):m.validated_custom(original,image,meta,b'X'+code[1:])
    def test_bootloader_identity_before_erase(self):
        b=Mock();b.version.return_value={'data':'unknown'}
        with self.assertRaises(RuntimeError):m.enter_bootloader(b)
        b.write_command.assert_not_called()
        b.version.return_value={'data':m.BOOT}
        self.assertEqual(m.enter_bootloader(b),m.BOOT);b.write_command.assert_not_called()
    def test_failed_verify_removes_stale_binding(self):
        b = Mock()
        b.version.return_value = {'data': 'unsupported'}
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            binding = out / 'binding.cmake'
            binding.write_text('stale')
            with self.assertRaises(RuntimeError):
                m.verify(b, out)
            self.assertFalse(binding.exists())
            self.assertFalse(json.loads((out / 'verify.json').read_text())['success'])

    def test_flash_uses_only_application_commands(self):
        b = Mock()
        b.version.side_effect = [{'data': x} for x in
                                 (m.APP, m.BOOT, m.APP, m.APP, m.BOOT)]
        b.status.return_value = {'decoded': {'cmd_status': 2}}
        with tempfile.TemporaryDirectory() as directory, patch.object(m.time, 'sleep'):
            out = Path(directory)
            m.flash(b, bytes(512), out)
            self.assertTrue(json.loads((out / 'flash-result.json').read_text())['complete'])
        commands = [call.args[0] for call in b.write_command.call_args_list]
        self.assertEqual([c[:2].hex() for c in commands],
                         ['0118', '8000', '8003', '8003', '8005', '0118', '8005'])
        self.assertEqual(commands[2][2:6], bytes(4))
        self.assertEqual(commands[3][2:6], (256).to_bytes(4, 'big'))

    def test_word_order(self):
        self.assertEqual(m.swap_words(bytes(range(8))),bytes([3,2,1,0,7,6,5,4]))
        with self.assertRaises(ValueError):m.swap_words(b'123')
    def test_ram_guard(self):
        b=Mock()
        for a,n in ((0x7ffffc,1),(0x80fffc,2),(0x800001,1),(0x800000,65)):
            with self.assertRaises(ValueError):m.read_words(b,a,n)
        b.read_command.assert_not_called()

if __name__=='__main__':unittest.main()
