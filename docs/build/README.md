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

Build a fixed-signature compatibility APK by keeping the signing material
outside Git and passing it through the environment:

```sh
export PDOCKER_SIGNING_STORE_FILE=$HOME/.pdocker/release.jks
export PDOCKER_SIGNING_STORE_PASSWORD=...
export PDOCKER_SIGNING_KEY_ALIAS=pdocker
export PDOCKER_SIGNING_KEY_PASSWORD=...
PDOCKER_ANDROID_FLAVOR=compat PDOCKER_ANDROID_BUILD_TYPE=release bash scripts/build-apk.sh
```

`*.jks`, `*.keystore`, `*.p12`, `*.pem`, `*.key`, `*.crt`, and local signing
property files are ignored by Git. Do not commit signing certificates or
private keys. A fixed release signature can reduce repeated install-time
security prompts compared with debug-signed APK churn, but Android/Google Play
Protect verification remains an OS/device policy and cannot be disabled by the
app.

Modern debug output:

```text
app/build/outputs/apk/modern/debug/app-modern-debug.apk
```

Compat debug output:

```text
app/build/outputs/apk/compat/debug/app-compat-debug.apk
```

Compat release output:

```text
app/build/outputs/apk/compat/release/app-compat-release.apk
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
