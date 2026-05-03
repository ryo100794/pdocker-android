# Third-party licenses and distribution notes

Snapshot date: 2026-05-01.

This inventory covers externally sourced code and binary payloads bundled by
pdocker-android. The current default APK set is usable for distribution when
the notice asset is included and upstream license texts/notices are preserved.

| Component | Where used | License | Distribution condition | Status |
|---|---|---|---|---|
| Docker CLI | `vendor/lib/docker`, test/compatibility tool only; not packaged in the APK | Apache-2.0 | If redistributed separately, include license/notice and preserve Docker upstream notices. | OK: excluded from app payload. |
| Docker Compose plugin | `vendor/lib/docker-compose`, test/compatibility tool only; not packaged in the APK | Apache-2.0 | If redistributed separately, include license/notice and preserve Docker Compose upstream notices. | OK: excluded from app payload. |
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

- Docker CLI upstream: https://github.com/docker/cli
- Docker Compose upstream: https://github.com/docker/compose
- go-containerregistry/crane upstream: https://github.com/google/go-containerregistry
- xterm.js upstream: https://github.com/xtermjs/xterm.js
- Chaquopy license page: https://chaquo.com/chaquopy/license/
- Kotlin upstream: https://github.com/JetBrains/kotlin
- llama.cpp upstream: https://github.com/ggml-org/llama.cpp

## Compliance notes

- The APK must include `assets/oss-licenses/THIRD_PARTY_NOTICES.md`.
- The default APK staging path omits `libproot.so`, `libproot-loader.so`,
  `libtalloc.so`, `libdocker.so`, and `libdocker-compose.so`. Optional proot
  comparisons and upstream Docker CLI/Compose compatibility runs are
  command-supplied developer diagnostics only, not bundled app payloads.
- crane is permissively licensed and remains in the app payload for registry
  exchange. Docker CLI and Docker Compose notices are kept in this source
  repository notice for test-tool redistribution only.
- No external source in this inventory blocks distribution under the current
  packaging model. The top-level `LICENSE` file records the current license
  status for pdocker's own original code.
