# Build Documents

Snapshot date: 2026-05-03.

## Purpose

This file owns build and packaging commands. Test commands live in
[`../test/COMPATIBILITY.md`](../test/COMPATIBILITY.md), and active work items
live in [`../plan/TODO.md`](../plan/TODO.md).

## Contents

This category currently has one canonical build document: this README.

## Environment

From an aarch64 Android/Linux shell:

```sh
git clone <this repo>
cd pdocker-android
bash scripts/setup-env.sh
```

`scripts/setup-env.sh` installs or stages the expected JDK, Android command-line
tools, and NDK pieces for this workspace.

## APK Builds

Build the default configured APK:

```sh
bash scripts/build-apk.sh
```

The default build flavor is `compat` because it is the flavor that enables the
scratch `pdocker-direct` process executor for Dockerfile `RUN`, `docker run`,
`docker exec`, and `compose up` validation. The `modern` flavor is useful for
API 29+ metadata, image browsing, editing, and Engine API work, but it does not
advertise `process-exec=1`.

Build the API 29+ metadata-only flavor explicitly:

```sh
PDOCKER_ANDROID_FLAVOR=modern bash scripts/build-apk.sh
```

Build explicit debug variants:

```sh
./gradlew assembleCompatDebug
./gradlew assembleModernDebug
```

Modern debug output:

```text
app/build/outputs/apk/modern/debug/app-modern-debug.apk
```

Compat debug output:

```text
app/build/outputs/apk/compat/debug/app-compat-debug.apk
```

## Install Over Wi-Fi ADB

Pair/connect the device through Android Wireless debugging, then install the
variant that matches the target device/API route:

```sh
adb connect <host>:<port>
adb install -r app/build/outputs/apk/modern/debug/app-modern-debug.apk
```

For the SDK 28 compatibility route:

```sh
adb install -r app/build/outputs/apk/compat/debug/app-compat-debug.apk
```

## Build-Time Gates

Short gate used during regular implementation:

```sh
bash scripts/verify-fast.sh
```

Kotlin/APK compile gate:

```sh
./gradlew assembleModernDebug
./gradlew assembleCompatDebug
```

Slower backend gates are documented in
[`../test/COMPATIBILITY.md`](../test/COMPATIBILITY.md).
