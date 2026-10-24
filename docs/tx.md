# Experimental frequency playback

This is an opt-in TX research profile, separate from the SDR++ receive source.
It streams frequency-control words to LR1121 transceiver **1.3**, with selectable
center frequency and PA settings. Defaults remain **868.350 MHz** and **−9 dBm requested output**. Board matching, output power
and spectral purity have not been characterized across other frequencies. This
is not an arbitrary complex-I/Q or amplitude-modulated transmitter.

The tested combination is Core1121-XF HF + RP2040, the normal repository wiring,
and a 15.625 MHz physical SPI clock (16 MHz request). No additional dependencies
are required beyond the pinned receiver toolchains and `host/requirements.txt`.
The SDR++ module remains receive-only. Stop it before opening the bridge.

## Status

The current build accepts requested rates from 20 to 400 kHz for research, rounded to a whole
ARC timer period, and selectable offset encoding scales. Use the matching
TX player, Pico profile and host CLI together; the ABI marker changed to reject
older players. The default remains 200 kHz. The accepted ceiling is not a streaming guarantee; see the [ISR/SPI measurements](tx-isr-spi.md) for tested delivery and timing limits.

The optimized 200 kHz path completed three 15.012-second runs: 9,007,104 updates,
matching checksums, no player underruns and no USB-starved polls. Two runs included
a 40 ms host pause. Minimum refill margin was 5.00–5.17 ms. Flipper recordings
reported zero dropped events and detected the reference tone.

One earlier long attempt triggered a Pico watchdog reboot before observable RF.
Its cause remains unresolved; subsequent successful runs do not establish a fix.
Late timer entries and pending interrupts remain. Correct checksums demonstrate
sample delivery, not uniformly spaced updates or spectral purity. See the
[measurement report](tx-measurements.md) before treating this as a reliable modem.

## Build profiles

```sh
python tools/build_tx.py
python tools/build_pico.py --tx --toolchain /path/to/arm-gnu-toolchain
```

Outputs:

- `build/lr1121-tx/player.bin`: 4076-byte player including the unchanged SDR prefix.
- `build/pico-tx/lr1121_bridge.uf2`: TX-only Pico profile, version `1.0.8-txopt6`.
- `build/lr1121/stream.bin`: ordinary receiver extension, built as a prerequisite.

The player SHA-256 is
`04d325b7ea603df41e08e88da8b5efdbf62a1c2a1fc27b5ccf33345453529e56`, for the sparse optimized ISR revision. Earlier measurements use a
different payload hash; see the validation records. The build verifies the prefix, 4080-byte ceiling and
CBC padding holes. `--tx` selects `pico/tx/`; the default selects
`pico/firmware/` and still produces `build/pico/lr1121_bridge.uf2`. Both profiles
compile the shared receiver implementation, but TX firmware rejects receive-stream
commands. This keeps the measured TX control loop separate from the receiver loop.
The small TX-specific control/parser copies are intentional experimental isolation.

Flash the TX UF2 through BOOTSEL. A packed-RX module binding is not needed for TX.
The player/vector installers require an already running transceiver 1.3; follow
[the normal installation procedure](lr1121-install.md) first if necessary.

## Install the application extension and vectors

All commands below run from the repository root. Add `--port DEVICE` when more than
one bridge is connected. They do not update the bootloader.

```sh
python lr1121/install.py forge \
  --payload build/lr1121-tx/player.bin --out build/lr1121-tx
python lr1121/install.py install \
  --payload build/lr1121-tx/player.bin --out build/lr1121-tx
python lr1121/tx/vectors.py forge
python lr1121/tx/vectors.py install
python lr1121/tx/vectors.py verify
```

The first installer retains the checksum-pinned vendor prefix, erases/replaces
only the application and verifies the SDR prefix self-test. TX needs an additional
256-byte encrypted fragment at `0x95ff0`: its first 16 bytes are uncontrolled CBC
material; the 60-entry vector table starts at `0x96000`. IRQ16 points to `0x95210`,
exceptions 1–15 to `0x95090`; other handlers retain the 1.3 address map. The vector
installer checks ciphertext, manifest, current UID, application identity and
bootloader identity, then verifies the table through the player's diagnostic
copy routine. It writes this application fragment without another erase.

