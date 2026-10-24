# ISR overhead and SPI limits above 250 kHz

Measured 2026-10-24, Core1121-XF HF, LR1121 transceiver 1.3, RP2040, 868.350 MHz,
HP −9 dBm requested, physical SPI 15.625 MHz. Bootloader identity stayed
`22df2100`. No RF fidelity/spectrum measurement or Flipper recording was made in
this campaign. Both native TX timeout and independent Pico reset deadline stayed
active; the ordinary receiver was restored after each campaign.

## Result

250 kHz has more refill margin with the shorter ISR. Requested 275 kHz
(timer setting **275862.069 updates/s**, period 116 clocks) is the highest tested
rate with repeated long streamed completions. It has less timing margin and more
SPI-associated delay than 250 kHz. Requested 300 kHz selects **299065.421 updates/s**
and is unreliable: short successes coexist with publication failures and USB
losses on long attempts. Do not treat the 400 kHz research acceptance ceiling as
a supported streaming rate.

Preloaded, repeating playback passed short controls at 320 kHz, 351648.352 and
400 kHz. This demonstrates timer/register playback without ongoing new waveform
data; it does not demonstrate a continuous 400 kHz host-to-radio stream.

## What changed

The ISR no longer saves/restores blink or r4–r7, calls the sample handler, checks
a pending-log pointer on every update, or rereads the stop reason after each
sample. It uses direct branches and only r0–r3/r9. Full checksum, index, bank
sequence validation, frequency write synchronization, sampled register readback,
and endpoint RTC timestamps remain. The final trace cadence is one record per
2048 updates; a separate intermediate build retained three for comparison.

Pico reports per-command and aggregate SPI-service, post-transfer BUSY-wait and
USB-pump times, alongside refill lead, queue occupancy and failure snapshots.
Optional controls select 16/32/64-word chunks, polling without bank writes,
predictive boundary polling, and Pico SPI DMA. Defaults remain 64-word chunks,
2 us guard, 100 us polling, ordinary CPU SPI. Neither shorter chunks nor Pico DMA
raised the measured streaming ceiling.

The host now prepares the finite session's binary frames and checksums **before**
arming playback. Previously it generated each frame while transmitting. This
costs at most about 6.4 MB of additional packed frame storage and avoids that
Python work in the refill path. It does not protect against arbitrary host stalls.

ABI is `0x46535435`, Pico `1.0.8-txopt6`, final 4076-byte player SHA-256
`04d325b7ea603df41e08e88da8b5efdbf62a1c2a1fc27b5ccf33345453529e56`.
The receiver prefix is unchanged. Reinstall the matching player/vectors and Pico
profile before using the new host CLI.

## Matched short comparison at 250 kHz

Each case requests 602112 updates. The baseline uses the published txopt5 ISR,
with only its ABI marker and minimum-period guard changed for the comparison.
`lean3` retains three diagnostics per bank; `lean1` is the final sparse version.

| Case | Worst bank write | Minimum published lead | Outcome |
| --- | ---: | ---: | --- |
| Baseline, 64 words | 5.626 ms | 1.864 ms | Passed |
| lean3, 64 words | 5.062 ms | 2.556 ms | Passed |
| lean1, 64 words | 5.053 ms | 2.508 ms | Passed |
| lean3, 32 words | 5.899 ms | 1.728 ms | Passed |
| lean1, 32 words | 5.975 ms | 1.608 ms | Passed |
| lean3 / lean1, 16 words | 7.670 / 7.636 ms | Deadline missed | Both failed |

The 16-word case quadruples the command count. Smaller individual transfers do
not compensate for command handling and BUSY overhead. Bank duration is 8.192 ms
at 250 kHz, but setup, polling, status checks and sequence publication also need
part of that window.

Preloaded baseline had the next interrupt pending at all 128 retained diagnostic
CONTROL0 observations; lean3 and lean1 had zero. **The observation point changed**:
the new CONTROL0/exit timestamp is before register restoration. This is not a
like-for-like measurement of full ISR duration and zero flags do not prove every
IRQ returned before the deadline. The unchanged foreground observer saw a 3.875 us
interruption in the optimized preloaded run. Its maximum is a sampled observation,
not a proven worst-case bound. At 275 kHz it saw two-IRQ chains even without SPI.

A polling-only control used cyclic preloaded samples while consuming dummy USB
frames, with no RAM writes or ready-sequence publication. It still produced
multi-IRQ interruption windows: baseline 27.625 us, lean3 19.438 us, lean1 14.313 us.
Consequently SPI command processing itself contributes; the problem is not limited
to copying bank contents. These windows include several interrupts/native work,
not one ISR. Early probe reports incorrectly called consumed dummy frames
`published`; the final code reports `published=0`, `consumed_frames` separately,
and leaves refill leads undefined (UINT32_MAX) in that control.

## Where refill time goes

In the short predictive-poll CPU-transfer controls, average measured contributions
per 4096-byte bank were approximately:

| Requested rate | SPI service | Post-transfer BUSY waits | USB pump inside transfers |
| --- | ---: | ---: | ---: |
| 250 kHz | 2.593 ms | 1.662 ms | 0.296 ms |
| 275 kHz | 2.612 ms | 2.025 ms | 0.351 ms |

