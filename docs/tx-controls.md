# External rate and frequency-offset controls

The host CLI and `host/tx.py` API accept modulation parameters at playback start.
They are fixed for the entire session. The input remains signed16 big-endian,
one sample per frequency update; USB frame sizes and the two-bank LR ring are unchanged.

## Update rate

`--rate HZ` accepts any finite requested rate in 20000–250000 updates/s. The host
selects `period = floor(32000000 / requested + 0.5)` and reports the actual nominal
rate `32000000 / period`, the requested rate, period in clocks and rounding error
in ppm. For example, 220000 selects 145 clocks and 220689.655172 updates/s.
“Actual” here means the timer setting, not a calibrated measurement of RF updates;
interrupt delay and clock error still apply. The 250 kHz timing limitations remain.

Waveform generation, default timeout calculation and duration validation use that
actual timer rate. The implementation accepts every integer period from 128 to
1600, rather than a preset whitelist. Rates above the tested 250 kHz ceiling are
still rejected. Modulation frequency and occupied RF bandwidth are separate from
this update rate.

## Offset range and resolution

The LR1121 writes `center_word + (offset << shift)` for each signed16 offset.
`--offset-shift N` selects N=1..15. This keeps the low PLL-word bit set and uses a
variable shift plus one RAM load in the ISR; host sample scaling does not add
per-update multiplication on the radio.

| Shift | Hz per encoded unit | Negative endpoint | Positive endpoint |
| ---: | ---: | ---: | ---: |
| 1 (default) | 1.907348633 | −62,500 Hz | +62,498.093 Hz |
| 2 | 3.814697266 | −125,000 Hz | +124,996.185 Hz |
| 3 | 7.629394531 | −250,000 Hz | +249,992.371 Hz |
| 4 | 15.258789063 | −500,000 Hz | +499,984.741 Hz |
| 8 | 244.140625 | −8 MHz | +7,999,755.859 Hz |
| 15 | 31,250 | −1.024 GHz | +1,023,968,750 Hz |

These are numeric encoding ranges, not measured modulation bandwidths, PLL
settling limits or antenna coverage. Host and Pico reject a center/shift combination
unless the entire signed16 range fits the positive 32-bit PLL-word interval.
For example, the default 868.35 MHz center cannot accommodate shift 15.
Widening the encoding trades frequency resolution for range.

There are two input interpretations:

- **Encoded offsets:** omit `--deviation-hz`. Each input integer is already in the
  chosen encoding units. Shift defaults to 1; supply a matching `--offset-shift`
  when reading a file generated with a wider encoding.
- **Normalized samples:** supply `--deviation-hz HZ` (alias `--offset-range-hz`).
  −32768 and +32767 map to approximately −HZ and +HZ respectively, and zero stays
  zero. The host selects the smallest shift that fits both endpoints unless an
  explicit shift is supplied. It rounds to the nearest encoded frequency step,
  with at most half a step of quantization error. It never silently clips.

Thus `--deviation-hz 75000` selects shift 2 but uses about ±75 kHz, not the full
±125 kHz encoding span. `--tag-blocks` is only accepted for encoded input; it adds
diagnostic offsets after waveform generation and is not part of normalized scaling.

## CLI examples

For an existing normalized signed16 waveform:

```sh
python host/tx.py play build/normalized.s16be --rate 220000 \
  --deviation-hz 75000 --frequency-hz 868350000 --power-dbm -9 \
  --out build/tx-normalized
```

To generate and repeat an encoded tone, use the same explicit encoding for both
commands. Tone generation uses the quantized timer rate:

```sh
python host/tx.py tone build/wide-tone.s16be --rate 220000 \
  --frequency 1670.2586206896551 --deviation 75000 --offset-shift 2
python host/tx.py play build/wide-tone.s16be --rate 220000 --offset-shift 2 \
  --repeat 147 --timeout-ms 3600 --out build/tx-wide-tone
```