Reinstall vectors **after every application reflash**. Do not start playback until
verification succeeds. Generated encrypted images, vector caches, bindings and
logs stay under ignored `build/`; do not publish or reuse them on another module.
Regenerate caches after changing the payload. Re-running forge intentionally
replaces its local cache; installation refuses inconsistent metadata.

## Frequency and PA controls

`play` accepts `--frequency-hz HZ`, `--pa auto|lp|hp|hf`, `--power-dbm DBM` and
`--ramp-us US`. Frequency is an integer in Hz. Validation follows the 32-bit PLL
word encoding, with enough headroom for all signed16 offsets; it does not impose
advertised operating bands. Native command acceptance and exact PLL-word readback
are required before playback. The encoding range is not a claim of PLL lock,
usable antenna output, matching, efficiency or spectral purity at arbitrary RF.
The Core1121-HF Sub-GHz matching is intended for 850–930 MHz.

| PA profile | Requested power range | Supply | Duty / HP size |
| --- | --- | --- | --- |
| `lp` | −17..+14 dBm | VREG | 4 / 0 |
| `hp` | −9..+22 dBm | VBAT | 4 / 7 |
| `hf` | −18..+13 dBm | VREG | 4 / 0 |

`auto` chooses HP at or below 1.5 GHz and HF above it, following the native LF/HF
frequency split. It is a host convenience, not a measurement of which PA works
best on a particular board. Explicit PA selection overrides that choice; it does
not replace the board's matching or RF switch wiring. LP/HP use Sub-GHz outputs;
HF uses the high-frequency output. The Waveshare switch table is retained.
Power is a native requested setting, not calibrated conducted or radiated power.
PA supply, duty and size use the documented profiles in the table.

Ramp times are 16,32,48,64,80,96,112,128,144,160,176,192,208,240,272 or 304 µs;
default 48 µs. These control TX startup/shutdown, not per-update modulation speed.
Profile bounds and supply choices follow LR1121 UM v2.2 §9.5. No fixed 868 MHz image
calibration is reused for another center: LF image calibration follows the requested
frequency within its documented 400–1000 MHz receive range and is omitted elsewhere.

```sh
python host/tx.py play build/tone.s16be --repeat 147 --frequency-hz 433920000 \
  --pa hp --power-dbm -9 --out build/tx-433
python host/tx.py play build/tone.s16be --repeat 147 --frequency-hz 868350000 \
  --pa lp --power-dbm -10 --ramp-us 80 --out build/tx-lp
python host/tx.py play build/tone.s16be --repeat 147 --frequency-hz 2440000000 \
  --pa hf --power-dbm -18 --out build/tx-hf
```

The offset file is independent of center frequency. Changing PA, power or center
requires a new playback session. Both existing hardware deadlines remain enabled.

## External modulation controls

See [external control and wider offset encoding](tx-controls.md) for normalized
input, arbitrary requested rates, the Python API and compatibility requirements.
Parameters are fixed when playback starts; this interface does not retime an
already running transmission.

## Generate and play a reference waveform

Input files contain big-endian signed16 offsets, one per frequency update. The
expansion is `center_word + (offset << offset_shift)`, where
`center_word = floor(frequency_hz * 16384 / 15625) | 1`, giving approximately 1.90735 Hz steps at the default shift 1
and −62.500 to +62.498 kHz deviation about the selected center. This is frequency
quantization, not an I/Q or audio DAC bit depth.

