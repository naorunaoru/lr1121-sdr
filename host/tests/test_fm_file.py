# SPDX-License-Identifier: MIT
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import fm_file

class FMFileTests(unittest.TestCase):
    def test_hz_roundtrip_and_scale(self):
        values=[-75000.,0.,75000.,1.]*1024
        raw,meta=fm_file.encode(struct.pack('<4096f',*values),220000)
        self.assertEqual(meta['offset_shift'],2)
        result=struct.unpack('>4096h',raw)
        self.assertEqual(result[:4],(-19661,0,19661,0))
        self.assertLessEqual(meta['max_quantization_error_hz'],meta['offset_step_hz']/2)
        self.assertAlmostEqual(meta['rate_actual'],32000000/145)
    def test_rejects_nonfinite_clipping_partial_banks(self):
        for raw in (b'x',b'\0'*16380,struct.pack('<4096f',*([float('nan')]*4096)),
                    struct.pack('<4096f',*([float('inf')]*4096))):
            with self.assertRaises(ValueError):fm_file.encode(raw,250000)
        with self.assertRaises(ValueError):fm_file.encode(struct.pack('<4096f',*([75000.]*4096)),250000,1)

if __name__=='__main__':unittest.main()