These are **software brackets**. `spi_clock_*` includes the SPI routine and, for
DMA, its setup/wait; it is not an oscilloscope measurement of wire-active time.
`max_transfer_us` also includes CS guard and USB pumping. BUSY-wait brackets can
service USB while waiting. They do not include every operation in total bank
latency, and independent maxima must not be added as though they occurred together.

Pico DMA gave about 2.577 ms SPI service/bank at 250 kHz and 2.578 ms at 275 kHz,
a small change with no useful increase in the streaming ceiling. Meanwhile BUSY
waits grow with the LR interrupt load. Predictive polling reduced foreground
interruption counts but did not materially improve worst refill margin. This
points toward LR native SPI-command service/available CPU time as the main next
target, rather than a larger Pico queue alone. A queue can absorb host stalls;
it cannot fix a bank that intrinsically arrives after the LR consumes it.

## Timing versus successful delivery

RTC means use `(count+1)*32768/(rtc_done-rtc_start)`: the completion timestamp is
one interrupt after the last update. The RTC is not an external calibrated clock.
For 3002368 updates at 250 kHz, optimized preloaded, fixed-poll and predictive-poll
controls all took **393577 RTC ticks**, including a predictive run with a 40 ms
host pause. This removes the earlier detectable streamed/preloaded endpoint
separation in those runs; it does not prove zero per-update jitter.

At requested 275 kHz the preloaded long control took **356679 ticks**.
Predictive CPU streams took 356719–356720; DMA streams took 356734–356736.
The extra 40–57 ticks correspond to 1.22–1.74 ms over roughly 10.9 seconds.
This is consistent with SPI-induced late/coalesced timer service. Non-simultaneous
oscillator drift is not excluded. All these completed cases matched checksums,
final indices and retained source/register samples. A checksum does not establish
uniform spacing or independently verify every individual RF update.

At requested 300 kHz the short fixed-poll stream had only **40.125 us** published
lead and a mean of 298600.663 updates/s versus 299030.597 for its matched preloaded
control. Longer CPU and DMA attempts lost USB (`Device not configured`); the
exact reset/USB failure cause was not captured. They are failures, not successful
high-rate runs. Hardware recovered automatically and both timeouts remained enabled.
320/350/400 kHz streams also failed their bank deadlines.

## Host starvation and final repetitions

One long 275 kHz run before host prepacking failed at index 2392064 with reason 4,
26 USB-starved polls and only one queued bank. Its maximum bank write was 5.594 ms,
so this differed from the immediate high-rate command-throughput failures.
The requested 40 ms pause was at bank 400; failure occurred much later, around
bank 1168. The evidence identifies queue starvation, not the precise cause of the
host scheduling delay. This failure is retained in the dataset.

The final three prepacked 275 kHz runs all passed: 9007104 updates total,
zero USB-starved polls, reason 2, full checksum and retained source/register matches.
They use ordinary fixed polling and CPU SPI; DMA/predictive controls remain opt-in.

| Final run | Injected host pause | Minimum published lead | Maximum bank write | RTC ticks |
| --- | ---: | ---: | ---: | ---: |
| 275 kHz run 1 | 0 ms | 1.062125 ms | 5.549 ms | 356733 |
| 275 kHz run 2 | 40 ms | 1.065750 ms | 5.564 ms | 356728 |
| 275 kHz run 3 | 40 ms | 0.946125 ms | 5.620 ms | 356733 |

The final prepacked 250 kHz run also passed with a 40 ms pause, zero starvation,
and 2.500 ms minimum published lead. These repetitions support the improvement;
they do not erase the earlier starvation failure or guarantee tolerance of an
arbitrary host scheduling stall.

## Reproduce

Follow [TX installation](tx.md). A short test, followed by a long one:

```sh
python host/tx.py tone build/tone275.s16be --rate 275000 \
  --frequency 2087.823275862
python host/tx.py play build/tone275.s16be --rate 275000 --repeat 147 \
  --tag-blocks --out build/check275-short
python host/tx.py play build/tone275.s16be --rate 275000 --repeat 733 \
  --tag-blocks --pause-block 400 --pause-ms 40 --out build/check275-long
```

The tone is 31 cycles per 4096 updates at the actual timer rate. For preloaded
control omit tags and use `--preloaded`; for polling-only use `--probe-reads`
without tags. Add `--chunk-words 32`, `--adaptive-poll` or `--spi-dma` separately
for controlled comparisons. Default rate remains 200 kHz. For new experiments,
first establish the desired baseline again on the actual host/hardware.

[Sanitized measurements](measurements/tx-isr-spi.json) retain failures, sparse
traces, on-chip state, queue metrics and source-variant hashes. Raw session logs,
module-bound images, local recovery caches and captures remain in ignored build
storage. The [GNU Radio workflow](gnuradio.md) documents the independently tested
file interfaces and the proposed live source/frequency-sink design.

Validation: 49 timing trials (38 passed, 11 failed), plus the 6144-update float-file TX smoke test. The standalone RX capture delivered 1014750 complex samples at 500 kS/s with zero reported CRC/sequence errors and a clean Stop. All 41 Python tests and native parser/format/frame/retune/waterfall checks passed. Both Pico profiles and the ARC payload built.
