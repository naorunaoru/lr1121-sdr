# SPDX-License-Identifier: MIT
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from bridge import Bridge
from lr1121 import Core1121, RadioError


class FakeRadioBridge:
    _payload = staticmethod(Bridge._payload)

    def __init__(self, mode=4, fail_opcode=None, errors="0x0000"):
        self.mode = mode
        self.fail_opcode = fail_opcode
        self.error_flags = errors
        self.command_status = 2
        self.commands = []

    def info(self):
        return {"bridge": "lr1121-spi"}

    def version(self):
        return {"version": {"is_lr1121_transceiver": True}}

    def reset(self):
        self.commands.append(b"RESET")
        self.mode = 1
        return {"ok": True}

    def errors(self):
        return {"error_flags": self.error_flags}

    def status(self):
        return {"decoded": {"chip_mode": self.mode, "cmd_status": self.command_status}}

    def write_command(self, data):
        self.commands.append(data)
        self.command_status = 0 if data[:2] == self.fail_opcode else 2
        if self.command_status == 2:
            if data[:2] == bytes.fromhex("011c"):
                self.mode = 1 + data[2]
            elif data[:2] == bytes.fromhex("0209"):
                self.mode = 4
        return {"ok": True}

    def read_command(self, command, length):
        self.commands.append(command)
        assert command == bytes.fromhex("0205") and length == 1
        return {"status": 6, "cmd_status": 3, "data": "b5"}


class RadioTests(unittest.TestCase):
    def test_half_db_precision(self):
        b = FakeRadioBridge()
        result = Core1121(b).sample_rssi(1, 0)
        self.assertEqual(result["samples"][0]["raw"], 181)
        self.assertEqual(result["summary"]["mean_dbm"], -90.5)

    def test_standby_rssi_is_rejected_without_rssi_command(self):
        b = FakeRadioBridge(mode=1)
        with self.assertRaisesRegex(RadioError, "not in RX"):
            Core1121(b).sample_rssi(1)
        self.assertEqual(b.commands, [])

    def test_radio_rejection_stops_initialization(self):
        b = FakeRadioBridge(fail_opcode=bytes.fromhex("0117"))
        with self.assertRaisesRegex(RadioError, "tcxo"):
            Core1121(b).initialize()
        self.assertEqual(b.commands[-1], bytes.fromhex("01170600012c"))
        self.assertFalse(any(c[:2] == bytes.fromhex("0209") for c in b.commands))

    def test_2g4_encoding_and_tcxo_order(self):
        b = FakeRadioBridge()
        with patch("lr1121.time.sleep"):
            report = Core1121(b).start_rx(2440000000)
        self.assertIn(bytes.fromhex("020b916f7200"), b.commands)
        self.assertIn(bytes.fromhex("01120300010202000000"), b.commands)
        self.assertIn(bytes.fromhex("0229764643cececc8c980907ee"), b.commands)
        self.assertIn(bytes.fromhex("020f070f0100"), b.commands)
        self.assertLess(b.commands.index(bytes.fromhex("01170600012c")),
                        b.commands.index(bytes.fromhex("011c01")))
        self.assertEqual(b.commands[-1], bytes.fromhex("0209ffffff"))
        self.assertEqual(report["rx_status"]["decoded"]["chip_mode"], 4)

    def test_persistent_errors_abort_before_rx(self):
        b = FakeRadioBridge(errors="0x0020")
        with self.assertRaisesRegex(RadioError, "error flags"):
            Core1121(b).start_rx()
        self.assertFalse(any(c[:2] == bytes.fromhex("0209") for c in b.commands))

    def test_frequency_validation_precedes_reset(self):
        b = FakeRadioBridge()
        for value in (868000000, 2399999999, 2500000001):
            with self.assertRaises(ValueError):
                Core1121(b).start_rx(value)
        self.assertEqual(b.commands, [])


if __name__ == "__main__":
    unittest.main()
