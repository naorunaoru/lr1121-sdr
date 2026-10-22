# LR1121 application and extension

This installer is specific to LR1121 transceiver **0x0103** and bootloader identity
**`22df2100`**. It recognizes stock application identities `22030101` and
`22030103` before requesting bootloader entry. Other identities stop the update
before erase. Firmware 1.5 or another Semtech product is not a compatible base.

The encrypted stock application is loaded from the pinned Semtech submodule:

```text
lr1121/transceiver/lr1121_transceiver_0103.bin
66592 bytes
SHA-256 d7e199871884a52666c49cd443c94318ee3fabceb8563548f41ebbb4458e9645
```

Read its adjacent `lr1121_transceiver_0103_license.txt` for redistribution terms.
This repository does not distribute generated custom images or decrypted vendor
firmware. The payload sources and installer are sufficient to reconstruct the
extension using the connected module as a CBC oracle.

## Provision

Use the unbound Pico build from [build.md](build.md). Close SDR++ and any other
serial client. `PORT` below is the Pico bridge device, not the Flipper.

```sh
export PORT=/dev/cu.usbmodemYOUR_PICO  # Linux: e.g. /dev/ttyACM0
python host/bridge.py --port "$PORT" info
python host/bridge.py --port "$PORT" version
```

If the application is still 1.1, install the checksum-pinned stock 1.3 image:

```sh
python lr1121/install.py stock --port "$PORT"
```

For an existing **stock** 1.3 module, skip that command. Build and install FIFO1:

```sh
python tools/build_extension.py
python lr1121/install.py forge --port "$PORT"
python lr1121/install.py install --port "$PORT"
```

`forge` requires a running 1.3 application. It uses command `0x050f` as the
oracle, verifies controlled plaintext bytes, and writes:

- `build/lr1121/custom-app.encrypted.bin`
- `build/lr1121/payload-verified.bin`
- `build/lr1121/image.json`

`install` checks the manifest, payload hash, stock prefix, padding and total size
before opening the device. It verifies bootloader identity, erases/writes the
**application** using `0x8000` / `0x8003`, reboots with `0x8005`, and checks the
bootloader identity again. It implements no bootloader-update opcode. Keep USB
power stable during application erase/write.

The extension's callback self-test must produce IQP3 (`0x49515033`) at `0x80f000`,
FIFO1 (`0x46494631`) at `0x80f078`, and restore the saved callback at `0x8004ac`.
Only a successful check writes `build/lr1121/binding.cmake`. Rebuild and flash the
Pico with that binding as described in [build.md](build.md).

If FIFO1 is already installed, regenerate the binding without rewriting flash:

```sh
python lr1121/install.py verify --port "$PORT"
```

The binding is a compatibility guard, not cryptographic attestation of the whole
image. The native driver also checks firmware, markers, descriptor layout and
register readbacks before starting packed capture. Do not copy another module's
binding or bypass failed checks.

## Scope of verification

The original 66,592-byte encrypted application prefix is retained byte-for-byte;
the extension is appended at flash address `0x94500`, with entry `0x94510`.
Verification comprises pre-write oracle plaintext checks, accepted write-command
statuses, post-write application identity and the extension callback self-test.
It is **not** a full flash readback comparison. JSON reports and session logs
remain in the output directory, including failed attempts.

`--out`, `--payload` and `--vendor` override local paths. An alternate vendor file
must still match the pinned 1.3 digest. Outputs from `forge` and `install` must
share the same `--out` and payload.

## Recovery / removing the extension

The installer accepts an already-running known bootloader to retry an interrupted
application update. `stock` restores the pinned vendor 1.3 application and removes
the extension; it does not downgrade to 1.1. If the bridge or radio is unresponsive,
power-cycle before retrying. Restore Pico firmware via BOOTSEL if necessary.
Unknown bootloader identities are deliberately unsupported.

After restoring stock, rebuild the Pico without `--binding` and use only the two
raw modes. The Python RSSI helper can test the board without undocumented DMA
capture: `python host/bridge.py --port "$PORT" rx-start`, then `rssi`, then
`rx-stop`.