```sh
python host/tx.py tone build/tone.s16be
# 4096 samples, 1513.671875 Hz tone at 200 kHz, about +/-38.086 kHz deviation.

python host/tx.py play build/tone.s16be --repeat 147 --rate 200000 \
  --timeout-ms 3800 --out build/tx-short

# No SPI traffic while the same waveform repeats from the two preloaded banks:
python host/tx.py play build/tone.s16be --repeat 147 --rate 200000 \
  --preloaded --timeout-ms 3800 --out build/tx-preloaded

# 15.012 s playback, tagged banks, deliberate host pause:
python host/tx.py play build/tone.s16be --repeat 733 --rate 200000 \
  --tag-blocks --pause-block 400 --pause-ms 40 --timeout-ms 15800 \
  --out build/tx-long

# 250 kHz: 31 tone cycles per 4096 updates, 12.009 s streamed playback:
python host/tx.py tone build/tone250.s16be --rate 250000 --frequency 1892.08984375
python host/tx.py play build/tone250.s16be --repeat 733 --rate 250000 \
  --tag-blocks --timeout-ms 12809 --out build/tx250-long
```

Output directories and tone files must be new. `--tag-blocks` adds a small distinct
DC offset to adjacent banks, exposing stale uploads hidden by identical periodic
content. It can push full-scale input out of range; validation rejects that before
opening the device. Preloaded mode requires exact repetitions of the first 4096
offsets, so it cannot be combined with bank tags. `--standby` exercises timer/RAM
playback without calling native TX. Requested rates may be any finite value in
20–400 kHz (research ceiling); the actual nominal rate is `32000000 / round(32000000 / requested)`; whole 2048-update banks are required, with 4096 minimum for preloaded and
6144 for queued playback. Maximum 3,200,000 updates and 60 s native timeout.

The host prepares finite-session frames/checksums before arming playback, avoiding Python packing work during refills. The default native timeout is waveform duration plus 500 ms. The independent Pico
alarm is native timeout plus 2000 ms and cannot be extended by a refill. A separate
3 s watchdog detects a stalled Pico; servicing it does not extend the alarm.
USB disconnect trips the radio reset guard. At completion the host verifies state,
checksum, sparse frequency readbacks, timeout IRQ and guard, then resets the radio.
On failure it retains `result.json` before cleanup and closes a partial binary
session instead of injecting ASCII commands into it. A watchdog reboot may require
reopening USB; `guardstatus` retains phase/block/index breadcrumbs for that boot.

`result.json` includes native state, timestamps and queue metrics; `serial.jsonl`
records control exchanges. A passing run ends at the requested index with reason 2,
checksum/source matches, no error flags and an unexpired Pico guard. A failure is
reported as a failure, including missing data and cleanup errors. Playback performs
no firmware installation and does not automatically restore the receiver profile.

## Transport and implementation

`txfill BLOCK CHECKSUM` accepts 4096 bytes after a ready response. `txqueue BLOCKS
PREFILL [GAP_US POLL_US [CHUNK_WORDS OPTIONS]]` accepts 4104-byte frames: BE32 sequence, BE32 sum of expanded
frequency words, then 1024 BE32 pairs with the earlier signed16 offset in the low
half. Sequence starts at 2 after the host loads banks 0 and1. Queue depth is 8;
the Pico writes sixteen 256-byte native RAM commands per bank and publishes the
ready sequence last. The LR checks the sequence at each 2048-update boundary.

`CHUNK_WORDS` is 16, 32 or 64 (default 64). OPTIONS is a bitmask:
bit 0 polls while consuming cyclic dummy frames without writing/publishing banks;
bit 1 predicts the next bank release and waits until about 500 us before it;
bit 2 uses Pico DMA for queued SPI bank writes. Default OPTIONS=0.
The host exposes these as `--probe-reads`, `--adaptive-poll`, `--spi-dma` and
`--chunk-words`. They are research controls; shorter chunks and DMA have not
improved the measured streaming ceiling. The probe requires cyclic input and
at least three banks; its `published` counter is zero and refill margins are
undefined (UINT32_MAX). `consumed_frames` counts frames removed from the USB queue.

