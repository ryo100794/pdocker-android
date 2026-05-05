# APK-Scoped Memory Pager

Snapshot date: 2026-05-05.

## Purpose

This document evaluates a pdocker extension that behaves like swap inside the
APK boundary. It is not Docker compatibility. It is an Android survival layer
for memory-heavy containers such as llama.cpp when the device does not allow
ADB/root swap tuning.

## Device Constraints Observed

On the SOG15 test device:

- `adb root` is not available on the production build.
- `swapon` exists, but a shell-created swapfile fails with `Operation not
  permitted`.
- `/proc/sys/vm/swappiness`, `/proc/sys/vm/page-cluster`, and `/proc/swaps`
  are not readable/writable from the shell user.
- zram is already present and heavily used.
- Kernel config exposes `CONFIG_USERFAULTFD=y`, but `/dev/userfaultfd` is
  `0600 root:root`, so it cannot be assumed usable by the APK.

Therefore the product cannot rely on adding system swap, changing zram size, or
changing VM policy. Any swap-like behavior must be scoped to processes launched
and mediated by pdocker.

The SDK28 compat APK now carries a repeatable native probe:
`pdocker-direct --pdocker-memory-pager-probe`. On 2026-05-05 it confirmed that
the ptrace fallback primitives are visible from the APK process:
`mmap(PROT_NONE)`, `mprotect`, `madvise`, child ptrace stop,
`process_vm_writev`, intentional `SIGSEGV` stop, and `PTRACE_GETSIGINFO`.
`userfaultfd` remains blocked (`EPERM` from the syscall and `EACCES` for
`/dev/userfaultfd`), so it is a future optional path rather than the default.

## Important Boundary

Normal Linux page faults are handled by the kernel and are not delivered to user
space. An APK cannot observe every ordinary major/minor page fault from another
process.

pdocker can only catch page-fault-like events that it deliberately creates:

- `userfaultfd` faults on registered ranges, if a future device permits it.
- `SIGSEGV` faults on ranges pdocker marks `PROT_NONE` or write-protected.
- ptrace stops caused by those `SIGSEGV` deliveries before the signal reaches
  the tracee.

This means the memory pager must manage explicit regions. It is not a global
replacement for kernel swap.

## Source of the SIGSEGV Pager Idea

The SIGSEGV path is not a new kernel bypass and is not copied from an external
component. It combines three established operating-system techniques:

- Guard pages: runtimes mark memory inaccessible with `mprotect(PROT_NONE)` so
  a later access produces a deterministic fault.
- User-space paging: a cooperating pager owns selected virtual ranges and fills
  pages on demand.
- Debugger signal interception: a ptrace tracer sees a signal-delivery stop
  before the tracee receives `SIGSEGV`, can inspect `siginfo.si_addr`, and can
  resume the tracee with or without delivering the signal.

The pdocker-specific reason this became plausible is that pdocker-direct
already controls traced container processes for syscall mediation. If a fault
address belongs to a pdocker-managed range, the tracer can treat that stop as a
pager event. If it does not, the original `SIGSEGV` must be delivered normally.

## Candidate Designs

### A. File-Backed Memory First

For data that is naturally file-backed, prefer real files and `mmap`:

- GGUF model weights should remain file-backed and mmap-friendly.
- Build caches, layer indexes, and large immutable artifacts should be kept on
  disk and mapped on demand.
- Use application-level chunking and streaming before inventing fault handling.

This is the safest path. It lets the kernel reclaim clean pages without a
pdocker-specific pager.

### B. Managed Anonymous Memory Pager

For large anonymous buffers that currently pressure RAM, add an opt-in
pdocker-managed pager.

Container opt-in:

- Compose label or env: `PDOCKER_MEMORY_PAGER=managed`.
- Optional limit: `PDOCKER_MEMORY_PAGER_MAX_BYTES`.
- Optional backing directory under app-private storage:
  `files/pdocker/memory/<container-id>/`.

