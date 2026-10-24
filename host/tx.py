#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Experimental LR1121 frequency playback; configurable center and documented PA profiles.

Input: big-endian signed16 offsets, with a selectable power-of-two PLL scale.
Requires the separately installed player/vectors and the TX-only Pico profile.
No firmware flashing is performed by this command.
"""
import argparse
import json
import math
from pathlib import Path
import struct
import sys
import time
from bridge import Bridge
from lr1121 import Core1121

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'lr1121/tx'))
from vectors import vectors
BASE_WORD = 910530969  # Historical default; session code passes its selected center.
PA_PROFILES = {
    'lp': dict(config=(0, 0, 4, 0), minimum=-17, maximum=14),
    'hp': dict(config=(1, 1, 4, 7), minimum=-9, maximum=22),
    'hf': dict(config=(2, 0, 4, 0), minimum=-18, maximum=13),
}
RAMP_US = (16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 240, 272, 304)


def radio_settings(frequency_hz=868350000, pa='auto', power_dbm=-9, ramp_us=48, offset_shift=1):
    encoding = offset_encoding(offset_shift)
    if type(frequency_hz) is not int or not 0 < frequency_hz < 4096000000:
        raise ValueError('Frequency must be an integer inside the 32-bit PLL encoding')
    center = (frequency_hz*16384//15625) | 1
    if not (32768 << offset_shift) < center <= 0xffffffff-(32767 << offset_shift):
        raise ValueError('Center must leave room for the full signed16 deviation range')
    selected = ('hp' if frequency_hz <= 1500000000 else 'hf') if pa == 'auto' else pa
    if selected not in PA_PROFILES:
        raise ValueError('PA must be auto, lp, hp or hf')
    profile = PA_PROFILES[selected]
    if type(power_dbm) is not int or not profile['minimum'] <= power_dbm <= profile['maximum']:
        raise ValueError(f"{selected} PA power range is {profile['minimum']}..{profile['maximum']} dBm")
    if type(ramp_us) is not int or ramp_us not in RAMP_US:
        raise ValueError('Unsupported PA ramp time')
    # Image calibration is documented for the LF receive path, 400..1000 MHz.
    # Retain frequency-appropriate setup there; never reuse the 868 MHz pair elsewhere.
    image = [math.floor((frequency_hz/1e6-1)/4), math.ceil((frequency_hz/1e6+1)/4)] if 400000000 <= frequency_hz <= 1000000000 else None
    return dict(frequency_hz=frequency_hz, center_word=center, pa=selected,
                power_dbm_requested=power_dbm, pa_config=list(profile['config']),
                ramp_us=ramp_us, ramp_code=RAMP_US.index(ramp_us), image_calibration=image,
                **encoding)

OFFSET_HZ = 2 * 15625 / 16384
BLOCK = 2048
S = 0x80f400
ARC_CLOCK_HZ = 32000000


def timing_settings(rate):
    if isinstance(rate, bool) or not isinstance(rate, (int, float)) or not math.isfinite(rate) or not 20000 <= rate <= 250000:
        raise ValueError('Requested update rate must be finite and within 20000..250000 Hz')
    period = math.floor(ARC_CLOCK_HZ/rate + 0.5)
    actual = ARC_CLOCK_HZ/period
    return dict(rate_requested=rate, rate_actual=actual, period_cycles=period,
                rate_error_ppm=(actual/rate-1)*1e6)


def offset_encoding(offset_shift=1):
    if type(offset_shift) is not int or not 1 <= offset_shift <= 15:
        raise ValueError('Offset shift must be an integer in 1..15')
    step = (1 << offset_shift)*15625/16384
    return dict(offset_shift=offset_shift, offset_step_hz=step,
                offset_min_hz=-32768*step, offset_max_hz=32767*step)


def resolve_shift(offset_shift=None, deviation_hz=None):
    if offset_shift is not None:
        offset_encoding(offset_shift)
        return offset_shift
    if deviation_hz is None:
        return 1
    if (isinstance(deviation_hz, bool) or not isinstance(deviation_hz, (int, float)) or
            not math.isfinite(deviation_hz) or deviation_hz < 0):
        raise ValueError('Deviation must be finite and nonnegative')
    for shift in range(1, 16):
        if deviation_hz <= offset_encoding(shift)['offset_max_hz']:
            return shift
    raise ValueError('Deviation exceeds the signed16 offset encoding')


def scale_samples(samples, deviation_hz, offset_shift=1):
    """Map normalized signed16 input to encoded offsets; never silently clip."""
    encoding = offset_encoding(offset_shift)
    if (isinstance(deviation_hz, bool) or not isinstance(deviation_hz, (int, float)) or
            not math.isfinite(deviation_hz) or not 0 <= deviation_hz <= encoding['offset_max_hz']):
        raise ValueError('Deviation must fit the selected encoding; increase offset_shift if needed')
    if any(type(x) is not int or not -32768 <= x <= 32767 for x in samples):
        raise ValueError('Normalized samples must be signed16 integers')
    return [round(x/(32768 if x < 0 else 32767)*deviation_hz/encoding['offset_step_hz'])
            for x in samples]

STATE_NAMES = ('mode timeout count period oldcb index calls reason timer_control '
               'timer_limit timer_count vector_base irq_enable irq_priority irq_trigger '
               'irq_select cyclic log_pointer span spare active max_observation '
               'preceding_irq marker irq_ctrl irq_count last_frequency center_word '
               'vector_readback priority_readback multi_irq_observations observer_baseline '
               'ready0 ready1 checksum rtc_start rtc_done log_count native_return max_chain offset_shift').split()


def save(path, value):
    path.write_text(json.dumps(value, indent=2)+'\n')


def read(b, address, count=1):
    result = []
    for offset in range(0, count, 64):
        n = min(64, count-offset)
        raw = b._payload(b.read_command(b'\x01\x06'+struct.pack('>IB', address+4*offset, n), 4*n), 4*n)
        result.extend(struct.unpack(f'>{n}I', raw))
    return result


def put(b, address, words):
    for offset in range(0, len(words), 64):
        part = words[offset:offset+64]
        b.write_command(b'\x01\x05'+struct.pack('>I', address+4*offset)+struct.pack(f'>{len(part)}I', *part))


def mask(b, address, value):
    b.write_command(b'\x01\x0c'+struct.pack('>III', address, 0xffffffff, value))


def packed(offsets):
    if len(offsets) % 2 or any(type(x) is not int or not -32768 <= x <= 32767 for x in offsets):
        raise ValueError('Expected paired signed16 offsets')
    pairs = [(offsets[i] & 0xffff) | ((offsets[i+1] & 0xffff) << 16)
             for i in range(0, len(offsets), 2)]
    return struct.pack(f'>{len(pairs)}I', *pairs)


def checksum(offsets, center_word=BASE_WORD, offset_shift=1):
    return sum(center_word+(x << offset_shift) for x in offsets) & 0xffffffff


def frame(block, offsets, center_word=BASE_WORD, offset_shift=1):
    if len(offsets) != BLOCK:
        raise ValueError('Frames require exactly 2048 offsets')
    return struct.pack('>II', block, checksum(offsets, center_word, offset_shift))+packed(offsets)


def read_reply(b, request_id, deadline):
    data = bytearray()
    while time.monotonic() < deadline:
        data.extend(b.io.read_until(b'\n', size=20000-len(data)))
        if len(data) >= 20000:
            raise RuntimeError('Queue response exceeds limit')
        if data.endswith(b'\n'):
            result = json.loads(data)
            b._record(direction='rx', response=result)
            if (type(result.get('id')) is not int or result['id'] != request_id or
                    type(result.get('ok')) is not bool):
                raise RuntimeError('Queue response ID/format mismatch')
            return result
    raise TimeoutError('Queue response deadline')


def fill(b, block, offsets, center_word, offset_shift=1):
    ready = b.request(f'txfill {block} {checksum(offsets, center_word, offset_shift)}')
    if ready.get('ready') != 4096:
        raise RuntimeError('Unexpected txfill handshake')
    raw = packed(offsets)
    if b.io.write(raw) != len(raw):
        raise RuntimeError('Partial initial-bank write')
    b.io.flush()
    response = read_reply(b, ready['id'], time.monotonic()+3)
    if not response['ok'] or response.get('checksum') != checksum(offsets, center_word, offset_shift) or response.get('block') != block:
        raise RuntimeError(f'Initial-bank upload failed: {response}')


def validate(offsets, rate, timeout_ms, preloaded=False):
    timing = timing_settings(rate)
    if not 4096 <= len(offsets) <= 3200000 or len(offsets) % BLOCK:
        raise ValueError('Use 4096..3200000 offsets, in whole 2048-update banks')
    packed(offsets[:2])
    if any(type(x) is not int or not -32768 <= x <= 32767 for x in offsets):
        raise ValueError('Offset outside signed16 range')
    if not 100 <= timeout_ms <= 60000 or len(offsets)/timing['rate_actual'] >= timeout_ms/1000:
        raise ValueError('Native timeout must exceed waveform duration and be <=60000 ms')
    if preloaded:
        if any(x != offsets[i % 4096] for i, x in enumerate(offsets)):
            raise ValueError('Preloaded mode requires repetitions of the first 4096 offsets')
    elif len(offsets) < 3*BLOCK:
        raise ValueError('Queued mode needs at least three banks; use --preloaded for two')


def configure(b, settings):
    r = Core1121(b)
    r.initialize()
    if settings['image_calibration'] is not None:
        r._write('image', 0x0111, bytes(settings['image_calibration']))
    for name, opcode, payload in [
        ('xosc', 0x011c, b'\x01'),
        ('gfsk', 0x020e, b'\x01'), ('frequency', 0x020b, struct.pack('>I', settings['frequency_hz'])),
        ('pa', 0x0215, bytes(settings['pa_config'])), ('power', 0x0211, bytes([settings['power_dbm_requested'] & 255, settings['ramp_code']])),
        ('modulation', 0x020f, struct.pack('>IBBI', 9997, 0, 0x0b, 19043)),
        ('clear', 0x0114, b'\xff'*4),
    ]:
        r._write(name, opcode, payload)


def play(offsets, *, rate, timeout_ms, out, port=None, preloaded=False,
         standby=False, gap_us=2, poll_us=100, pause_block=None, pause_ms=0,
         frequency_hz=868350000, pa='auto', power_dbm=-9, ramp_us=48,
         offset_shift=None, deviation_hz=None):
    offset_shift = resolve_shift(offset_shift, deviation_hz)
    if deviation_hz is not None:
        offsets = scale_samples(offsets, deviation_hz, offset_shift)
    timing = timing_settings(rate)
    validate(offsets, rate, timeout_ms, preloaded)
    settings = radio_settings(frequency_hz, pa, power_dbm, ramp_us, offset_shift)
    center_word = settings['center_word']
    if not 2 <= gap_us <= 100 or not 20 <= poll_us <= 500:
        raise ValueError('SPI gap must be 2..100 us; polling 20..500 us')
    if not 0 <= pause_ms <= 80 or (pause_block is not None and
            (preloaded or not 2 <= pause_block < len(offsets)//BLOCK)):
        raise ValueError('Invalid host pause')
    out.mkdir(parents=True, exist_ok=False)
    row = dict(rate=timing['rate_actual'], **timing, deviation_hz=deviation_hz,
               input_mode='normalized_s16' if deviation_hz is not None else 'encoded_offsets', count=len(offsets), preloaded=preloaded, standby=standby,
               timeout_ms=timeout_ms, gap_us=gap_us, poll_us=poll_us,
               pause_block=pause_block, pause_ms=pause_ms, radio=settings, completed=False)
    b = None
    binary = False
    try:
        b = Bridge(port=port, log=out/'serial.jsonl')
        row['bridge'] = b.info()
        if row['bridge']['firmware'] != '1.0.8-txopt5':
            raise RuntimeError('Use the tested TX-only Pico profile (1.0.8-txopt5)')
        b.reset()
        if b.version()['data'] != '22030103':
            raise RuntimeError('Transceiver 1.3 required')
        row['spi'] = b.speed(16000000)
        configure(b, settings)
        actual = read(b, 0xf30058)[0]
        row['pll_word_readback'] = actual
        if actual != center_word:
            raise RuntimeError('Native frequency encoding/readback differs; no playback started')
        put(b, 0x80f500, [0])
        mask(b, 0x8004ac, 0x95010)
        mask(b, 0x8004bc, 2)
        if read(b, 0x80f500)[0] != 0x46535434:
            raise RuntimeError('TX player ABI mismatch; install the configurable-offset player')
        if read(b, 0x80d000, 60) != vectors():
            raise RuntimeError('TX vectors not installed/verified')
        state = [0]*len(STATE_NAMES)
        state[:4] = [2 if standby else 3, round(timeout_ms*32768/1000), len(offsets), timing['period_cycles']]
        state[16], state[23], state[27] = int(preloaded), 0x46535434, center_word
        state[40] = offset_shift
        put(b, S, state)
        for block in range(2):
            binary = True
            fill(b, block, offsets[block*BLOCK:(block+1)*BLOCK], center_word, offset_shift)
            binary = False
        if read(b, 0x80c000, 2048) != list(struct.unpack('>2048I', packed(offsets[:4096]))):
            raise RuntimeError('Initial RAM verification failed')
        row['guard'] = b.request(f'guard {timeout_ms+2000}')
        start = time.monotonic()
        if preloaded:
            mask(b, 0x8004ac, 0x94d10)
            mask(b, 0x8004bc, 2)
        else:
            blocks = len(offsets)//BLOCK
            ready = b.request(f'txqueue {blocks} 8 {gap_us} {poll_us}')
            if ready.get('ready') != 4104:
                raise RuntimeError('Unexpected txqueue handshake')
            binary = True
            for block in range(2, blocks):
                if block == pause_block:
                    time.sleep(pause_ms/1000)
                raw = frame(block, offsets[block*BLOCK:(block+1)*BLOCK], center_word, offset_shift)
                if b.io.write(raw) != len(raw):
                    raise RuntimeError('Partial USB frame write')
            b.io.flush()
            row['queue'] = read_reply(b, ready['id'], start+timeout_ms/1000+1)
            binary = False
            if not row['queue']['ok']:
                raise RuntimeError(f"Queue failure: {row['queue']['reason']}")
        # No SPI while preloaded playback runs; native timeout is checked afterwards.
        prefill_s = row.get('queue', {}).get('prefill_us', 0)/1e6
        time.sleep(max(0, start+prefill_s+timeout_ms/1000+.05-time.monotonic()))
        row['guard_after'] = b.request('guardstatus')
        row['state'] = dict(zip(STATE_NAMES, read(b, S, len(STATE_NAMES))))
        s = row['state']
        raw = read(b, 0x80e000, min(s['log_count'], 128)*8)
        row['timestamps'] = sorted([raw[i:i+8] for i in range(0, len(raw), 8)])
        row['checksum_ok'] = s['checksum'] == checksum(offsets, center_word, offset_shift)
        row['trace_matches_source'] = bool(row['timestamps']) and all(1 <= t[0] <= len(offsets) and
            t[5] == t[6] == center_word+(offsets[t[0]-1] << offset_shift) for t in row['timestamps'])
        row['status'], row['errors'] = b.status(), b.errors()
        if not (row['checksum_ok'] and row['trace_matches_source'] and
                s['index'] == len(offsets) and s['reason'] == 2 and not s['active'] and
                not row['guard_after']['expired'] and row['errors']['error_flags'] == '0x0000'):
            raise RuntimeError('Playback validation failed; inspect result.json')
        if not standby and (row['status']['decoded']['chip_mode'] != 1 or
                            row['status']['decoded']['irq_status'] != '0x00000400'):
            raise RuntimeError('Native TX timeout did not leave the expected state')
        row['completed'] = True
        return row
    except BaseException as exc:
        row['exception'] = repr(exc)
        raise
    finally:
        # A full disk or logging failure must not prevent radio cleanup.
        try:
            save(out/'result.json', row)
        finally:
            if b:
                try:
                    if binary:
                        # Never inject ASCII into a partially sent binary session.
                        # Closing CDC trips the armed guard; native timeout remains.
                        row['cleanup'] = 'closed incomplete binary session; no ASCII reset injected'
                    else:
                        b.reset()
                        row['cleanup'] = 'radio reset'
                except Exception as exc:
                    row['cleanup_error'] = repr(exc)
                finally:
                    b.close()
            save(out/'result.json', row)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='command', required=True)
    tone = sub.add_parser('tone', help='Generate a signed16 offset file without hardware access')
    tone.add_argument('output', type=Path)
    tone.add_argument('--rate', type=float, default=200000, help='Requested updates/s, 20000..250000; rounded to the nearest ARC timer period')
    tone.add_argument('--samples', type=int, default=4096)
    tone.add_argument('--frequency', type=float, default=1513.671875)
    tone.add_argument('--deviation', type=float, default=38086)
    tone.add_argument('--offset-shift', type=int, help='Encoding shift; auto-select from deviation when omitted')
    run = sub.add_parser('play')
    run.add_argument('input', type=Path)
    run.add_argument('--out', type=Path, required=True)
    run.add_argument('--port')
    run.add_argument('--rate', type=float, default=200000, help='Requested updates/s, 20000..250000; rounded to the nearest ARC timer period')
    run.add_argument('--offset-shift', type=int, help='PLL-word shift 1..15; defaults to 1 for raw input or auto-selects for normalized input')
    run.add_argument('--deviation-hz', '--offset-range-hz', type=float, help='Interpret input as normalized signed16 and scale its peaks to this deviation')
    run.add_argument('--frequency-hz', type=int, default=868350000)
    run.add_argument('--pa', choices=['auto', 'lp', 'hp', 'hf'], default='auto')
    run.add_argument('--power-dbm', type=int, default=-9)
    run.add_argument('--ramp-us', type=int, choices=RAMP_US, default=48)
    run.add_argument('--repeat', type=int, default=1)
    run.add_argument('--timeout-ms', type=int)
    run.add_argument('--preloaded', action='store_true')
    run.add_argument('--standby', action='store_true', help='Timer/RAM test without native TX')
    run.add_argument('--gap-us', type=int, default=2)
    run.add_argument('--poll-us', type=int, default=100)
    run.add_argument('--tag-blocks', action='store_true', help='Add distinct small DC offsets to adjacent banks')
    run.add_argument('--pause-block', type=int)
    run.add_argument('--pause-ms', type=int, default=0)
    a = p.parse_args()
    try:
        timing = timing_settings(a.rate)
        a.offset_shift = resolve_shift(a.offset_shift, a.deviation if a.command == 'tone' else a.deviation_hz)
        encoding = offset_encoding(a.offset_shift)
    except ValueError as exc:
        p.error(str(exc))
    if a.command == 'tone':
        if (not 4096 <= a.samples <= 3200000 or a.samples % BLOCK or
                not 0 < a.frequency < timing['rate_actual']/2 or not 0 <= a.deviation <= encoding['offset_max_hz']):
            p.error('Invalid length, tone frequency or deviation')
        values = [round(a.deviation/encoding['offset_step_hz']*math.sin(2*math.pi*a.frequency*i/timing['rate_actual']))
                  for i in range(a.samples)]
        with a.output.open('xb') as f:
            f.write(struct.pack(f'>{len(values)}h', *values))
        print(json.dumps(dict(samples=len(values), duration_s=len(values)/timing['rate_actual'], **timing, **encoding)))
    else:
        raw = a.input.read_bytes()
        if not raw or len(raw) % 2 or not 1 <= a.repeat <= 1563 or len(raw)//2*a.repeat > 3200000:
            p.error('Invalid offset file length/repeat count')
        offsets = list(struct.unpack(f'>{len(raw)//2}h', raw))*a.repeat
        if a.tag_blocks and a.deviation_hz is not None:
            p.error('--tag-blocks is only defined for encoded-offset input')
        if a.tag_blocks:
            offsets = [v+(((i//BLOCK*73)&255)-128) for i,v in enumerate(offsets)]
        timeout = a.timeout_ms if a.timeout_ms is not None else math.ceil(len(offsets)/timing['rate_actual']*1000)+500
        result = play(offsets, rate=a.rate, timeout_ms=timeout, out=a.out, port=a.port,
                      preloaded=a.preloaded, standby=a.standby, gap_us=a.gap_us,
                      poll_us=a.poll_us, pause_block=a.pause_block, pause_ms=a.pause_ms,
                      frequency_hz=a.frequency_hz, pa=a.pa, power_dbm=a.power_dbm, ramp_us=a.ramp_us,
                      offset_shift=a.offset_shift, deviation_hz=a.deviation_hz)
        print(json.dumps({k:result[k] for k in ['completed', 'count', 'rate_requested', 'rate_actual', 'period_cycles', 'checksum_ok']}))


if __name__ == '__main__':
    main()