The four-argument queue defaults remain 100 µs post-CS guard/500 µs polling;
`host/tx.py` requests 2/100. Bounds are 2–100 µs guard and 20–500 µs polling. BUSY
handshaking remains enabled. Ordinary configuration/installation retains 100 µs
guards. No SPI overclock was introduced by this optimization.

The player preserves synchronization, checksum/index updates and bank sequencing.
State offset+108 holds the session center word and +160 holds the offset shift;
both Pico upload/checksum paths and the ARC ISR expand offsets against them.
The configurable shift adds one RAM read per update relative to the 250 kHz build. The diagnostic routine reports ABI marker `0x46535435`; the host
and vector verifier reject an older player before starting playback.
RTC and frequency readback occur only on sparse trace updates. The ISR touches only r0–r3/r9 and never modifies blink; it no longer saves r4–r7 or checks a pending log pointer on every update. State +68 is now reserved. The host and Pico validate center-word headroom, so every signed16 encoding can
be expanded without overflow. Per-bank range/sequence/checksum checks remain active.

Timing records contain index, entry phase, exit phase, RTC, post-write observation
phase, requested word, readback and CONTROL0. The final RTC is sampled by the
completion IRQ one period after the final update. Sparse bracket duration omits
entry, register restoration and the final return tail. This exit bracket changed relative to txopt5; do not directly compare the old and new bracket as full ISR duration. The foreground observer instead measures complete
interruptions using two consistent IRQ-count/Timer0 snapshots separated by 8 NOPs;
subtract its measured no-IRQ baseline. A multi-IRQ interval is not one ISR duration.
Its unwrapping assumes timer expiries are accounted for; it cannot prove the
absence of isolated coalesced expiries. See [measurements](tx-measurements.md).

The trace is a 128-record ring, one record per 2048 updates. Check `host/tx.py`
`STATE_NAMES` against `lr1121/tx/player.s` before interpreting legacy state offsets.

## Restore receive operation

```sh
python lr1121/install.py forge --payload build/lr1121/stream.bin --out build/lr1121
python lr1121/install.py install --payload build/lr1121/stream.bin --out build/lr1121
python tools/build_pico.py --binding build/lr1121/binding.cmake \
  --toolchain /path/to/arm-gnu-toolchain
```

Flash `build/pico/lr1121_bridge.uf2` through BOOTSEL. The application install removes
the TX vector fragment and re-verifies the ordinary extension; the normal Pico
profile restores receive streaming. Both operations are explicit. Merely resetting
the radio after a test leaves the experimental extension installed but inactive.

## Modulation scope

NFM voice, audio FSK over FM, paging/SSTV and shaped 2/4-FSK are natural applications
of this frequency-only path. Earlier POCSAG/SSTV experiments are evidence of
feasibility, not protocol encoders shipped in this CLI. Codec/framing, filtering,
interoperability, RF output calibration and spectrum measurements remain separate
work. The Flipper pulse recorder used for the RF checks is not a dependency of
this package; its historical RF measurements are reported, not reproduced by
`host/tx.py`. Use an independent receiver for external validation.

At 200 kHz,3 kHz audio has about 67 updates/cycle. With audio limited to 3 kHz,
Carson's estimate `2*(deviation + audio bandwidth)` gives about11 kHz occupied
bandwidth for ±2.5 kHz deviation (typically 12.5 kHz channels), or 16 kHz for ±5 kHz
(typically 25 kHz channels). These are estimates, not emission-mask measurements.
The control sequence's 100 kHz mathematical Nyquist frequency is not a measured
analog modulation bandwidth. A shift of 2 can encode ±75 kHz deviation, but this
does not establish broadcast-FM fidelity or spectral performance. Independent amplitude control and general QAM/OFDM/SSB are not provided.

References: [NTIA necessary bandwidth](https://redbookdev.ntia.gov/view/J-19),
[ETSI EN 300 086](https://www.etsi.org/deliver/etsi_EN/300001_300099/300086/02.01.02_60/en_300086v020102p.pdf).
