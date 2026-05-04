# Android media bridge

Snapshot date: 2026-05-04.

## Goal

Phase 1 defines the container-visible audio/video contract without pretending
that capture or playback works yet. Containers get a Linux-like pdocker contract
through env vars, a mounted `/run/pdocker-media` directory, and a Unix-socket
command path. They do not get raw `/dev/video*`, `/dev/snd/*`, Android vendor
nodes, or direct Android framework libraries.

## Boundary

```text
glibc container process
  -> /run/pdocker-media/pdocker-media.sock + env contract
  -> pdocker Android media executor boundary
  -> Android public APIs
```

The Android side must use public APIs first:

- video: Camera2, with explicit front, rear, and external camera targets;
- audio capture: AudioRecord for the device microphone and selected input
  devices;
- audio playback: AudioTrack for the device speaker and selected output
  devices;
- audio routing/inventory: AudioManager and AudioDeviceInfo, including USB
  multichannel inputs/outputs when Android reports them.

## Current Phase 1 Scaffold

`PdockerdService` writes
`files/pdocker-runtime/media/pdocker-media-capabilities.json` before attempting
to start any executor. The descriptor records Camera2 and AudioManager device
inventory, runtime permission state, and the public API targets. It is
diagnostic truth, not a capture stream.

`pdockerd_bridge.py` exports:

- `PDOCKER_MEDIA_COMMAND_API=pdocker-media-command-v1`
- `PDOCKER_MEDIA_CONTRACT=linux-like-socket-env-v1`
- `PDOCKER_MEDIA_QUEUE_SOCKET=/run/pdocker-media/pdocker-media.sock`
- `PDOCKER_MEDIA_DESCRIPTOR_PATH=/run/pdocker-media/pdocker-media-capabilities.json`
- `PDOCKER_MEDIA_DEVICE_PASSTHROUGH=0`
- `PDOCKER_MEDIA_CAPTURE_READY=0`
- `PDOCKER_MEDIA_CAMERA_READY=0`
- `PDOCKER_MEDIA_AUDIO_READY=0`

`pdockerd` injects those env vars only when a container requests media through
`HostConfig.Runtime`, `HostConfig.DeviceRequests`, or `pdocker.media` labels.
Generic requests expand to specific target modes such as `video.camera2`,
`camera.front`, `camera.rear`, `audio.capture`, `audio.playback`, and
`audio.usb.multichannel`.

## Readiness Rules

Readiness flags must stay false until an APK-owned executor implements real
Camera2 or AudioRecord/AudioTrack commands and reports success. A present
socket, descriptor file, permission, or enumerated device is not capture
readiness by itself.

Future executor milestones:

1. Serve `pdocker-media-command-v1` on the Unix socket.
2. Implement explicit open/configure/start/stop commands for Camera2 streams.
3. Implement explicit AudioRecord capture and AudioTrack playback commands with
   AudioManager device selection.
4. Add USB multichannel format negotiation and underrun/overrun diagnostics.
5. Flip readiness flags only after command probes pass on the real Android API
   path.
