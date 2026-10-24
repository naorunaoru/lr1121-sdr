# GNU Radio integration

The first integration path is file based. It uses the existing verified receive
transport and finite TX player; GNU Radio itself is not a dependency of the
firmware or these helpers. A live out-of-tree module is proposed below, not yet
implemented or tested. This revision does not claim a GNU Radio runtime test.

## Receive into a flowgraph

Build the existing GPL receiver capture utility without the SDR++ GUI:

```sh
python tools/build_probe.py
build/tools/lr1121_probe --ports
build/tools/lr1121_probe /dev/cu.usbmodem1101 500000 1 868350000 2 build/rx.cf32 agc 0 wide 15
```

Use the ordinary, UID-bound receive Pico firmware and receiver extension. The
utility shares `sdrpp/lr1121_source/src/device.cpp`: configuration, gain, input
selection, filter and scaling controls, packet CRCs and sequence validation are
identical to SDR++. Its JSON report goes to stdout; the named file contains
interleaved native-endian float32 I,Q pairs (complex64 on the tested little-endian
Mac). Do not interpret stdout as samples. Existing output files are overwritten
by the probe. Capture includes queued samples drained during Stop; use the report's
actual sample count rather than assuming an exact requested duration.

In GRC, use a **File Source (Complex)** at the selected sample rate, followed by
analysis/demodulation blocks. Add a Throttle for interactive replay when no other
block supplies pacing. Rates are 62500, 125000, 250000 and 500000 complex samples/s.
At 500 kS/s the wire format is LIP3 with signed8 block scaling, expanded to float
by the transport; a float file does not imply float precision in the receiver.
The usable analog bandwidth remains the measured receiver passband, not simply
sample rate. This is an offline capture workflow, not a live source block.

## Frequency modulation from a flowgraph

TX takes **real-valued instantaneous frequency deviation in Hz**. Do not connect
an arbitrary complex modem output directly: the current player has no controlled
amplitude path and cannot reproduce arbitrary QAM/OFDM/IQ waveforms.

For a reproducible first test, generate a real sinusoid with amplitude 2500 Hz,
frequency 1000 Hz and sample rate 250000; use a Head of 602112 float items and a
File Sink (Float) to `build/deviation.f32`. On the tested little-endian host this
is float32-LE. A GNU Radio Frequency Modulator block is unnecessary: LR1121 is
receiving the frequency trajectory itself.

```sh
python host/fm_file.py build/deviation.f32 build/deviation.s16be --rate 250000 \
  > build/deviation-format.json
# Use offset_shift from the JSON report (1 for this example).
python host/tx.py play build/deviation.s16be --rate 250000 --offset-shift 1 \
  --frequency-hz 868350000 --pa hp --power-dbm -9 --out build/gr-fm-test
```

Install the matching TX player, vectors and Pico profile first, as described in
[tx.md](tx.md). Conversion performs no USB access or transmission. It rejects
nonfinite values, incomplete banks, out-of-range deviations and existing output
files. It neither clips nor pads nor resamples. Length is 4096..3200000 updates
in multiples of 2048; queued playback needs at least 6144. It automatically
chooses a PLL encoding shift and reports the quantization error. Retain the JSON
metadata with the raw file; raw signed16 files do not carry scale or sample rate.

For noninteger timer periods, generate the waveform at `rate_actual` reported by
`tx.timing_settings(requested_rate)` (32 MHz divided by rounded timer period).
Pass the original requested rate to playback. Otherwise a tone generated for the
requested rate plays at a slightly different frequency. Both TX deadlines remain
active, and a completed checksum does not prove uniform update timing.

## Validation of these file interfaces

The standalone utility captured 1014750 complex samples at 500 kS/s, with zero
reported CRC/sequence errors and a clean terminal Stop. A synthesized float-Hz
file was converted and played for 6144 updates at 250 kHz with matching checksum
and retained frequency readbacks. This tests the adapters, not GNU Radio itself
or RF audio fidelity. The receiver's firmware allowlist now includes the existing
ordinary bridge version 1.0.8; the same fix applies to the SDR++ source.

## Proposed live interface

A small GNU Radio OOT module is a better next step than duplicating configuration
inside Embedded Python Blocks:

* `lr1121_source_c`: complex64 output, the existing C++ Device transport and
  configuration, and a status message port for CRC/sequence/overflow errors.
* `lr1121_frequency_sink_f`: float32 deviation-Hz input, fixed center/PA/rate/scale
  at start, bank packing and a bounded worker queue. Initially use explicitly
  sized finite bursts; report quantization, refill margin and final checksum.
* One worker owns serial I/O. Scheduler callbacks consume/produce bounded buffers;
  Stop wakes the worker, drains/cancels the binary protocol, then resets/standbys
  the radio. Never mix ASCII commands into an active binary transfer.

RX must stop and report a discontinuity if downstream processing cannot keep up.
TX must report underrun rather than silently repeating or inserting samples.
Do not add a Throttle to a live hardware source. The current Pico and application
profiles differ, so automatic profile deployment/caching is a separate necessary
usability step. They are half duplex; a source and sink cannot own the USB port
simultaneously. The current finite TX protocol cannot transparently support an
unbounded GNU Radio stream without changing count/deadline/session semantics.

Reuse of the existing C++ receiver makes the OOT module GPL-3.0-only, matching
that code; independent file conversion and host TX components remain MIT.
A small standalone capture build already removes the SDR++ GUI dependency.
For live blocks, prefer a single transport implementation shared by SDR++ and
GNU Radio over two independently maintained copies.

GNU Radio references: [OOT modules](https://wiki.gnuradio.org/index.php/OutOfTreeModules),
[block types](https://wiki.gnuradio.org/index.php/Types_of_Blocks),
[Embedded Python Block](https://wiki.gnuradio.org/index.php/Embedded_Python_Block).