Container injection:

- Add `libpdocker-mempager.so` through the same direct-loader preload mechanism
  already used for rootfs shims.
- The shim wraps large `mmap(MAP_ANONYMOUS)` allocations first.
- Later, selected allocator entry points may be wrapped, but stack, executable
  mappings, JIT mappings, GPU shared buffers, and small allocations stay out of
  scope.

Managed region lifecycle:

1. Reserve a virtual range with `mmap(PROT_NONE)`.
2. Create a sparse backing file per region or per container.
3. Register region metadata in a shared table visible to the direct executor.
4. On first access, fault the page intentionally.
5. Load the page from backing storage, make it accessible, and resume.
6. On memory pressure or aging, write dirty pages to backing storage and return
   them to `PROT_NONE`.

### C. Preferred Fault Catch: userfaultfd

If a device allows unprivileged userfaultfd from the APK process:

1. Register managed ranges with `UFFDIO_REGISTER`.
2. Run a pager thread in the same process or a pdocker helper process.
3. Resolve missing pages with `UFFDIO_COPY`.
4. Track dirty pages with write-protect mode where available.

This is the cleanest architecture, but it is not the current default because
the observed production device exposes `/dev/userfaultfd` as root-only.

### D. Fallback Fault Catch: ptrace SIGSEGV Pager

pdocker-direct already owns the tracee lifecycle, so it can catch intentional
faults without kernel privileges:

1. The shim reserves managed pages as `PROT_NONE`.
2. The tracee touches a managed page and receives `SIGSEGV`.
3. Because pdocker-direct is tracing the process, the tracer sees a signal stop
   before delivery.
4. The tracer reads `siginfo.si_addr` with `PTRACE_GETSIGINFO`.
5. If the address belongs to a registered managed region:
   - suppress delivery of `SIGSEGV`;
   - inject an `mprotect(page, page_size, wanted_prot)` syscall into the tracee;
   - load page bytes from the backing file;
   - write them into the tracee with `process_vm_writev` or ptrace data writes;
   - resume the original instruction.
6. If the address is not managed, deliver the original `SIGSEGV`.

The key detail is that the virtual address must already belong to a reserved
managed VMA. The pager does not discover an arbitrary fault and map unrelated
memory into it. The shim first creates the whole managed window with
`mmap(PROT_NONE)`, records its start/end, and hands normal pointers from that
window to the program. When a fault arrives, the tracer page-aligns
`siginfo.si_addr`, validates that page against the managed table, changes that
same page to accessible permissions inside the tracee, copies the saved page
bytes into that same virtual address, and resumes. For eviction, the dirty page
is written back, discarded with `madvise(MADV_DONTNEED)` where available, and
returned to `PROT_NONE` so a later access faults again.

If `userfaultfd` is available, the same virtual-address ownership rule applies:
the managed range is registered first and `UFFDIO_COPY` fills the exact faulting
page. It is cleaner than ptrace because the kernel provides the missing-page
event and page fill API directly.

Dirty tracking can use a second intentional fault:

- Pages are restored read-only after load.
- The first write faults.
- The tracer marks the page dirty, upgrades it to writable, and resumes.

Eviction can be conservative:

- Only evict pages from managed regions.
- Never evict a page while a GPU/shared-memory command owns it.
- Use an approximate clock/LRU list maintained by the shim and tracer.
- Write dirty pages to backing storage before setting `PROT_NONE`.

This is slower than kernel swap. Its value is avoiding OOM for selected large
buffers, not improving normal performance.

## Safety Rules

- Do not page executable text, loader state, thread stacks, signal stacks, or
  libc internal mappings.
- Do not page GPU shared buffers, Vulkan/OpenCL mapped memory, or command ring
  memory unless a dedicated GPU memory contract exists.
