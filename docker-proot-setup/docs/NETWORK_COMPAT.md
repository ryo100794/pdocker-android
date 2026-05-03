# pdocker network visibility and port rewrite plan

Snapshot date: 2026-05-01.

pdockerd still executes containers in Android's app/user-space networking
context. It does not yet create a kernel network namespace or a real bridge
interface. The current work makes container network identity visible through
Docker-compatible API fields and records the mapping needed by the future
syscall-hook port rewriter.

## Docker-compatible surface

For each created container, pdockerd now stores:

- `NetworkSettings.IPAddress`
- `NetworkSettings.Networks.bridge.IPAddress`
- `NetworkSettings.Ports`
- `/containers/json[].Ports`

The IP address is a stable synthetic identity in `10.88.0.0/16`, derived from
the container ID. It is intended for UI/API identity and for future hook lookup;
it is not currently assigned by Android's kernel.

Example inspect shape:

```json
{
  "NetworkSettings": {
    "IPAddress": "10.88.12.34",
    "Ports": {
      "80/tcp": [{"HostIp": "127.0.0.1", "HostPort": "18080"}],
      "443/tcp": null
    },
    "Networks": {
      "bridge": {"IPAddress": "10.88.12.34"}
    }
  }
}
```

## pdocker extension surface

`/containers/{id}/json` also includes `PdockerNetwork`:

```json
{
  "PdockerNetwork": {
    "IPAddress": "10.88.12.34",
    "Ports": {"80/tcp": [{"HostIp": "127.0.0.1", "HostPort": "18080"}]},
    "PortRewrite": [
      {
        "ContainerPort": 80,
        "Protocol": "tcp",
        "HostIp": "127.0.0.1",
        "HostPort": 18080,
        "Hook": "bind",
        "Status": "planned"
      }
    ],
    "Kind": "host-network-with-syscall-hook-plan",
    "Warnings": [
      "pdocker records requested port publishing, but Android sandbox runtime is still host-network-only; bind/connect syscall rewrite is planned and not active yet."
    ]
  },
  "PdockerWarnings": [
    "pdocker records requested port publishing, but Android sandbox runtime is still host-network-only; bind/connect syscall rewrite is planned and not active yet."
  ]
}
```

`PortRewrite` is deliberately explicit. The next runtime hook layer can use it
to rewrite container `bind(2)` / related socket calls from a synthetic
container address/port to the Android-host-visible port. For now, the entries
are marked `planned`.

## Port source rules

- `Config.ExposedPorts` and image `config.ExposedPorts` are merged.
- `HostConfig.PortBindings` is honored when present.
- `HostConfig.PublishAllPorts=true` allocates deterministic host ports.
- Invalid or empty host ports fall back to deterministic ports derived from the
  container ID and container port.
- Container create responses and inspect payloads expose warnings whenever port
  publishing is requested. That warning means Docker-compatible metadata was
  recorded, but the syscall rewrite layer has not started forwarding traffic.

This keeps `docker ps`, `docker inspect`, UI widgets, and future syscall hook
logic reading the same persisted state.

## Test coverage

`scripts/verify_all.sh` includes a reusable regression named
`pdocker network identity + port plan`. It imports `bin/pdockerd` directly and
asserts stable virtual IP generation, Docker-style `Ports`, `/containers/json`
port summaries, invalid host-port fallback behavior, and port-publishing
warnings.