The generator prints JSON describing its rate and encoding. Playback prints JSON
with completion, requested/actual rates, period and checksum status; `result.json`
also records encoding step/endpoints, selected shift and normalized deviation.
Raw files have no embedded metadata: a consuming program must retain these settings.

## Python API

Set `PYTHONPATH=host` when invoking an external script from the repository root:

```python
from pathlib import Path
import math
import struct
import tx

raw = Path("build/normalized.s16be").read_bytes()
samples = list(struct.unpack(f">{len(raw)//2}h", raw))
timing = tx.timing_settings(220000)
result = tx.play(
    samples,
    rate=220000,
    deviation_hz=75000,       # auto-selects shift 2
    frequency_hz=868350000,
    pa="hp",
    power_dbm=-9,
    timeout_ms=math.ceil(len(samples)/timing["rate_actual"]*1000)+800,
    out=Path("build/tx-from-program"),
)
```

`tx.offset_encoding(shift)` describes a wire encoding; `tx.resolve_shift(None,
deviation_hz)` selects one; `tx.scale_samples(samples, deviation_hz, shift)` converts
normalized input without opening hardware. Existing callers that supply raw offsets
and omit both new parameters retain shift-1 behavior. Buffer-length and timeout
constraints still apply; see [TX reproduction](tx.md).

## Firmware compatibility

Rebuild/reinstall the player and TX Pico profile together. The current player is
4076 bytes, SHA-256 `7c2bba8afc67282700cb134fc170b744a016b736e039e204d196de0092924ce6`;
Pico is `1.0.8-txopt5`, and the ABI marker is `0x46535434`. Host, vector verifier
and Pico reject the preceding ABI. State offset +160 stores the shift; the previous
40 state words keep their offsets. Both upload modes include the chosen shift in
frequency expansion and checksum validation. Normal receiver firmware is unchanged.

Both hardware timeouts remain active. Changing modulation parameters starts a new
session and does not extend an existing TX deadline.

## Hardware validation

Six cases passed on 2026-10-23, all with matching checksums, final indices and retained
source/register readbacks, no radio error flags, no USB-starved polls and unexpired
Pico guards. All RF cases used 868.35 MHz and requested −9 dBm. No external RF capture
or calibrated deviation/spectrum measurement was made in this campaign.

| Case | Requested → timer rate, updates/s | Encoding | Outcome |
| --- | --- | --- | --- |
| Standby endpoint pattern | 123456.7 → 123552.124 | Shift 4, signed16 extremes | 602112 updates passed |
| Standby endpoint pattern, 2440 MHz center | 220000 → 220689.655 | Shift 15, signed16 extremes | 602112 updates passed |
| Preloaded normalized tone | 220000 → 220689.655 | ±75 kHz, shift 2 | 602112 updates passed |
| Streamed normalized tone | 220000 → 220689.655 | ±75 kHz, shift 2 | 602112 updates passed; 3.711 ms minimum margin |
| Streamed normalized tone, 40 ms host pause | 250000 → 250000 | ±75 kHz, shift 2 | 3002368 updates passed; 1.948 ms minimum margin |
| Legacy raw offsets with bank tags | 200000 → 200000 | Default shift 1 | 602112 updates passed; 5.105 ms minimum margin |

The 250 kHz run's slowest bank write was 5.608 ms. Its mean against the nominal RTC
was 249960.942 updates/s and all 128 retained diagnostic records had a pending timer
interrupt before return. The added RAM read retains streaming throughput, but does
not remove the timing limitations described in [the earlier comparison](tx-250k.md).
The extreme-width tests were standby-only: they establish signed arithmetic and
register readback, not an ability to radiate that modulation span.

Both changed firmware components built, native checks and 39 Python tests passed.
The normal receiver extension and Pico 1.0.8 were restored, with LR1121 1.3 in standby
and zero error flags. No Flipper changes were needed. See the
[sanitized measurements](measurements/tx-modulation-controls.json).
