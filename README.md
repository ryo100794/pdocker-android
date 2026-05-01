# pdocker-android

Standalone Android APK wrapping `docker-proot-setup` — no Termux required.

- **Daemon**: `pdockerd` (Python, from the submodule) running inside a
  Chaquopy-hosted ForegroundService. Unix socket at
  `filesDir/pdocker/pdockerd.sock`.
- **Console**: WebView + xterm.js (CJK IME compatible) backed by a
  pty child via JNI (`app/src/main/cpp/pty.c`).
- **CoW**: current default runtime uses bundled glibc `libcow.so` inside
  containers; experimental `PDOCKER_USE_COW_BIND=1` uses the self-built
  proot `--cow-bind` backend for write-open copy-up.

Plan document: [`docker-proot-setup/APK_PLAN.md`](docker-proot-setup/APK_PLAN.md)

## Build (from Termux+PRoot Ubuntu aarch64)

```sh
git clone <this repo>
cd pdocker-android
bash scripts/setup-env.sh         # JDK / cmdline-tools / NDK (first run only)
bash scripts/build-apk.sh         # -> app/build/outputs/apk/debug/app-debug.apk
```

## Install (ADB over Wi-Fi to the same phone)

```sh
adb connect 127.0.0.1:5555        # pair once via Android's Wireless debugging
adb install app/build/outputs/apk/debug/app-debug.apk
```
