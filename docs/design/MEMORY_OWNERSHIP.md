# Native Memory Ownership

Snapshot date: 2026-05-09.

## Purpose

This note defines the native allocation rules used by the APK-side runtime,
GPU bridge, ICD shims, and diagnostic helpers.  The goal is to keep large
workloads debuggable without introducing leaks, allocator fragmentation, or
cross-thread ownership bugs.

## Rules

| Area | Rule |
|---|---|
| Hot path buffers | Prefer caller-owned stack buffers or fixed caller-provided storage. |
| Large diagnostics | Use stack-first buffers and grow geometrically only when evidence would otherwise be truncated. |
| Allocation caps | Every dynamic diagnostic buffer must have a hard upper bound and return an explicit error such as `-EMSGSIZE` or `-ENOMEM`. |
| Ownership | The allocating function owns the buffer unless ownership is explicitly transferred in the function contract. |
| Thread safety | Do not hide mutable buffers in global/static storage. Per-call buffers are preferred over pooled global buffers unless a pool has a documented lock/lifetime model. |
| Fragmentation | Avoid frequent allocate/free loops. Grow at most geometrically, and reuse caller-owned buffers when the call pattern is repeated. |
| Failure behavior | On allocation failure, keep the previous buffer valid, return an error, and still release any heap block owned by the function. |

## Current Pattern: ICD Executor Response

`docker-proot-setup/src/gpu/pdocker_vulkan_icd.c` reads executor responses with
a stack-first policy:

- 16 KiB stack buffer for the normal response path.
- Heap growth only for large diagnostic JSON.
- 1 MiB hard cap.
- Geometric growth to avoid allocation storms.
- Per-call state only, so concurrent dispatches do not share a mutable buffer.
- No `realloc` dependency: a new block is allocated first, then copied, then the
  old owned heap block is released.  This keeps the old buffer valid on
  `ENOMEM`.

This is intentionally not a process-global response buffer.  A global pool
could reduce allocator churn, but it would require locking, lifetime ownership,
and reentrancy rules.  Until profiling proves the response buffer is a bottleneck,
local ownership is safer.

## Review Checklist

- Does every `malloc`/`calloc`/`strdup` have one clear owner and release path?
- Is the allocation size bounded before multiplication or growth?
- Is integer overflow checked before computing byte sizes?
- Can an error path leak or double-free?
- Does the code allocate repeatedly inside a syscall/dispatch loop?
- If a buffer is shared across threads, where is the lock and who owns reset?
- Can diagnostics be disabled without changing functional behavior?

