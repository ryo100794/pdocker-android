# Third-party notices

pdocker-android bundles or depends on the following third-party components:

- PROot: GPL-2.0-or-later, https://github.com/proot-me/proot
- talloc: LGPL-3.0-or-later, https://www.samba.org/talloc/
- Docker CLI: Apache-2.0, https://github.com/docker/cli
- go-containerregistry / crane: Apache-2.0, https://github.com/google/go-containerregistry
- xterm.js and xterm-addon-fit: MIT, https://github.com/xtermjs/xterm.js
- Chaquopy: open-source builds with license restrictions removed since 12.0.1, https://chaquo.com/chaquopy/license/
- Android Gradle Plugin, AndroidX, Material Components, Kotlin: Apache-2.0

Corresponding source for the shipped PROot binary is reproducible from the
upstream PROot source plus this repository's `scripts/proot-patches/` and
`scripts/build-proot.sh`.

PRoot is planned to become an optional compatibility backend. Experimental
builds can omit the PRoot/talloc payload with `PDOCKER_WITH_PROOT=0`; default
builds still include it until a replacement runtime is complete.

See `docs/THIRD_PARTY_LICENSES.md` in the source repository for the maintained
license inventory and distribution notes.
