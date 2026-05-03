# Android Self-Debug Workflow

pdockerd を同じ Android 端末の **Termux + PRoot Ubuntu** から、Wi-Fi ADB
経由でビルド→インストール→ログ確認までを手元 1 台で回す手順。

```
┌─────────────────────────────────────────────┐
│                Android phone                │
│                                             │
│   ┌─────────────────────┐     ┌──────────┐  │
│   │ Termux              │     │  pdocker  │  │
│   │  └ PRoot Ubuntu     │ ADB │  APK      │  │
│   │      (build + adb)  │◄───►│ (target)  │  │
│   └─────────────────────┘     └──────────┘  │
└─────────────────────────────────────────────┘
         loop on 127.0.0.1:<wireless-port>
```

## 前提 (一度だけ)

- 端末: 開発者オプション → **ワイヤレス デバッグ** を ON
- PRoot 側: `apt install adb` (Ubuntu の native aarch64 adb — box64 wrap版
  の adb はデーモン起動時にクラッシュするので使用しない)
- 初回のみ **ペアリング**:
  1. 端末「ペア設定コードでデバイスをペア設定する」を押す
     → `IP:PORT` と 6 桁コードが出る (pair 用、60 秒で消える)
  2. `adb pair 127.0.0.1:<PORT> <CODE>` (実行は PRoot 側)
  3. Success 表示 → ペア情報が端末に永続保存される

## 毎回の接続

ワイヤレスデバッグのトップ画面「IP アドレスとポート」の PORT (pair 用とは別。
数分〜端末再起動ごとに変わる) をメモ:

```sh
adb connect 127.0.0.1:<PORT>
adb devices   # "127.0.0.1:<PORT>  device" が出れば OK
```

**同一端末なので LAN IP ではなく 127.0.0.1 で繋がる** のがポイント。
PRoot から端末本体の LAN に出るより localhost の方が速く安定。

## ビルド → インストール → 起動

```sh
cd /root/tl/pdocker-android
# versionCode を上げる (PdockerdRuntime.extractAsset は versionCode 変化で
# 再展開する。上げずに adb install -r すると旧 pdockerd が filesDir に
# 残ったまま動いてハマる)
$EDITOR app/build.gradle.kts   # versionCode = N+1, versionName = "0.x.y"

export PATH="$HOME/opt/gradle-8.7/bin:$HOME/android-sdk/cmdline-tools/latest/bin:$PATH"
export ANDROID_HOME=$HOME/android-sdk

# DNS/TLS/direct-runtime assets が jniLibs/assets に入っているため copy-native.sh が必要。
# pdockerd を触ったときは統合済み backend から asset を再ステージする:
bash scripts/copy-native.sh
gradle --no-daemon :app:assembleDebug

ADB='/usr/bin/adb -s 127.0.0.1:<PORT>'
PKG=io.github.ryo100794.pdocker
$ADB install -r app/build/outputs/apk/debug/app-debug.apk
$ADB shell am force-stop $PKG
$ADB shell am start -n $PKG/.MainActivity
# 画面で「Start pdockerd」を押す (サービスは exported=false なので
# am start-foreground-service からは起動できない)
```

## ログを読む

pdockerd は stderr を出しまくり、Chaquopy が `python.stderr` タグで logcat に流す。
```sh
$ADB logcat -d --pid=$($ADB shell pidof $PKG) \
    | grep -E 'python\.stderr|pdockerd-runtime|AndroidRuntime: E' \
    | tail -40
```

**フィルタの役割:**
| タグ | 中身 |
|---|---|
| `python.stderr` | pdockerd の log/print (HTTP アクセスログ、crane stderr、probe 結果) |
| `pdockerd-runtime` | Kotlin `PdockerdRuntime` の asset extract + symlink log |
| `AndroidRuntime: E` | Kotlin / Java 例外で死んだときのスタックトレース |

## ファイル配置を実機で確認

`adb shell run-as` は app UID で任意コマンドを直接 exec できる。
ただし **`sh -c '...'` 経由では UID が shell(2000) に戻ってしまう** 既知の罠あり:

```sh
# これは app UID — OK
$ADB shell run-as $PKG ls -la files/pdocker-runtime/docker-bin
$ADB shell run-as $PKG ls -la files/pdocker-runtime/docker-bin
# これは shell UID に落ちる — 失敗する
$ADB shell run-as $PKG sh -c 'cd files && ls'
```

回避: `sh -c` を使うときは中で `id` を挟んで UID を確認する、あるいは直接
`adb shell run-as $PKG <cmd> <args>` 形式で渡す。

## Unix socket を外から叩く

```sh
$ADB shell run-as $PKG curl -s --unix-socket files/pdocker/pdockerd.sock \
    http://d/_ping                         # -> OK
$ADB shell run-as $PKG curl -s --unix-socket files/pdocker/pdockerd.sock \
    http://d/version | jq .                # Docker Engine API
$ADB shell run-as $PKG curl -s -X POST --unix-socket files/pdocker/pdockerd.sock \
    'http://d/images/create?fromImage=ubuntu&tag=22.04'
```

## クラッシュの直し方 (テンプレ)

pdockerd が起動しない、または Start 後 /_ping が返らない:
1. `adb logcat -d --pid=$(adb shell pidof $PKG) | grep -E 'python\.stderr|AndroidRuntime'`
2. `AndroidRuntime: E ... FATAL EXCEPTION` → Kotlin/Java 例外。スタックを読む
3. `python.stderr Traceback` → Python 例外。pdockerd.py のどの行か特定
4. `hardlink_probe: link_ok=False (EACCES ...)` → Android の SELinux が拒否。
   該当 ENV (`PDOCKER_LINK_MODE=symlink` など) を bridge で立てて回避

## versionCode を忘れるとどうなるか

`PdockerdRuntime.prepare()` は `.apk-version` ファイルの versionCode と現在の
versionCode を比較して、変わってないときはアセット展開を **skip** する。
つまり:

- versionCode 据え置きで `adb install -r` → filesDir の pdockerd は旧版のまま
- 新 pdockerd の修正が効かない
- 「直したはずなのにログが前回と同じエラー」現象の原因 No.1

**ルール: pdockerd_bridge.py か docker-proot-setup/bin/pdockerd を触ったら
必ず versionCode++**。

## 既知の落とし穴

| 症状 | 原因 | 対処 |
|---|---|---|
| `process-exec=0` | modern flavor は direct runtime を metadata/edit/browse モードに制限している | 実行検証は compat flavor で行う |
| direct 実行が遅い | ptrace/seccomp の停止回数、またはレイヤ snapshot が支配的 | `scripts/android-runtime-bench.sh --trace-mode seccomp` で stops と hot syscall を確認 |
| `tls: certificate signed by unknown authority` | Go の Linux 標準 CA パスが Android に無い | `SSL_CERT_DIR=/system/etc/security/cacerts` |
| `Permission denied: /tmp/pdblob_...` | /tmp 書き込み不可 | `PDOCKER_TMP_DIR=<filesDir>/pdocker-runtime/tmp` |
| `tar: can't link ...: Permission denied` | SELinux が link() 拒否 | `PDOCKER_LINK_MODE=symlink` (自動 probe 済み) |
| アプリ起動直後クラッシュ `Theme.AppCompat` | Manifest の `android:theme=` 未指定 | `Theme.AppCompat.DayNight.NoActionBar` 等を application タグに |
| `adb connect` が connection refused | ワイヤレスデバッグの PORT が変わった | 端末画面で現在の PORT を再確認して connect しなおす |
