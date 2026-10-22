# Hardware

Validated board: Waveshare Core1121-XF, **Core1121-HF / 850–930 MHz** variant.
“HF” in this product name describes the Sub-GHz matching variant. In the SDR++
source, **LF** means the LR1121 Sub-GHz RF input and **HF** means its 2.4 GHz input.
They are not the same naming convention.

## Pico wiring

| Core1121 signal | RP2040 GPIO | Pico physical pin |
| --- | --- | --- |
| SCK | GP10 / SPI1 SCK | 14 |
| MOSI | GP11 / SPI1 TX | 15 |
| MISO | GP12 / SPI1 RX | 16 |
| NSS / CS | GP13 | 17 |
| BUSY | GP14 | 19 |
| NRESET | GP5 | 7 |
| DIO9 / IRQ | GP15 | 20 |
| VCC | 3V3 OUT | 36 |
| GND | GND | e.g. 18 |

Use 3.3 V logic and short connections with a common ground. BUSY and NRESET are
required; the streaming implementation polls state rather than relying on IRQ.
USB supplies the Pico. This pin map is compiled into `pico/firmware/main.c`.

The Waveshare setup selects a 3.0 V TCXO supply and 300 RTC ticks of startup
(~9.16 ms), DCDC regulation, external LF crystal with ready wait, and RF-switch
mapping `03 00 01 02 02 00 00 00` (enable, standby, RX, TX_LP, TX_HP, TX_HF,
GNSS, Wi-Fi). These board-specific settings are in `host/lr1121.py` and
`sdrpp/lr1121_source/src/device.cpp`. Porting to another board requires reviewing
both. `HF_XOSC_START` after reset can be a latched consequence of starting the
wrong clock configuration; initialization clears it only after board setup,
then calibrates and checks crystal standby.

The HF board's Sub-GHz matching is optimized for 850–930 MHz. A 433 MHz antenna
alone does not convert that matching network into the LF board variant. Weak
433 MHz or broadcast-FM sensitivity therefore does not establish a digital
capture problem. Reception around 91.6 MHz was observed on strong local stations;
it is not a verified sensitivity or coverage specification.

Optional reference source: Flipper Zero, official firmware 1.5.1, BLE **Full**
(stack type 1). Sub-GHz carrier tests use its CC1101; 2.4 GHz tests require the
`bt tx_carrier` debug command. See [validation](validation.md). No Flipper firmware
or coprocessor updater is part of this repository.
