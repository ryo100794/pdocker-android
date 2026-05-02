# pdocker compatibility audit result

Summary: PASS=55 FAIL=0 SKIP=0

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
| PASS | protocol token: PdockerWarnings |  |
| PASS | protocol token: not active yet |  |
| PASS | protocol: daemon start | /tmp/pdocker-compat-t6pkgbd_/pdockerd.sock |
| PASS | protocol: GET /_ping | status=200, headers={'server': 'pdockerd/0.1 Python/3.13.3', 'date': 'Sat, 02 May 2026 02:08:56 GMT', 'api-version': '1.43', 'ostype': 'linux', 'docker-experimental': 'false', 'content-type': 'text/plain', 'content-length': '2'}, body=b'OK' |
| PASS | protocol: GET /version | status=200, headers={'server': 'pdockerd/0.1', 'date': 'Sat, 02 May 2026 02:08:56 GMT', 'content-type': 'application/json', 'api-version': '1.43', 'docker-experimental': 'false', 'ostype': 'linux', 'content-length': '581'}, body=b'{"Platform": {"Name": "pdockerd"}, "Components": [{"Name": "Engine", "Version": "0.1.0", "Details": {"ApiVersion": "1.43' |
| PASS | protocol: GET /info | status=200, headers={'server': 'pdockerd/0.1', 'date': 'Sat, 02 May 2026 02:08:56 GMT', 'content-type': 'application/json', 'api-version': '1.43', 'docker-experimental': 'false', 'ostype': 'linux', 'content-length': '1759'}, body=b'{"ID": "PDOCKER:PROOT", "Containers": 0, "ContainersRunning": 0, "ContainersPaused": 0, "ContainersStopped": 0, "Images"' |
| PASS | protocol: GET /containers/json?all=1 | status=200, headers={'server': 'pdockerd/0.1', 'date': 'Sat, 02 May 2026 02:08:56 GMT', 'content-type': 'application/json', 'api-version': '1.43', 'docker-experimental': 'false', 'ostype': 'linux', 'content-length': '2'}, body=b'[]' |
| PASS | protocol: GET /images/json | status=200, headers={'server': 'pdockerd/0.1', 'date': 'Sat, 02 May 2026 02:08:56 GMT', 'content-type': 'application/json', 'api-version': '1.43', 'docker-experimental': 'false', 'ostype': 'linux', 'content-length': '2'}, body=b'[]' |
| PASS | protocol: GET /volumes | status=200, headers={'server': 'pdockerd/0.1', 'date': 'Sat, 02 May 2026 02:08:56 GMT', 'content-type': 'application/json', 'api-version': '1.43', 'docker-experimental': 'false', 'ostype': 'linux', 'content-length': '31'}, body=b'{"Volumes": [], "Warnings": []}' |
| PASS | protocol: GET /networks | status=200, headers={'server': 'pdockerd/0.1', 'date': 'Sat, 02 May 2026 02:08:56 GMT', 'content-type': 'application/json', 'api-version': '1.43', 'docker-experimental': 'false', 'ostype': 'linux', 'content-length': '199'}, body=b'[{"Name": "bridge", "Id": "bridge", "Created": "2026-01-01T00:00:00Z", "Scope": "local", "Driver": "host", "IPAM": {"Dri' |
| PASS | protocol: GET /v1.43/version | status=200, headers={'server': 'pdockerd/0.1', 'date': 'Sat, 02 May 2026 02:08:56 GMT', 'content-type': 'application/json', 'api-version': '1.43', 'docker-experimental': 'false', 'ostype': 'linux', 'content-length': '581'}, body=b'{"Platform": {"Name": "pdockerd"}, "Components": [{"Name": "Engine", "Version": "0.1.0", "Details": {"ApiVersion": "1.43' |
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
| PASS | apk payload: assets/project-library/library.json |  |
| PASS | apk payload: assets/project-library/llama-cpp-gpu/compose.yaml |  |
| PASS | apk payload: assets/project-library/llama-cpp-gpu/scripts/pdocker-gpu-profile.sh |  |
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
| PASS | project library templates | ok: required templates listed<br>ok: dev-workspace includes code-server, Continue, Codex, and GPU request<br>ok: llama-cpp-gpu template has compose, Dockerfile, GPU profile, and server entrypoint<br> |
| PASS | native UI action wiring | ok: docker actions use persistent terminal helper<br>ok: docker terminal starts pdockerd first<br>ok: docker actions wait for daemon readiness<br>ok: docker build uses legacy builder env<br>ok: terminal exports legacy builder env<br>ok: compose up action is detached and builds first<br>ok: one-shot docker commands leave an interactive shell<br>ok: docker actions create native job cards<br>ok: docker jobs persist state to json<br>ok: docker jobs capture terminal output<br>ok: docker jobs record exit markers<br>ok: docker jobs expose retry actions<br>ok: docker jobs expose stop and log actions<br>ok: container console avoids host shell fallback<br>ok: host shell is diagnostic-only<br>ok: container cards surface network warnings<br>ok: container cards open known service ports<br>ok: terminals label their origin<br>ok: existing templates migrate service ports<br>ok: image browser accepts selected image extra<br>ok: image browser accepts selected container extra<br>ok: image rows deep-link to selected image<br>ok: generic image browser action remains available<br>ok: container rows deep-link to container files<br>ok: image/container files copy into editable projects<br>ok: editor tabs are keyed by canonical file path<br>ok: same-name editors get parent-qualified titles<br>ok: project files are surfaced as editor shortcuts<br>ok: terminal font remains 12pt<br>ok: terminal shortcut key palette is present<br>ok: terminal supports pinch zoom<br>ok: terminal touch scroll remains available<br>ok: terminal shows touch selection markers<br>ok: code editor exposes whitespace<br>ok: code editor keeps indent/outdent actions<br>ok: code editor supports search and replace<br>ok: code editor supports pinch zoom<br>ok: localized terminal title formats exist<br> |
| PASS | gpu compatibility design | cuVK/Vulkan benchmark scope recorded |
