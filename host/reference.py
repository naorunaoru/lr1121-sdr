"""Bounded USB CLI reference carriers; stock Flipper firmware and BLE Full.
See docs/validation.md. No Flipper firmware flashing is performed.
"""
from contextlib import contextmanager
import datetime
import re
import threading
import time
import serial
from serial.tools import list_ports

def utc():return datetime.datetime.now(datetime.timezone.utc).isoformat()

class Flipper:
    def __init__(self, port=None):
        if port is None:
            candidates = [p.device for p in list_ports.comports()
                          if p.vid == 0x0483 and p.pid == 0x5740 and
                          'flip' in (p.device + (p.product or '')).lower()]
            if len(candidates) != 1:
                raise RuntimeError(f'Expected one Flipper USB serial device, found {len(candidates)}')
            port = candidates[0]
        self.serial = serial.Serial(port, 115200, timeout=.05, write_timeout=1)
        # USB DTR opens a CLI session with its own banner/prompt. Let that arrive
        # before sending a command so it cannot be mistaken for the reply.
        time.sleep(.3)
        self.serial.reset_input_buffer()
        self.events = []
        self._write_lock = threading.Lock()
        self.active = False

    def send(self, data):
        with self._write_lock:
            self.serial.write(data)
            self.serial.flush()

    def read_until(self, marker, timeout=3):
        response = b''
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            response += self.serial.read(max(1, self.serial.in_waiting))
            if marker in response:
                return response.decode('utf-8', 'replace')
        raise RuntimeError(f'Flipper response timeout: {response.decode("utf-8", "replace")!r}')

    def command(self, command):
        if self.active:
            raise RuntimeError('Stop the carrier before sending CLI commands')
        self.serial.reset_input_buffer()
        self.send((command+'\r').encode())
        reply = self.read_until(b'>: ')
        self.events.append(dict(utc=utc(), command=command, reply=reply))
        return reply

    def __enter__(self):
        enable_attempted = False
        try:
            # device_info is read-only; record only non-unique fields.
            info = self.command('info device')
            debug = re.search(r'^system\.debug\s*:\s*(\d+)', info, re.M)
            if not debug or debug.group(1) != '0':
                raise RuntimeError('Expected debug disabled before test; no setting changed')
            self.events[-1]['reply'] = '\n'.join(line for line in info.splitlines()
                if re.match(r'^(firmware\.|radio\.stack\.|system\.debug|hardware\.region)', line))
            enable_attempted = True
            if 'Debug enabled' not in self.command('sysctl debug 1'):
                raise RuntimeError('Could not enable Flipper debug commands')
            return self
        except BaseException:
            # Enabling may have succeeded even if the reply was lost.
            try:
                if enable_attempted:
                    self.command('sysctl debug 0')
            finally:
                self.serial.close()
            raise

    def __exit__(self, *exc):
        try:
            if self.active:
                self.send(b'\x03')
                self.read_until(b'>: ')
                self.active = False
            self.command('sysctl debug 0')
            self.command('uptime')
        finally:
            self.serial.close()

    @contextmanager
    def tone(self, frequency_hz):
        if frequency_hz not in (433920000, 868350000):
            raise ValueError('This experiment permits only the two specified reference frequencies')
        event = dict(utc=utc(), command=f'subghz tx_carrier {frequency_hz}',
                     watchdog_fired=False, stopped=False)
        self.events.append(event)
        self.serial.reset_input_buffer()
        started = time.monotonic()
        def watchdog():
            event['watchdog_fired'] = True
            try:
                self.send(b'\x03')
            except Exception as error:
                event['watchdog_error'] = str(error)
        timer = threading.Timer(4, watchdog)
        timer.daemon = True
        self.active = True
        timer.start()
        try:
            self.send((event['command']+'\r').encode())
            event['start_reply'] = self.read_until(b'Press CTRL+C to stop', timeout=2)
            frequency = re.search(r'Transmitting at frequency (\d+) Hz', event['start_reply'])
            if not frequency:
                raise RuntimeError('Flipper did not confirm carrier transmission')
            event['reported_frequency_hz'] = int(frequency.group(1))
            time.sleep(.04)
            yield event
            if event['watchdog_fired']:
                raise RuntimeError('Carrier exceeded its host watchdog bound')
        finally:
            # Always stop, including a CLI timeout or a failed Pico transaction.
            self.send(b'\x03')
            try:
                event['stop_reply'] = self.read_until(b'>: ', timeout=2)
                event['stopped'] = True
                self.active = False
                event['command_duration_s'] = round(time.monotonic()-started, 6)
            finally:
                timer.cancel()
                timer.join(timeout=1.2)



def expected_tone_hz(channel):
    if type(channel) is not int or not 0 <= channel <= 39:
        raise ValueError('BLE channel must be an integer 0..39')
    return 2402000000 + channel*2000000 - 250000


class BluetoothFlipper(Flipper):
    def __enter__(self):
        super().__enter__()
        try:
            info = self.events[0]['reply']
            if not re.search(r'^radio\.stack\.type\s*:\s*1\s*$', info, re.M):
                raise RuntimeError('The Bluetooth test requires BLE Full (stack type 1)')
            if 'tx_carrier' not in self.command('bt'):
                raise RuntimeError('Bluetooth carrier command unavailable')
            return self
        except BaseException:
            super().__exit__(None, None, None)
            raise

    @contextmanager
    def bt_tone(self, channel=19, power=0):
        tone_hz = expected_tone_hz(channel)
        if type(power) is not int or not 0 <= power <= 6:
            raise ValueError('CLI power setting must be an integer 0..6')
        if self.active:
            raise RuntimeError('A carrier is already active')
        event = dict(utc=utc(), command=f'bt tx_carrier {channel} {power}',
                     expected_frequency_hz=tone_hz, nominal_channel_hz=tone_hz+250000,
                     cli_power_setting=power, watchdog_fired=False, stopped=False)
        self.events.append(event)
        self.serial.reset_input_buffer()
        started = time.monotonic()
        def watchdog():
            event['watchdog_fired'] = True
            try:
                self.send(b'\x03')
            except Exception as error:
                event['watchdog_error'] = str(error)
        timer = threading.Timer(4, watchdog)
        timer.daemon = True
        self.active = True
        timer.start()
        try:
            self.send((event['command']+'\r').encode())
            event['start_reply'] = self.read_until(b'Press CTRL+C to stop', timeout=2.5)
            expected_reply = f'Transmitting carrier at {channel} channel at {power} dB power'
            if expected_reply not in event['start_reply']:
                raise RuntimeError('Flipper did not enter the requested carrier test')
            # CLI prints its message before the HCI call; let the radio settle.
            time.sleep(.08)
            yield event
            if event['watchdog_fired']:
                raise RuntimeError('Carrier exceeded its host watchdog bound')
        finally:
            try:
                self.send(b'\x03')
                event['stop_reply'] = self.read_until(b'>: ', timeout=3)
                event['stopped'] = True
                self.active = False
                event['command_duration_s'] = round(time.monotonic()-started, 6)
            finally:
                timer.cancel()
                timer.join(timeout=1.2)
