# Operation

Use one client at a time: SDR++, `lr1121_probe`, `retune_probe`, or the Python
bridge. Stop SDR++ before using a probe. The native driver claims the serial
device exclusively and uses the same configuration and frame decoder in both the
UI and command-line receiver.

## Source controls

- **Rate:** 62.5, 125, 250 or 500 kS/s. 250/500 require a verified FIFO1 module and
  a Pico build bound to its UID.
- **Input:** Auto, LF or HF. Auto follows the firmware's frequency selection;
  explicit LF/HF applies register overrides with readback. LF selects the
  Sub-GHz connector, HF the 2.4 GHz connector. The source does not claim that
  arbitrary frequencies will lock or have useful sensitivity.
- **Filter:** narrow LoRa or wide GFSK; 250/500 require wide GFSK. The filter and
  sample-rate settings are coupled. “Wide” is relative to the narrow path, not a
  flat response across the whole Nyquist interval.
- **Hardware AGC:** uses the chip's gain control. When disabled, manual gain step
  is 4–13; these are register steps, not dB. LF boost is 0–7; HF boost is 0–3.
- **8-bit shift:** 0–16, default 15, for 500 kS/s only. Hardware gain and this
  quantizer setting are independent.
- **Digital gain:** scales decoded floating-point samples for SDR++; it cannot
  restore quantization detail or RF sensitivity.

At 500 kS/s the LR1121 sign-extends each raw 23-bit component, arithmetic-shifts
it, and saturates to `[-128,127]`. Decoding uses
`float = int8 × 2^(shift−22)`. Lower shifts preserve smaller signals but reduce
headroom; shift 15 accommodates the full signed-23 input range. Start at 15,
select sensible hardware gain, then lower the shift while watching the waveform
and endpoint counter. Values at −128/+127 can be valid, so that counter is an
overload indicator, not an exact clipping measurement. It also cannot detect
analog clipping upstream. Low-level signals with too few quantization levels,
analog overload, narrow filters and discontinuities can all produce harsh audio.

250 kS/s uses 12-bit packing with a scale chosen per 125-sample bank. 62.5/125
transfer the original signed-23 containers without this reduction.

## Tuning and stopping

Retuning drains/stops the old stream before changing the receiver. Nearby tunes
reuse configuration only when rate, input, filter, gain, calibration coverage and
RSSI profile are compatible. Crossing those boundaries performs full setup.
Gain and shift changes briefly restart capture. Retuning has a deliberate gap;
continuity claims apply within one stream, never across separate tunes.

A CRC error, sequence gap, bad dimensions/rate, ownership failure or DSP queue
overflow stops reception rather than silently filling a gap. A normal stop has
terminal flags `4`. Stop and restart after an error; if the USB bridge is stuck,
reconnect normally. BOOTSEL is for reflashing the Pico, not ordinary recovery.
USB reset shortcuts are disabled in this firmware.

## Command-line capture

```sh
export PORT=/dev/cu.usbmodemYOUR_PICO
PROBE=build/sdrpp/source_modules/lr1121_source/lr1121_probe
"$PROBE" --ports
"$PROBE" "$PORT" 500000 1 868350000 30 build/capture.cf32 10 0 wide 15
```

Arguments are `PORT RATE INPUT FREQUENCY [SECONDS] [CF32_FILE|-] [agc|GAIN_STEP]
[BOOST] [narrow|wide] [SHIFT_0_16]`. Input is 0=Auto, 1=LF, 2=HF. Specify `wide`
for 250/500; the probe's omitted-filter default is narrow. CF32 is interleaved
native float32 I,Q (little-endian on the tested host), before SDR++ digital gain.
The JSON summary includes setup readbacks, actual SPI clock and terminal counters.
An exception exits nonzero; zero error counters in a successful summary mean the
decoder accepted the entire stream.

On macOS, the UI can enumerate the bridge by USB product name. Elsewhere enter
the device path manually; automatic enumeration is currently macOS-only.

## Frequency and sensitivity limits

Frequency requests must be nonzero and fit the firmware's 32-bit PLL encoding
`floor(f_Hz × 16384 / 15625)`. This deliberately permits exploration outside the
published bands. Readback proves the programmed divider, not RF performance or
lock over that whole range. The setup performs explicit image calibration only
between 400 MHz and 1 GHz; outside it, reset calibration is retained.

The measured 500 kS/s response is approximately flat only over the central
100 kHz. At ±240 kHz it is roughly 25 dB down. Increasing displayed span or digital
gain will not flatten that response. Board matching, antenna, RF-input choice,
analog gain and digital decimation/filter response are separate limitations.
