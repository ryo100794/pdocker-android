# Third-party notices

pdocker-android bundles or depends on the following third-party components:

- Docker CLI: Apache-2.0, https://github.com/docker/cli
- Docker Compose: Apache-2.0, https://github.com/docker/compose
- go-containerregistry / crane: Apache-2.0, https://github.com/google/go-containerregistry
- xterm.js and xterm-addon-fit: MIT, https://github.com/xtermjs/xterm.js
- Chaquopy: open-source builds with license restrictions removed since 12.0.1, https://chaquo.com/chaquopy/license/
- Android Gradle Plugin, AndroidX, Material Components, Kotlin: Apache-2.0

The default APK does not bundle PRoot, proot-loader, or talloc. Optional
external proot comparisons are developer-supplied diagnostics only and are not
part of the shipped app payload.

See `docs/THIRD_PARTY_LICENSES.md` in the source repository for the maintained
license inventory and distribution notes.
