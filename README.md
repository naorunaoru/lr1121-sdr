# LR1121 SDR

SDR receive and experimental frequency-playback transmit work with a Semtech LR1121, an RP2040 Pico USB/SPI bridge,
and a native receive-only SDR++ source module. Tested with a Waveshare Core1121-XF **HF board
variant** (850–930 MHz matching on its Sub-GHz connector) and an original Pico.

The receiver exposes 62.5, 125, 250 and 500 kS/s complex streams. The two higher
rates use an ARC extension appended to LR1121 transceiver firmware **1.3**;
500 kS/s uses saturated 8-bit I/Q and a custom SPI FIFO service routine.
The extension installer leaves the LR1121 bootloader unchanged.

| Rate | Transport | Component representation | LR1121 extension |
| --- | --- | --- | --- |
| 62.5 / 125 kS/s | LIQ1 | signed 23-bit values in 32-bit containers | No |
| 250 kS/s | LIP2 | packed 12-bit, automatic per-bank scale | FIFO1 |
| 500 kS/s | LIP3 | packed 8-bit, manual shift 0–16 | FIFO1 |

**500 kS/s is not 500 kHz of flat RF bandwidth.** At 500 kS/s, measurements gave
about 100 kHz within 1 dB, 173–176 kHz at −3 dB, and 243–245 kHz at −6 dB.
See [measurements and validation](docs/validation.md).

## Experimental TX

An opt-in Pico profile and ARC player support frequency updates through 250 kHz,
with [external rate and deviation controls](docs/tx-controls.md).
Three 12-second 250 kHz streams passed delivery checks with at least 1.916 ms
refill margin; interrupt jitter remains. See [250 kHz measurements](docs/tx-250k.md).
Three 15-second 200 kHz runs completed with correct checksums and no player underruns;
minimum refill margin was 5 ms. Startup reliability, update jitter and spectral
purity remain unresolved. Center frequency and LP/HP/HF PA power are configurable. The path is separate from
SDR++ and without independent amplitude control. See [TX build/use](docs/tx.md)
and [measurements, including the failed startup](docs/tx-measurements.md).

## Layout

- `pico/`: Pico SDK C firmware; default receive bridge and opt-in `tx/` profile.
- `sdrpp/lr1121_source/`: native C++ source, decoder, command-line probes and tests.
- `sdrpp/patches/`: SDR++ registration, defaults and waterfall bounds fix.
- `lr1121/`: ARC receiver extension, experimental `tx/` player/vectors, installers and tests.
- `host/`: Python bridge, board setup, TX waveform/player CLI and optional Flipper reference carriers.
- `tools/`: builds, macOS app packaging and offline checks.
- `ci/`: optional GitHub Actions workflow template.
- `external/`: pinned Git submodules; no copied SDK/build trees.
- `docs/`: hardware, installation, operation, internals and measured limitations.

## Reproduce

```sh
git clone --recurse-submodules https://github.com/naorunaoru/lr1121-sdr.git
cd lr1121-sdr
```

Follow [hardware](docs/hardware.md), [build](docs/build.md), then
[LR1121 installation](docs/lr1121-install.md). For an already provisioned module,
run the extension verification and regenerate its Pico binding instead of
reinstalling LR1121 firmware. [Operation](docs/operation.md) covers source controls,
scaling and recovery; [internals](docs/internals.md) documents the capture path.

End-to-end operation and app packaging have been tested on macOS ARM64. The
serial implementation is POSIX; Linux is not yet hardware-validated and needs
an explicit serial path. Windows is not implemented.

These are firmware-specific experiments, not a supported Semtech SDR mode.
Frequency entry is limited by the PLL encoding, not a claimed RF coverage range.
Out-of-band reception and sensitivity depend strongly on board matching and
antennas. SPI at 20.833 MHz is experimental and may not work on other wiring.

## Credits and licensing

LR1121 custom-code work builds on
[radioegor146/lr-semtech-research](https://github.com/radioegor146/lr-semtech-research).
[LakeShark](https://github.com/SAMS0N1TE/LakeShark) and
[lr2021-sdr](https://github.com/SAMS0N1TE/lr2021-sdr) motivated the investigation;
the LR1121 capture implementation is separate.

Independent components (`pico/`, `lr1121/`, `host/`, and project-owned build/tools
code) use [MIT](LICENSE). The entire `sdrpp/` portion, including its source module,
core patches and tests, remains [GPLv3](sdrpp/LICENSE). Project documentation is
MIT unless it contains separately licensed upstream material. See the precise
[license scope](docs/licensing.md) and [third-party notices](THIRD_PARTY_NOTICES.md).
External dependencies and Semtech firmware retain their own licenses. Generated
images, device bindings, raw captures, USB logs and local SDR++ configuration are excluded from Git.
