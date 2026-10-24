# SPDX-License-Identifier: MIT
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import tx


class PlaybackTests(unittest.TestCase):
    def test_requested_rate_quantization_and_actual_deadline(self):
        timing = tx.timing_settings(220000)
        self.assertEqual(timing['period_cycles'], 145)
        self.assertAlmostEqual(timing['rate_actual'], 32000000/145)
        for rate in (20000, 44100, 123456.7, 220000, 250000):
            t=tx.timing_settings(rate)
            self.assertLessEqual(abs(t['period_cycles']-32000000/rate), .5)
        for rate in (True, 0, 19999, 250001, float('nan'), float('inf'), '200000'):
            with self.assertRaises(ValueError): tx.timing_settings(rate)
        # Requested duration <1 s; quantized hardware duration >1 s.
        with self.assertRaises(ValueError): tx.validate([0]*53248, 53250, 1000)

    def test_normalized_range_and_expanded_word_bounds(self):
        self.assertEqual(tx.resolve_shift(None, 75000), 2)
        self.assertEqual(tx.resolve_shift(None, 250000), 4)  # positive endpoint needs headroom
        scaled=tx.scale_samples([-32768,0,32767],75000,2)
        self.assertEqual(scaled, [-19661,0,19661])
        for x in (True,float('nan'),float('inf'),-1):
            with self.assertRaises(ValueError): tx.resolve_shift(None,x)
        with self.assertRaises(ValueError): tx.scale_samples([0],75000,1)
        with self.assertRaises(ValueError): tx.scale_samples([32768],2500,1)
        for shift in (0,16,True,1.5):
            with self.assertRaises(ValueError): tx.offset_encoding(shift)
        for shift in range(1,15):
            setting=tx.radio_settings(offset_shift=shift)
            encoded=[-32768,32767]*1024
            expected=sum(tx.BASE_WORD+(x<<shift) for x in encoded)&0xffffffff
            self.assertEqual(tx.checksum(encoded,tx.BASE_WORD,shift),expected)
            self.assertEqual(struct.unpack('>II',tx.frame(7,encoded,tx.BASE_WORD,shift)[:8]),(7,expected))
        with self.assertRaises(ValueError): tx.radio_settings(offset_shift=15)
        self.assertEqual(tx.radio_settings(2440000000,offset_shift=15)['offset_shift'],15)

    def test_invalid_modulation_never_opens_device(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(tx, 'Bridge') as bridge:
            out=Path(directory)/'trial'
            for kwargs in ({'rate':float('nan')}, {'offset_shift':16},
                           {'deviation_hz':75000,'offset_shift':1},
                           {'deviation_hz':float('inf')}, {'offset_shift':15}):
                args=dict(rate=200000,timeout_ms=100,out=out)
                args.update(kwargs)
                with self.assertRaises(ValueError): tx.play([0]*6144,**args)
            bridge.assert_not_called()
            self.assertFalse(out.exists())

    def test_frequency_and_pa_profiles(self):
        self.assertEqual(tx.radio_settings()['center_word'], tx.BASE_WORD)
        self.assertEqual(tx.radio_settings()['pa_config'], [1, 1, 4, 7])
        for pa, low, high in [('lp',-17,14),('hp',-9,22),('hf',-18,13)]:
            for power in (low, high):
                self.assertEqual(tx.radio_settings(868350000, pa, power)['power_dbm_requested'], power)
            for power in (low-1, high+1, True, 0.5):
                with self.assertRaises(ValueError): tx.radio_settings(868350000, pa, power)
        self.assertEqual(tx.radio_settings(2440000000)['pa'], 'hf')
        self.assertEqual(tx.radio_settings(433920000)['image_calibration'], [108,109])
        self.assertIsNone(tx.radio_settings(2440000000)['image_calibration'])
        for frequency in (0, 1, 62499, 4096000000, -433920000, True, 868350000.5):
            with self.assertRaises(ValueError): tx.radio_settings(frequency)
        for frequency in (100000000, 433920000, 868350000, 1200000000, 2440000000):
            settings=tx.radio_settings(frequency)
            base=settings['center_word']
            self.assertTrue(base & 1)
            self.assertGreater(base-65536, 0)
            self.assertLessEqual(base+65534, 0xffffffff)
            offsets=[-32768,32767]*1024
            self.assertEqual(tx.checksum(offsets,base), sum(base+2*x for x in offsets)&0xffffffff)
            self.assertEqual(struct.unpack('>II',tx.frame(4,offsets,base)[:8]),(4,tx.checksum(offsets,base)))
        with self.assertRaises(ValueError): tx.radio_settings(pa='other')
        with self.assertRaises(ValueError): tx.radio_settings(ramp_us=17)

    def test_invalid_power_does_not_open_device(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(tx, 'Bridge') as bridge:
            out=Path(directory)/'trial'
            with self.assertRaises(ValueError):
                tx.play([0]*6144,rate=200000,timeout_ms=100,out=out,pa='hf',power_dbm=22)
            bridge.assert_not_called()
            self.assertFalse(out.exists())

    def test_pair_order_and_checksum(self):
        self.assertEqual(tx.packed([-32768, 32767, -1, 0]), bytes.fromhex('7fff80000000ffff'))
        offsets = [-32768, 32767]*1024
        f = tx.frame(3, offsets)
        self.assertEqual(len(f), 4104)
        self.assertEqual(struct.unpack('>II', f[:8]), (3, sum(tx.BASE_WORD+2*x for x in offsets) & 0xffffffff))
        self.assertEqual(f[8:12], bytes.fromhex('7fff8000'))
        for values in ([1], [0, 32768], [-32769, 0]):
            with self.assertRaises(ValueError): tx.packed(values)

    def test_limits_before_device_open(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(tx, 'Bridge') as bridge:
            out = Path(directory)/'trial'
            for offsets, rate, timeout in [([0]*6144, 200000, 10), ([0]*6144, 300000, 100),
                                            ([0]*6144, 200000, 60001), ([0]*4097, 200000, 100),
                                            ([32768]*6144, 200000, 100)]:
                with self.assertRaises(ValueError):
                    tx.play(offsets, rate=rate, timeout_ms=timeout, out=out)
            bridge.assert_not_called()
            self.assertFalse(out.exists())
        tx.validate([0]*4096, 200000, 100, True)
        tx.validate([0]*6144, 250000, 100)
        with self.assertRaises(ValueError): tx.validate([0]*4096, 200000, 100)
        with self.assertRaises(ValueError): tx.validate([0]*4096+[1]*4096, 200000, 100, True)

    def test_wrong_firmware_preserves_failure_when_reset_fails(self):
        b = Mock()
        b.info.return_value = {'firmware':'1.0.8'}
        b.reset.side_effect = RuntimeError('USB lost')
        with tempfile.TemporaryDirectory() as directory, patch.object(tx, 'Bridge', return_value=b):
            out = Path(directory)/'trial'
            with self.assertRaisesRegex(RuntimeError, 'TX-only'):
                tx.play([0]*6144, rate=200000, timeout_ms=100, out=out)
            row = json.loads((out/'result.json').read_text())
            self.assertFalse(row['completed'])
            self.assertIn('TX-only', row['exception'])
            self.assertIn('USB lost', row['cleanup_error'])
            b.request.assert_not_called()
            b.close.assert_called_once()

    def test_failed_binary_write_closes_without_ascii_reset(self):
        b = Mock()
        b.info.return_value = {'firmware':'1.0.8-txopt5'}
        b.version.return_value = {'data':'22030103'}
        b.speed.return_value = {'spi_hz':15625000}
        b.request.return_value = {'id':10, 'ready':4104, 'ok':True}
        b.io.write.side_effect = OSError('USB lost')
        def memory(_b, address, count=1):
            if address == 0x80f500: return [0x46535434]
            if address == 0xf30058: return [tx.BASE_WORD]
            if address == 0x80d000: return tx.vectors()
            if address == 0x80c000: return list(struct.unpack('>2048I', tx.packed([0]*4096)))
            raise AssertionError('Unexpected read')
        with tempfile.TemporaryDirectory() as directory, patch.object(tx, 'Bridge', return_value=b), \
                patch.object(tx, 'configure'), patch.object(tx, 'put'), patch.object(tx, 'mask'), \
                patch.object(tx, 'fill'), patch.object(tx, 'read', side_effect=memory):
            out = Path(directory)/'trial'
            with self.assertRaises(OSError):
                tx.play([0]*6144, rate=200000, timeout_ms=100, out=out)
            row = json.loads((out/'result.json').read_text())
            self.assertFalse(row['completed'])
            self.assertIn('no ASCII reset', row['cleanup'])
            self.assertEqual(b.reset.call_count, 1)  # setup only
            b.close.assert_called_once()

    def test_report_write_failure_still_resets_and_closes(self):
        b = Mock()
        b.info.return_value = {'firmware':'unsupported'}
        with tempfile.TemporaryDirectory() as directory, patch.object(tx, 'Bridge', return_value=b), \
                patch.object(tx, 'save', side_effect=[OSError('disk full'), None]):
            with self.assertRaisesRegex(OSError, 'disk full'):
                tx.play([0]*6144, rate=200000, timeout_ms=100, out=Path(directory)/'trial')
            b.reset.assert_called_once()
            b.close.assert_called_once()

    def test_response_identity_and_timeout(self):
        b = Mock()
        for response in [b'{"id":7,"ok":true}\n', b'{"id":8,"ok":1}\n']:
            b.io.read_until.return_value = response
            with self.assertRaises(RuntimeError): tx.read_reply(b, 8, tx.time.monotonic()+1)
        with self.assertRaises(TimeoutError): tx.read_reply(b, 8, tx.time.monotonic()-1)


if __name__ == '__main__': unittest.main()
