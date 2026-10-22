#!/usr/bin/env python3
"""Mac/Linux/Windows client for the native Pico LR1121 SPI bridge.

Python runs on the host computer only. The Pico runs Pico SDK C firmware.
"""
from __future__ import annotations

import argparse
import datetime
import json
import sys
import time
from pathlib import Path

import serial
from serial.tools import list_ports
from lr1121 import Core1121, RadioError

MAX_BYTES = 4096


class BridgeError(RuntimeError):
    def __init__(self, message, response=None):
        super().__init__(message)
        self.response = response


def ports():
    return [dict(device=p.device, description=p.description, product=p.product,
                 vid=p.vid, pid=p.pid, serial_number=p.serial_number)
            for p in list_ports.comports()]


def find_port():
    matches = [p["device"] for p in ports()
               if p["vid"] == 0x2E8A and p["product"] == "LR1121 SPI Bridge"]
    if len(matches) != 1:
        raise BridgeError(f"Found {len(matches)} LR1121 bridges; use 'ports' and --port DEVICE")
    return matches[0]


def _hex(data, minimum=1):
    data = bytes(data)
    if not minimum <= len(data) <= MAX_BYTES:
        raise ValueError(f"Transfer length must be {minimum}..{MAX_BYTES} bytes")
    return data.hex()