- Do not page `MAP_SHARED` mappings by default.
- Keep the feature opt-in until correctness and performance are measured.
- Store backing files in app-private storage and delete them on container
  removal.
- Treat storage exhaustion as a hard failure with clear UI diagnostics.

## Interaction With llama.cpp

The current llama GPU result shows the bridge is functional but slower than CPU
because of upload/copy overhead. The memory pager is not the first performance
fix for that path. The immediate GPU work remains persistent registered buffers
and command-ring transport.

The memory pager is useful for:

- preventing OOM during large model/container workloads;
- experimenting with larger context or batch sizes;
- keeping CPU fallback alive when zram is saturated.

It is not expected to make token generation faster by itself.

## Implementation Plan

Do not implement the managed pager path until the SDK28 compat probe gate
below has been recorded on a real device. The product may keep file-backed
memory and streaming improvements without this gate, but the SIGSEGV pager
must not become a runtime feature on hope alone.

1. Add a probe command that records userfaultfd availability, zram/swap
   visibility, `swapon` permission, and SDK28 compat syscall behavior into a
   device artifact.
2. Add a host-only `libpdocker-mempager` prototype with a synthetic managed
   region and fault counter.
3. Add an Android direct-executor experiment for ptrace `SIGSEGV` interception:
   reserve one page as `PROT_NONE`, fault it, suppress the signal, map/write the
   page, and resume.
4. Move the experiment behind `PDOCKER_MEMORY_PAGER=managed`.
5. Add a synthetic memory-pressure benchmark:
   - working set size;
   - backed bytes;
   - page faults served;
   - evictions;
   - average fault latency;
   - OOM/LMK result.
6. Only after the synthetic benchmark is reliable, allow selected container
   templates to opt in.

## SDK28 Compat Probe Gate

The first Android implementation slice must prove these behaviors inside the
compat APK process, not from a root shell:

| Gate | Required result | If blocked |
|---|---|---|
| `mmap(PROT_NONE)` reserve | Creates a page-aligned managed VMA. | No SIGSEGV pager; file-backed mmap only. |
| `mprotect(PROT_READ|PROT_WRITE)` | Makes the exact faulting page accessible. | No SIGSEGV pager. |
| `madvise(MADV_DONTNEED)` | Releases resident backing for an evicted page, or fails with a recorded errno. | Keep pages accessible after writeback; treat as memory-saving partial failure. |
| child trace attach | Parent/tracer can trace a child launched by pdocker-direct. | No ptrace pager; userfaultfd-only future path. |
| `PTRACE_GETSIGINFO` on intentional `SIGSEGV` | Returns `si_addr` for the managed page before delivery. | No ptrace pager. |
| signal suppression and resume | The tracee resumes the original instruction after the tracer handles the page. | No ptrace pager. |
| tracee syscall injection or equivalent | The tracer can change permissions for the tracee page. | Fall back to a cooperative in-process shim handler only. |
| `process_vm_writev` or ptrace data writes | Page bytes can be copied into the tracee at the same virtual address. | Use slower ptrace word writes if allowed; otherwise no ptrace pager. |
| storage backing file | Sparse backing file works under app-private storage. | Limit to in-memory accounting/profiling only. |
| latency budget | Synthetic page-in latency is recorded before any llama/container opt-in. | Keep feature experimental and disabled by default. |

The probe artifact should record target SDK flavor, device SDK, SELinux mode,
errno values, and whether the process ran under `run-as` or normal APK launch.
The compat flavor currently uses target SDK 28, but the device may still run a
new Android release, so both values must be captured.

## Open Questions

- Whether ptrace syscall injection for `mprotect` is fast enough under real
  workloads.
- Whether `process_vm_writev` can write a newly mprotected tracee page reliably
  across Android SELinux policy on all target devices.
- Whether write-protect dirty tracking causes too many stops for llama.cpp
  compute buffers.
- Whether the pager should be per-process or per-container when a container has
  multiple processes.
