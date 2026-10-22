# Capture and transport internals

All addresses below are observations for LR1121 transceiver **1.3**. They are not
an ABI for another release. Executable sources of truth are
`sdrpp/lr1121_source/src/device.cpp`, `lr1121/extension/{pack,stream}.s` and
`pico/firmware/{stream,packed_stream}.c`.

## Acquisition and rate

The receiver exposes alternating signed-23 I/Q components at `0x00901004`.
Acquisition DMA writes RAM independently of host SPI reads. The rate field in
`0xf20124[22:20]` is 5/4/3/2 for 62.5/125/250/500 kS/s; the driver also checks
`[25:24] == 1`. Wide GFSK profiles select those native decimation settings with
filter codes `0x0d/0x0c/0x0b/0x0a`. Narrow LoRa operation at 62.5/125 changes the
rate field after checking its original value. This is why increasing rate changes
the passband but does not remove filtering.

Gain state is checked through `0xf20214` and `0xf20230`; boost is in
`0xf3008c[18:16]`. Explicit RF input selection overrides fields at `0xf20014`,
`0xf3008c`, `0xf30024` and `0xf30094`. The full masks and expected values live in
`Device::configure`; all overrides are reapplied after vendor setup and verified.
They are volatile and disappear on reset.

## Packed ring

The packed path uses 16 banks at `0x806000 + bank*2048`, each containing 125
complex samples (250 32-bit components before packing). The first two DMA
descriptors remain at `0x800a00`; the remainder use `0x80e000 + bank*16`.
Control state is at `0x80f000`, with per-bank metadata at `0x80f100`.

The extension runs through the firmware callback at `0x8004ac`, scheduled through
`0x8004bc`. Its inactive self-test restores the original callback. During capture
it packs only a completed, invalidated DMA descriptor whose bank has not already
been marked ready. Metadata carries sequence, scale and ownership; unread banks
must not be overwritten. The 250 kS/s path has the host rearm the descriptor and
clear ready state. The 500 kS/s FIFO service acknowledges the previous transmitted
bank when moving to the next and checks that it is not the current DMA descriptor
(`0x800580`). An ownership conflict stops progress and is reported.

Simply copying packed RAM through the vendor SPI TX DMA was insufficient at
500 kS/s: individual replies could stall acquisition and shift I/Q pairing despite
correct USB transport. FIFO1 replaces that reply phase with controlled CPU FIFO
servicing while acquisition DMA remains active. It configures the SPI block at
`0xf12034`, preloads 56 bytes to `0xf12014`, then refills according to progress
from ARC auxiliary register `0x6a5`. Completion comes from `0xf12004`; a saved and
restored ARC timer bounds the service wait at 64,000 ticks (~2 ms in this setup).
The routine preserves/restores interrupt state and records service timeouts.
Inspect `stream.s` for the hardware-specific refill accounting.

The original encrypted application prefix is unchanged. The extension is linked
at `0x94510`; each encrypted 256-byte page has an uncontrolled first 16 bytes,
which the assembly skips with fixed `.org` placement. The builder rejects code in
those holes. The CBC construction follows radioegor146's research; it does not
require extracting an encryption key.

## Pico and USB

The RP2040 runs SPI1 and binary USB CDC. Requested 16 MHz gives 15.625 MHz for the
three lower rates; requested 24 MHz gives **20.833333 MHz** for 500 kS/s with the
standard 125 MHz Pico clock. The native driver checks these actual values. No
RP2040 core overclock is required. Signal integrity at the higher SPI clock is
board/wiring dependent.

Both stream paths share one statically allocated, aligned **166,320-byte** queue.
There is no per-session heap allocation for it. This avoids allocation/fragmentation
risk when switching rates; the earlier mixed-rate stall did not produce a panic
dump, so its original cause was not conclusively established.

- LIQ1: signed-23 containers at 62.5/125 kS/s; see `stream_format.{c,h}` and
  `decodeFrame` in the native driver.
- LIP2: 250 kS/s, 12-bit I/Q and automatic per-bank scaling; see
  `decodePackedFrame` and `packed_stream.c`.
- LIP3: 500 kS/s, 8-bit I/Q, fixed manual shift, grouped banks and reversible
  whitening. The full wire layout is [LIP3.txt](LIP3.txt).

Whitening avoids problematic USB bit patterns; it is not encryption. CRC covers
transmitted bytes, before unwhitening. Sequence numbers count 125-sample banks,
not USB frames. Frame integrity and queue counters establish transport continuity;
they cannot independently prove that the ADC/acquisition path never lost a pair.
Reference-tone phase tests address that separate question.

The SDR++ source has a bounded queue between serial reception and DSP. Its core
patch also guards waterfall signal-window indexes when a VFO lies partly or
wholly outside the displayed FFT; a focused native test covers that crash path.
