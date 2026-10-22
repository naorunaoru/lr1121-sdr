import json
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from bridge import Bridge, BridgeError


class FakeSerial:
    def __init__(self, replies):
        self.replies = list(replies)
        self.writes = []

    def write(self, data):
        self.writes.append(data)
        return len(data)

    def flush(self):
        pass

    def read_until(self, *args, **kwargs):
        return self.replies.pop(0) if self.replies else b""

    def close(self):
        pass


def reply(**fields):
    return (json.dumps(dict(id=1, ok=True, **fields)) + "\n").encode()


class HostTests(unittest.TestCase):
    def test_version_framing_and_big_endian(self):
        io = FakeSerial([reply(cmd_status=3, status=6, data="22030103")])
        result = Bridge(transport=io).version()
        self.assertEqual(io.writes, [b"1 read 1000 4 0101\n"])
        self.assertEqual(result["version"]["firmware"], "0x0103")
        self.assertTrue(result["version"]["is_lr1121_transceiver"])

    def test_split_usb_response(self):
        data = reply(rx="aabb")
        io = FakeSerial([data[:5], data[5:15], data[15:]])
        self.assertEqual(Bridge(transport=io).xfer(b"\x01\x02")["rx"], "aabb")
        self.assertEqual(len(io.writes), 1)

    def test_radio_reject_is_not_silently_decoded(self):
        io = FakeSerial([reply(cmd_status=1, status=2, data="00000000")])
        with self.assertRaisesRegex(BridgeError, "CMD_DAT"):
            Bridge(transport=io).version()

    def test_busy_timeout_preserves_sent_flag_and_does_not_replay(self):
        data = dict(id=1, ok=False, error="busy_timeout", phase="after_command", command_sent=True)
        io = FakeSerial([(json.dumps(data) + "\n").encode()])
        with self.assertRaises(BridgeError) as caught:
            Bridge(transport=io).write_command(b"\x01\x02")
        self.assertTrue(caught.exception.response["command_sent"])
        self.assertEqual(len(io.writes), 1)

    def test_mismatched_id_blocks_further_commands(self):
        io = FakeSerial([b'{"id":99,"ok":true}\n'])
        bridge = Bridge(transport=io)
        with self.assertRaisesRegex(BridgeError, "mismatch"):
            bridge.pins()
        with self.assertRaisesRegex(BridgeError, "synchronization"):
            bridge.reset()
        self.assertEqual(len(io.writes), 1)

    def test_usb_timeout_does_not_replay(self):
        io = FakeSerial([])
        bridge = Bridge(transport=io)
        with patch("bridge.time.monotonic", side_effect=[0, 0, 6]):
            with self.assertRaisesRegex(BridgeError, "not retried"):
                bridge.reset()
        self.assertEqual(len(io.writes), 1)
        with self.assertRaises(BridgeError):
            bridge.reset()
        self.assertEqual(len(io.writes), 1)

    def test_raw_no_wait_is_explicit(self):
        io = FakeSerial([reply(rx="0000")])
        Bridge(transport=io).xfer(b"\xaa\xbb", wait_busy=False)
        self.assertEqual(io.writes, [b"1 xfer 0 aabb\n"])

    def test_direct_status_does_not_send_0100(self):
        io = FakeSerial([reply(rx="055300000004")])
        result = Bridge(transport=io).status()["decoded"]
        self.assertEqual(io.writes, [b"1 xfer 1000 000000000000\n"])
        self.assertEqual(result["irq_status"], "0x00000004")
        self.assertEqual(result["reset_reason"], 5)
        self.assertEqual(result["chip_mode"], 1)
        self.assertTrue(result["running_from_flash"])

    def test_pending_data_is_not_misread_as_reset_and_irq(self):
        io = FakeSerial([reply(rx="075300000000")])
        with self.assertRaisesRegex(BridgeError, "payload is not system status") as caught:
            Bridge(transport=io).status()
        self.assertEqual(caught.exception.response["rx"], "075300000000")
        self.assertNotIn("decoded", caught.exception.response)
        self.assertEqual(len(io.writes), 1)

    def test_invalid_lengths_never_reach_usb(self):
        io = FakeSerial([])
        bridge = Bridge(transport=io)
        for n in (0, 4097, -1):
            with self.assertRaises(ValueError):
                bridge.read_command(b"\x01\x01", n)
        with self.assertRaises(ValueError):
            bridge.xfer(bytes(4097))
        with self.assertRaises(ValueError):
            bridge.request("pins\n2 reset 1000")
        self.assertEqual(io.writes, [])


if __name__ == "__main__":
    unittest.main()
