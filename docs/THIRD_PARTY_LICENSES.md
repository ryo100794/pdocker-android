# Third-party licenses and distribution notes

Snapshot date: 2026-05-01.

This inventory covers externally sourced code and binary payloads bundled by
pdocker-android. The current set is usable for APK distribution when the notice
asset is included, GPL/LGPL source availability is maintained, and upstream
license texts/notices are preserved.

| Component | Where used | License | Distribution condition | Status |
|---|---|---|---|---|
| PROot | `vendor/lib/proot`, `vendor/lib/proot-loader`, packaged as `libproot.so` and `libproot-loader.so`; patched by `scripts/proot-patches/proot-cow-bind.patch` | GPL-2.0-or-later | Provide corresponding source, local patch, and build script for the shipped binary. Keep GPL notice. | OK: source origin, patch, and build script are recorded in this repo. |
| talloc | `vendor/lib/libtalloc.so.2`, packaged as `libtalloc.so`; PRoot dependency | LGPL-3.0-or-later | Keep LGPL notice. Dynamic library form must remain replaceable/relinkable in practice. | OK: shipped as separate shared object; notice added. |
| Docker CLI | `vendor/lib/docker`, packaged as `libdocker.so` and exposed as `docker` at runtime | Apache-2.0 | Include license/notice; preserve Docker upstream notices. Export-control notice should remain visible to distributors. | OK with notice asset. |
| Docker Compose plugin | `vendor/lib/docker-compose`, packaged as `libdocker-compose.so` and exposed as `docker-bin/cli-plugins/docker-compose` at runtime | Apache-2.0 | Include license/notice; preserve Docker Compose upstream notices. | OK with notice asset. |
| go-containerregistry / crane | `docker-proot-setup/docker-bin/crane`, packaged as `libcrane.so` | Apache-2.0 | Include license notice. | OK with notice asset. |
| xterm.js | `app/src/main/assets/xterm/xterm.js`, `xterm.css` | MIT | Include copyright and license notice. | OK with notice asset. |
| xterm-addon-fit | `app/src/main/assets/xterm/xterm-addon-fit.js` | MIT | Include copyright and license notice. | OK with notice asset. |
| Chaquopy | Gradle plugin/runtime for Python on Android | Open-source builds; upstream states restrictions were removed from 12.0.1 onward | Use current OSS version and Maven Central-compatible distribution. | OK: project uses Chaquopy 15.0.1. |
| Android Gradle Plugin | Build plugin | Apache-2.0 | Build-time dependency; keep normal Gradle/Maven notices for redistributed build artifacts if needed. | OK. |
| AndroidX core/appcompat/webkit | App dependencies | Apache-2.0 | Include license notice when redistributed. | OK with notice asset. |
| Material Components for Android | App dependency | Apache-2.0 | Include license notice when redistributed. | OK with notice asset. |
| Kotlin Gradle plugin / stdlib | Kotlin build/runtime dependency | Apache-2.0 | Include license notice when redistributed. | OK with notice asset. |
| llama.cpp | Optional source fetched by the bundled `project-library/llama-cpp-gpu` Dockerfile during user-initiated container builds; no llama.cpp source or binary is bundled in the APK | MIT | If a user or distributor publishes a prebuilt llama.cpp image, include upstream MIT license notice for that image. | OK: template reference only, not APK-bundled code. |

## Source and license references

- PROot upstream: https://github.com/proot-me/proot
- PRoot license identifier recorded by upstream: `GPL-2.0-or-later`
- Local PRoot patch: `scripts/proot-patches/proot-cow-bind.patch`
- Local PRoot build recipe: `scripts/build-proot.sh`
- talloc upstream: https://www.samba.org/talloc/
- Docker CLI upstream: https://github.com/docker/cli
- Docker Compose upstream: https://github.com/docker/compose
- go-containerregistry/crane upstream: https://github.com/google/go-containerregistry
- xterm.js upstream: https://github.com/xtermjs/xterm.js
- Chaquopy license page: https://chaquo.com/chaquopy/license/
- Kotlin upstream: https://github.com/JetBrains/kotlin
- llama.cpp upstream: https://github.com/ggml-org/llama.cpp

## Compliance notes

- The APK must include `assets/oss-licenses/THIRD_PARTY_NOTICES.md`.
- Any public binary APK release must publish or link the exact PRoot source,
  `cow_bind` patch, and build instructions used to produce `libproot.so`.
- PRoot is now opt-in. The default APK staging path omits `libproot.so`,
  `libproot-loader.so`, and `libtalloc.so`; set `PDOCKER_WITH_PROOT=1` only for
  a legacy diagnostic build. The default no-PRoot build is not yet a complete
  container runtime.
- Because `libtalloc.so` is LGPL, keep it as a separate shared object and do
  not merge it into a non-replaceable binary.
- Docker CLI, Docker Compose, and crane are permissively licensed, but their
  notices should stay available in documentation or the app notice asset.
- No external source in this inventory blocks distribution under the current
  packaging model. The remaining release hygiene item is to add an explicit
  top-level license for pdocker's own original code if this repository is to be
  consumed by third parties.
