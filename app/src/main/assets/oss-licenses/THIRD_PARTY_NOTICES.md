# Third-party notices

pdocker-android bundles or depends on the following third-party components:

- go-containerregistry / crane: Apache-2.0, https://github.com/google/go-containerregistry
- xterm.js and xterm-addon-fit: MIT, https://github.com/xtermjs/xterm.js
- Chaquopy: open-source builds with license restrictions removed since 12.0.1, https://chaquo.com/chaquopy/license/
- Android Gradle Plugin, AndroidX, Material Components, Kotlin: Apache-2.0

The default APK does not bundle PRoot, proot-loader, talloc, upstream Docker
CLI, or upstream Docker Compose plugin binaries. Optional external proot
comparisons and upstream Docker CLI/Compose compatibility tools are
developer-supplied diagnostics only and are not part of the shipped app payload.

See `THIRD_PARTY_NOTICES.md` in the source repository for the maintained
license inventory and distribution notes.
