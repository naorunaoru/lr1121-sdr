# Build

## Dependencies

Source revisions are gitlinks in `external/`; do not update submodules with
`--remote` when reproducing these results.

| Component | Pinned revision |
| --- | --- |
| Pico SDK 2.3.1 | `079c6f39023649b154152db30f1d781e884879bc` |
| picotool 2.3.1 | `2041936441b48a3cc53ae3da9e805229fe8f4e18` |
| SDR++ | `8bedb1fbc107c31455fe8abd877332df5d2842d4` |
| Semtech radio firmware images | `e1e1a328567bc78018a072fda7d318a7d022899f` |
| GNU binutils 2.44 | `815d9a14cbbb3b81843f7566222c87fb22e7255d` |

The Pico SDK pins its own TinyUSB dependency. A recursive checkout initializes
it. For a selective checkout, initialize all five top-level submodules and then
`git -C external/pico-sdk submodule update --init lib/tinyusb`.

Host prerequisites: Python ≥3.12, Git, CMake, Make, a C/C++17 compiler,
`pkg-config`, libusb and an Arm bare-metal toolchain. ARM GNU
**14.3.Rel1** was used for Pico builds. ARC tools are built from the pinned
binutils source; no preinstalled ARC compiler is needed. SDR++ additionally needs
FFTW3 (single precision), GLFW, VOLK, PortAudio and their development headers.
System compilers/libraries are not vendored or locked by the source gitlinks.

macOS prerequisites, using Homebrew plus an Arm GNU toolchain installation:

```sh
brew install cmake pkg-config libusb fftw glfw volk portaudio
python3 -m venv .venv
. .venv/bin/activate
pip install -r host/requirements.txt
```

The tested host was macOS 27 ARM64. Bundle portability depends on the deployment
versions of the linked libraries; the build does not produce a universal binary.
On Linux, install the equivalent development packages and `gcc-arm-none-eabi`.
Only offline/native builds are intended to be portable so far; hardware and UI
operation on Linux remain unverified.

## Pico

```sh
python tools/build_pico.py --toolchain /path/to/arm-gnu-toolchain
```

Omit `--toolchain` if `arm-none-eabi-gcc` is on PATH. The script first builds the
pinned picotool, then produces `build/pico/lr1121_bridge.uf2`. This initial build
has no module binding: ordinary SPI commands and 62.5/125 kS/s work, while packed
250/500 kS/s are gated. Copy the UF2 to the Pico in BOOTSEL mode.

After [installing/verifying the LR1121 extension](lr1121-install.md):

```sh
python tools/build_pico.py --toolchain /path/to/arm-gnu-toolchain \
  --binding build/lr1121/binding.cmake
```

Flash that UF2 once more. The generated binding identifies the module on which
FIFO1 verification succeeded. Running the build without `--binding` explicitly
clears any binding cached by a prior build.

## ARC extension

```sh
python tools/build_extension.py
```

Outputs: `build/lr1121/stream.{o,elf,bin}`. The expected payload is **1,828 bytes**,
SHA-256 `085df4ad21ca8407515199ef5f6716cbfc5afe9dcfab8ca37d4e79c68144313e`.
The build checks the reserved CBC holes. Binutils documentation generation is
skipped (`MAKEINFO=true`); the assembler, linker and objcopy are built locally.

## SDR++

```sh
python tools/build_sdrpp.py --bundle  # macOS
# or: python tools/build_sdrpp.py     # native executable/modules
```

The script archives the pinned upstream tree into `build/sdrpp-src`, applies
`sdrpp/patches/lr1121.patch`, copies the source module, and builds into
`build/sdrpp`. It leaves the upstream submodule clean. A staging manifest checks
the upstream revision and patch hash; use a fresh `--build` directory after
changing either. Module-only changes can reuse the existing directory.

Only LR1121 source, Radio, Audio Sink, Frequency Manager and Recorder are enabled.
The macOS bundle is `build/SDR++ LR1121.app`, with copied shared libraries and
ad-hoc signatures. It includes the MIT and GPLv3 project licenses, license scope,
and third-party notices; see [licensing](licensing.md). Its launcher stores settings
in `build/config/` alongside the app. Bundle creation refuses to replace an existing app: move the old bundle
aside or choose a fresh build directory. Nothing is installed system-wide.

The command-line receiver is
`build/sdrpp/source_modules/lr1121_source/lr1121_probe` (also inside the bundle's
`Contents/MacOS/`). For a native non-bundle build, install/stage SDR++ with CMake
according to its upstream conventions; ensure its resources and module search
path refer to that build/install, not another SDR++ installation.

## Checks

```sh
python tools/test.py
```

This builds/runs C protocol and format tests, native frame/retune/waterfall tests,
and Python bridge/radio/installer tests. It also builds `build/tests/retune_probe`
for hardware validation. No radio is accessed by the offline tests.

All scripts accept `--build`; build scripts accept `--jobs`. Outputs, serial logs
and generated binding files belong under ignored `build/`. Keep those files
private when sharing test results: they can include device identifiers.

## Optional GitHub Actions

`ci/github-actions.yml` is an inactive workflow template for Ubuntu offline tests
and Pico/ARC builds. To enable it, commit it as `.github/workflows/checks.yml`
using a GitHub credential permitted to write workflows. No hardware is used by
those jobs. The initial publication credential lacked workflow scope, so hosted
CI was not run; the local build and hardware results above were verified.