class Bridge:
    def __init__(self, port=None, timeout_ms=1000, log=None, *, transport=None):
        if not 1 <= timeout_ms <= 10000:
            raise ValueError("timeout_ms must be 1..10000")
        self.timeout_ms = timeout_ms
        self._id = 0
        self._desynced = False
        self._log = open(log, "a", encoding="utf-8") if log else None
        self.io = None
        try:
            if transport is not None:
                self.io = transport
            else:
                self.io = serial.Serial(port or find_port(), baudrate=115200,
                                        timeout=0.1, write_timeout=2, exclusive=True)
                # CDC baud rate is nominal. DTR enables the SDK's CDC connection.
                self.io.dtr = True
                time.sleep(0.15)
                self.io.reset_input_buffer()
                self.io.write(b"\n")  # Terminate/discard any abandoned partial line.
                self.io.flush()
                time.sleep(0.15)
                self.io.reset_input_buffer()
        except Exception:
            self.close()
            raise

    def close(self):
        if self.io is not None:
            self.io.close()
            self.io = None
        if self._log:
            self._log.close()
            self._log = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def _record(self, **fields):
        if self._log:
            fields["utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
            self._log.write(json.dumps(fields, separators=(",", ":")) + "\n")
            self._log.flush()

    def request(self, command):
        """Send one protocol request; never replay a request on timeout or error."""
        if self._desynced:
            raise BridgeError("Connection lost synchronization; close and reopen it. No command sent.")
        if not command or any(ord(c) < 32 or ord(c) > 126 for c in command):
            raise ValueError("Command must be a single printable ASCII line")
        if len(command) > 2 * MAX_BYTES + 80:
            raise ValueError("Command line too long")
        self._id = self._id % 0xFFFFFFFF + 1
        wire = f"{self._id} {command}\n".encode("ascii")
        self._record(direction="tx", line=wire.decode().rstrip("\n"))
        # read/write can each wait twice. At 100 kHz, two maximum transfers
        # take another 656 ms; leave time for USB delivery as well.
        deadline = time.monotonic() + 2 * self.timeout_ms / 1000 + 3
        try:
            if self.io.write(wire) != len(wire):
                raise BridgeError("Partial USB write; command outcome unknown")
            self.io.flush()
            response = bytearray()
            while time.monotonic() < deadline:
                response.extend(self.io.read_until(b"\n", size=20000 - len(response)))
                if len(response) >= 20000:
                    raise BridgeError("Response exceeded protocol size limit")
                if response.endswith(b"\n"):
                    break
            else:
                raise BridgeError("USB response timeout; command outcome unknown (not retried)")
            try:
                result = json.loads(response)
            except (ValueError, UnicodeError) as exc:
                raise BridgeError("Invalid JSON response") from exc
            self._record(direction="rx", response=result)
            if (not isinstance(result, dict) or type(result.get("id")) is not int
                    or result["id"] != self._id or type(result.get("ok")) is not bool):
                raise BridgeError("Response ID/format mismatch", result)
        except Exception as exc:
            self._desynced = True
            self._record(direction="error", id=self._id, error=str(exc))
            raise
        if not result["ok"]:
            raise BridgeError(f"{result.get('error')}: {result.get('phase')}", result)
        return result

    def info(self):
        result = self.request("info")
        if result.get("bridge") != "lr1121-spi" or result.get("protocol") != 1:
            raise BridgeError("This device is not a supported LR1121 bridge", result)
        return result

    def pins(self):
        return self.request("pins")

    def reset(self):
        return self.request(f"reset {self.timeout_ms}")

    def wait_ready(self):
        return self.request(f"wait {self.timeout_ms}")

    def speed(self, hz):
        if type(hz) is not int or not 100000 <= hz <= 16000000:
            raise ValueError("SPI speed must be an integer, 100000..16000000 Hz")
        return self.request(f"speed {hz}")

    def benchmark(self, iterations=500, guard_us=5):
        """Read fixed RAM 0x806000 repeatedly, at most one second; no radio writes."""
        if type(iterations) is not int or not 1 <= iterations <= 1000:
            raise ValueError("iterations must be an integer, 1..1000")
        if type(guard_us) is not int or guard_us not in (5, 100):
            raise ValueError("guard_us must be 5 or 100")
        return self.request(f"bench {iterations} {guard_us}")

    def xfer(self, data, *, wait_busy=True):
        return self.request(f"xfer {self.timeout_ms if wait_busy else 0} {_hex(data)}")

    def read_command(self, command, length):
        if not 1 <= length <= MAX_BYTES:
            raise ValueError("Read length must be 1..4096 bytes, excluding the status byte")
        return self.request(f"read {self.timeout_ms} {length} {_hex(command, 2)}")

    def write_command(self, command):
        return self.request(f"write {self.timeout_ms} {_hex(command, 2)}")

    @staticmethod
    def _payload(result, length):
        if result.get("cmd_status") != 3:
            raise BridgeError("Radio did not return CMD_DAT (3)", result)
        payload = bytes.fromhex(result["data"])
        if len(payload) != length:
            raise BridgeError("Unexpected radio reply length", result)
        return payload

    def version(self):
        result = self.read_command(bytes.fromhex("0101"), 4)
        data = self._payload(result, 4)
        result["version"] = dict(hardware=f"0x{data[0]:02x}", type=f"0x{data[1]:02x}",
                                 firmware=f"0x{int.from_bytes(data[2:], 'big'):04x}",
                                 is_lr1121_transceiver=data[1] == 3)
        return result

    def status(self):
        # Direct status read; sending 0x0100 would clear reset information.
        result = self.xfer(bytes(6))
        data = bytes.fromhex(result["rx"])
        if len(data) != 6:
            raise BridgeError("Unexpected direct status length", result)
        if data[0] >> 1 == 3:
            # A pending read response has Stat1 followed by payload, not Stat2
            # and IRQ bits. Preserve the bytes without inventing a chip state.
            raise BridgeError("Pending CMD_DAT reply consumed; payload is not system status", result)
        result["decoded"] = dict(stat1=f"0x{data[0]:02x}", stat2=f"0x{data[1]:02x}",
                                 cmd_status=data[0] >> 1, irq_active=bool(data[0] & 1),
                                 reset_reason=data[1] >> 4, chip_mode=(data[1] & 15) >> 1,
                                 running_from_flash=bool(data[1] & 1),
                                 irq_status=f"0x{int.from_bytes(data[2:], 'big'):08x}")
        return result

    def errors(self):
        result = self.read_command(bytes.fromhex("010d"), 2)
        flags = int.from_bytes(self._payload(result, 2), "big")
        names = ["LF_RC_CALIB", "HF_RC_CALIB", "ADC_CALIB", "PLL_CALIB", "IMG_CALIB",
                 "HF_XOSC_START", "LF_XOSC_START", "PLL_LOCK"]
        result["error_flags"] = f"0x{flags:04x}"
        result["errors"] = [name for bit, name in enumerate(names) if flags & (1 << bit)]
        result["unknown_error_bits"] = f"0x{flags & 0xff00:04x}"
        return result

    def probe(self):
        result = dict(bridge=self.info(), before_reset=self.pins(), reset=self.reset())
        result["versions"] = [self.version() for _ in range(10)]
        result["status"] = self.status()
        result["errors"] = self.errors()
        values = {r["data"] for r in result["versions"]}
        result["stable_lr1121"] = (len(values) == 1 and
                                     result["versions"][0]["version"]["is_lr1121_transceiver"])
        if not result["stable_lr1121"]:
            raise BridgeError("Version reads were inconsistent or did not identify LR1121", result)
        return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="USB serial device; auto-detect this bridge by default")
    parser.add_argument("--timeout-ms", type=int, default=1000, help="BUSY wait timeout, 1..10000")
    parser.add_argument("--log", type=Path, help="Append request/response JSONL log")
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("ports", "info", "pins", "reset", "wait", "version", "status", "errors", "probe", "bootsel", "rf-init", "rx-stop"):
        commands.add_parser(name)
    rx = commands.add_parser("rx-start", help="Reset/configure Core1121-XF and start continuous 2.4 GHz RX")
    rx.add_argument("--frequency", type=int, default=2440000000, help="Frequency in Hz, 2400000000..2500000000")
    rx.add_argument("--bandwidth", type=int, choices=(200, 400, 800), default=800,
                    help="SDK bandwidth name; actual widths are 203/406/812 kHz")
    rssi = commands.add_parser("rssi", help="Sample instantaneous RSSI; requires rx-start first")
    rssi.add_argument("--samples", type=int, default=100)
    rssi.add_argument("--interval-ms", type=float, default=20)
    commands.add_parser("speed").add_argument("hz", type=int)
    bench = commands.add_parser("bench", help="Time repeated fixed 256-byte RAM reads in native C")
    bench.add_argument("--iterations", type=int, default=500)
    bench.add_argument("--guard-us", type=int, choices=(5, 100), default=5)
    xfer = commands.add_parser("xfer", help="One full-duplex SPI transaction")
    xfer.add_argument("hex", help="MOSI bytes as hex; quote if separated by spaces")
    xfer.add_argument("--no-wait", action="store_true", help="Explicitly bypass BUSY")
    read = commands.add_parser("read", help="Opcode/params, BUSY wait, then status + reply")
    read.add_argument("hex", help="Opcode and parameters as hex")
    read.add_argument("--length", type=int, required=True, help="Reply bytes excluding status")
    commands.add_parser("write", help="Opcode/params/data in one transaction").add_argument("hex")
    args = parser.parse_args(argv)
    try:
        if args.command == "ports":
            result = ports()
        else:
            with Bridge(args.port, args.timeout_ms, args.log) as bridge:
                if args.command == "info":
                    result = bridge.info()
                else:
                    bridge.info()  # Confirm identity before any radio/bootloader operation.
                    if args.command == "rf-init":
                        result = Core1121(bridge).initialize()
                    elif args.command == "rx-start":
                        result = Core1121(bridge).start_rx(args.frequency, args.bandwidth)
                    elif args.command == "rssi":
                        result = Core1121(bridge).sample_rssi(args.samples, args.interval_ms)
                    elif args.command == "rx-stop":
                        result = Core1121(bridge).stop_rx()
                    elif args.command == "xfer":
                        result = bridge.xfer(bytes.fromhex(args.hex), wait_busy=not args.no_wait)
                    elif args.command == "read":
                        result = bridge.read_command(bytes.fromhex(args.hex), args.length)
                    elif args.command == "write":
                        result = bridge.write_command(bytes.fromhex(args.hex))
                    elif args.command == "speed":
                        result = bridge.speed(args.hz)
                    elif args.command == "bench":
                        result = bridge.benchmark(args.iterations, args.guard_us)
                    elif args.command == "wait":
                        result = bridge.wait_ready()
                    elif args.command == "bootsel":
                        result = bridge.request("bootsel")
                    else:
                        result = getattr(bridge, args.command)()
        print(json.dumps(result, indent=2))
        return 0
    except (BridgeError, RadioError, serial.SerialException, OSError, ValueError) as exc:
        error = dict(ok=False, error=str(exc))
        if isinstance(exc, (BridgeError, RadioError)) and exc.response is not None:
            error["response"] = exc.response
        print(json.dumps(error, indent=2), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
