# pdocker compatibility audit result

Summary: PASS=48 FAIL=0 SKIP=0

| status | check | detail |
|---|---|---|
| PASS | static api: ping | /_ping |
| PASS | static api: version | path == "/version" |
| PASS | static api: info | path == "/info" |
| PASS | static api: image list/create/save/load/history/inspect/delete | /images/(json\|create\|get\|load\|.+?/(history\|json)\|.+) |
| PASS | static api: container lifecycle/inspect/logs/wait/archive/exec/stats | /containers/.+?(start\|json\|logs\|wait\|archive\|exec\|stats) |
| PASS | static api: exec start/json | /exec/.+?/(start\|json) |
| PASS | static api: network list/create/connect/disconnect/inspect/delete | /networks |
| PASS | static api: volume list/create/prune/inspect/delete | /volumes |
| PASS | static api: build | path == "/build" |
| PASS | protocol token: application/vnd.docker.raw-stream |  |
| PASS | protocol token: X-Docker-Container-Path-Stat |  |
| PASS | protocol token: Api-Version |  |
| PASS | protocol token: application/x-tar |  |
| PASS | protocol: daemon start | /tmp/pdocker-compat-r9y18d19/pdockerd.sock |
| PASS | protocol: GET /_ping | status=200, headers={'server': 'pdockerd/0.1 Python/3.13.3', 'date': 'Fri, 01 May 2026 11:12:56 GMT', 'api-version': '1.43', 'ostype': 'linux', 'docker-experimental': 'false', 'content-type': 'text/plain', 'content-length': '2'}, body=b'OK' |
| PASS | protocol: GET /version | status=200, headers={'server': 'pdockerd/0.1', 'date': 'Fri, 01 May 2026 11:12:56 GMT', 'content-type': 'application/json', 'api-version': '1.43', 'docker-experimental': 'false', 'ostype': 'linux', 'content-length': '581'}, body=b'{"Platform": {"Name": "pdockerd"}, "Components": [{"Name": "Engine", "Version": "0.1.0", "Details": {"ApiVersion": "1.43' |
| PASS | protocol: GET /info | status=200, headers={'server': 'pdockerd/0.1', 'date': 'Fri, 01 May 2026 11:12:56 GMT', 'content-type': 'application/json', 'api-version': '1.43', 'docker-experimental': 'false', 'ostype': 'linux', 'content-length': '1759'}, body=b'{"ID": "PDOCKER:PROOT", "Containers": 0, "ContainersRunning": 0, "ContainersPaused": 0, "ContainersStopped": 0, "Images"' |
| PASS | protocol: GET /containers/json?all=1 | status=200, headers={'server': 'pdockerd/0.1', 'date': 'Fri, 01 May 2026 11:12:56 GMT', 'content-type': 'application/json', 'api-version': '1.43', 'docker-experimental': 'false', 'ostype': 'linux', 'content-length': '2'}, body=b'[]' |
| PASS | protocol: GET /images/json | status=200, headers={'server': 'pdockerd/0.1', 'date': 'Fri, 01 May 2026 11:12:56 GMT', 'content-type': 'application/json', 'api-version': '1.43', 'docker-experimental': 'false', 'ostype': 'linux', 'content-length': '2'}, body=b'[]' |
| PASS | protocol: GET /volumes | status=200, headers={'server': 'pdockerd/0.1', 'date': 'Fri, 01 May 2026 11:12:56 GMT', 'content-type': 'application/json', 'api-version': '1.43', 'docker-experimental': 'false', 'ostype': 'linux', 'content-length': '31'}, body=b'{"Volumes": [], "Warnings": []}' |
| PASS | protocol: GET /networks | status=200, headers={'server': 'pdockerd/0.1', 'date': 'Fri, 01 May 2026 11:12:56 GMT', 'content-type': 'application/json', 'api-version': '1.43', 'docker-experimental': 'false', 'ostype': 'linux', 'content-length': '199'}, body=b'[{"Name": "bridge", "Id": "bridge", "Created": "2026-01-01T00:00:00Z", "Scope": "local", "Driver": "host", "IPAM": {"Dri' |
| PASS | protocol: GET /v1.43/version | status=200, headers={'server': 'pdockerd/0.1', 'date': 'Fri, 01 May 2026 11:12:56 GMT', 'content-type': 'application/json', 'api-version': '1.43', 'docker-experimental': 'false', 'ostype': 'linux', 'content-length': '581'}, body=b'{"Platform": {"Name": "pdockerd"}, "Components": [{"Name": "Engine", "Version": "0.1.0", "Details": {"ApiVersion": "1.43' |
| PASS | docker CLI: version negotiation | Client:<br> Version:           29.4.0<br> API version:       1.43 (downgraded from 1.54)<br> Go version:        go1.26.1<br> Git commit:        9d7ad9f<br> Built:             Tue Apr  7 08:34:25 2026<br> OS/Arch:           linux/arm64<br> Context:           default<br><br>Server: pdockerd<br> Engine:<br>  Version:          0.1.0<br>  API version:      1.43 (minimum version 1.12)<br>  Go version:       none<br>  Git commit:       pdocker<br>  Built:            Fri Apr 17 00:00:00 2026<br>  OS/Arch:          linux/arm64<br>  Experimental:     false<br> |
| PASS | apk: debug artifact | /root/tl/pdocker-android/app/build/outputs/apk/debug/app-debug.apk |
| PASS | apk payload: lib/arm64-v8a/libproot.so |  |
| PASS | apk payload: lib/arm64-v8a/libproot-loader.so |  |
| PASS | apk payload: lib/arm64-v8a/libtalloc.so |  |
| PASS | apk payload: lib/arm64-v8a/libcrane.so |  |
| PASS | apk payload: lib/arm64-v8a/libdocker.so |  |
| PASS | apk payload: lib/arm64-v8a/libcow.so |  |
| PASS | apk payload: lib/arm64-v8a/libpdockerpty.so |  |
| PASS | apk payload: assets/pdockerd/pdockerd |  |
| PASS | apk payload: assets/xterm/xterm.js |  |
| PASS | apk payload: assets/oss-licenses/THIRD_PARTY_NOTICES.md |  |
| PASS | apk payload: proot advertises --cow-bind |  |
| PASS | license token: PROot |  |
| PASS | license token: GPL-2.0-or-later |  |
| PASS | license token: talloc |  |
| PASS | license token: LGPL-3.0-or-later |  |
| PASS | license token: Docker CLI |  |
| PASS | license token: Apache-2.0 |  |
| PASS | license token: go-containerregistry |  |
| PASS | license token: xterm.js |  |
| PASS | license token: MIT |  |
| PASS | license token: Chaquopy |  |
| PASS | license token: AndroidX |  |
| PASS | license token: Kotlin |  |
| PASS | full regression: verify_all.sh | ok docker run via pdockerd<br>  ok docker exec output OK<br>  ok docker build via pdockerd<br>  ok built image runs OK<br>  ok docker pull alpine:3.18<br>  ok alpine (musl) runs OK<br>  ok alpine CoW isolation (libcow-musl.so works)<br>  ok B3: bind mount host→container readable<br>  ok B3: bind mount container→host writable<br>  ok B4: docker cp host→container<br>  ok B4: copied file readable inside container<br>  ok B4: docker cp container→host<br>  ok B5: docker stats CPU% present<br>  ok B5: docker stats memory column present<br>  ok B2: docker network create<br>  ok B2: network alias resolves (verify-webby → 127.0.0.1)<br>  ok B1: docker history shows >=7 layers (7)<br>  ok B1: WORKDIR layer present in history<br>  ok B1: RUN layer text preserved<br>  ok C1: compose up (build + pull) succeeds<br>  ok C1: compose ps shows 2 Up containers<br>  ok C1: compose down succeeds (network removed)<br>  ok C1: compose down removed the network cleanly<br><br>=== D. overlayfs-compat regression (Phase 1-6 layer semantics) ===<br>  ok D1: alpine manifest.json + config.json present<br>  ok D1: manifest diff_ids == config diff_ids, all layer trees present (1 layers)<br>  ok D2: merged rootfs has /etc/alpine-release<br>  ok D2: rootfs file hardlink-shares inode with layer tree<br>  ok D3: build verify-d3 succeeded<br>  ok D3: +3 layers, whiteout in #2, isolation OK<br>  ok D4: docker save alpine produced tar<br>  ok D4: 6 entries, oci-layout+index+manifest+blobs OK<br>  ok D5: _hardlink_clone_tree upper-on-lower 7/7 transitions<br>  ok D6: base 1 layer(s) shared, +1 new<br>  ok D8: User+Labels+ExposedPorts+Volumes persisted (2 ports, 2 vols)<br>  ok D7: unlink+recreate does not mutate image layer<br>  ok D7: second container sees pristine /etc/hosts<br><br>=== 8. Android NDK cross-build (skip if NDK not set) ===<br>  skip NDK env not set (set NDK=/path/to/android-ndk to enable)<br><br>=== summary ===<br>passed: 63, failed: 0<br><br>libcow.c: In function ‘openat’:<br>libcow.c:334:17: warning: ‘nonnull’ argument ‘path’ compared to NULL [-Wnonnull-compare]<br>  334 \|     if (fd >= 0 && path && (path[0] == '/' \|\| dirfd == AT_FDCWD)) {<br>      \|                 ^~<br>libcow.c:330:8: warning: ‘nonnull’ argument ‘path’ compared to NULL [-Wnonnull-compare]<br>  330 \|     if (path && (path[0] == '/' \|\| dirfd == AT_FDCWD)) {<br>      \|        ^<br>libcow.c: In function ‘openat64’:<br>libcow.c:353:17: warning: ‘nonnull’ argument ‘path’ compared to NULL [-Wnonnull-compare]<br>  353 \|     if (fd >= 0 && path && (path[0] == '/' \|\| dirfd == AT_FDCWD)) {<br>      \|                 ^~<br>libcow.c:348:8: warning: ‘nonnull’ argument ‘path’ compared to NULL [-Wnonnull-compare]<br>  348 \|     if (path && (path[0] == '/' \|\| dirfd == AT_FDCWD)) {<br>      \|        ^<br>libcow.c: In function ‘fchmodat’:<br>libcow.c:443:8: warning: ‘nonnull’ argument ‘path’ compared to NULL [-Wnonnull-compare]<br>  443 \|     if (path && (path[0] == '/' \|\| dirfd == AT_FDCWD)) {<br>      \|        ^<br>libcow.c: In function ‘fchownat’:<br>libcow.c:460:8: warning: ‘nonnull’ argument ‘path’ compared to NULL [-Wnonnull-compare]<br>  460 \|     if (path && (path[0] == '/' \|\| dirfd == AT_FDCWD) &&<br>      \|        ^<br>libcow.c: In function ‘remember_open’:<br>libcow.c:119:38: warning: ‘%s’ directive output may be truncated writing up to 4093 bytes into a region of size between 2 and 4095 [-Wformat-truncation=]<br>  119 \|     snprintf(abs_, sizeof(abs_), "%s/%s", cwd, path);<br>      \|                                      ^~<br>libcow.c:119:5: note: ‘snprintf’ output between 2 and 8188 bytes into a destination of size 4096<br>  119 \|     snprintf(abs_, sizeof(abs_), "%s/%s", cwd, path);<br>      \|     ^~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~<br> |
