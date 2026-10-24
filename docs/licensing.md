# License scope

This repository uses MIT for the independent components and GPLv3 for the SDR++
portion. The root [LICENSE](../LICENSE) is the MIT text; it does not override the
exceptions below.

| Paths | License |
| --- | --- |
| `pico/` — bridge firmware and tests | MIT |
| `lr1121/` — ARC extension, installer and tests | MIT; retained upstream MIT attribution applies to the CBC construction |
| `host/` — bridge, radio/reference helpers, analysis and tests | MIT |
| `tools/` project-owned scripts, excluding `tools/third-party-licenses/` | MIT |
| `ci/`, root project metadata, `README.md`, and project-authored `docs/` material | MIT |
| Everything under `sdrpp/`, including module, driver/probes, tests and core patches | GPL-3.0-only; full text in [`sdrpp/LICENSE`](../sdrpp/LICENSE) |
| `external/` submodules and their nested dependencies | Each upstream project's existing license |
| `tools/third-party-licenses/` and other reproduced upstream notices | Their stated terms; preserved verbatim |

Project-owned source files carry SPDX identifiers matching this split. The copy
of GPLv3 at `sdrpp/lr1121_source/LICENSE` is retained so the module keeps its license
when copied into an SDR++ source tree. The `sdrpp/patches/` directory is also GPLv3;
patches to upstream SDR++ do not become MIT merely because MIT is the repository
default. Upstream copyright and license notices remain applicable.

The experimental TX profile (`pico/tx/`), ARC player and vector installer
(`lr1121/tx/`), and host playback CLI (`host/tx.py`) are included in the MIT scope.
They do not relicense the vendor firmware contained in generated application images.

The MIT grant for independent components permits their reuse outside SDR++,
subject to its notice requirements. It does not change the GPL obligations for
distributing the SDR++ integration or a combined SDR++ application. Building with
GNU binutils does not change the declared license of the ARC extension sources.

Semtech's firmware image is separately licensed. Generated custom images retain
that vendor material and are not wholly MIT. Consult the terms adjacent to the
pinned 0x0103 image before redistributing an image. The repository does not include
generated images, and the root MIT license does not grant rights over Semtech's
code or other upstream components.

The macOS app bundler copies both project license texts, this scope document,
`THIRD_PARTY_NOTICES.md`, the upstream SDR++ license and the existing library
notices into `Contents/Resources/`. Applicable source-distribution obligations
still depend on the exact components shipped. See
[third-party notices](../THIRD_PARTY_NOTICES.md) for provenance and attribution.
