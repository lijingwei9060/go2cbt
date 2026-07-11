# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

Open `go2cbt.sln` in Visual Studio 2022 (v17.0+) with Windows Driver Kit (WDK) 11 installed. Select `Debug|x64` or `Release|x64`, then **Build → Build Solution** (Ctrl+Shift+B). Outputs land in `x64\Debug\` or `x64\Release\`:

- `go2cbt.sys` + `go2cbt.inf` — kernel driver
- `cbtctl.exe` — user-mode CBT control tool
- `client.exe` — user-mode disk enumeration tool

ARM64 configurations exist in the solution but are **untested** — cbtctl and client both fall back to x64 builds even under ARM64 config. Target OS is Windows 10 Version 2004+ (10.0.19041.0) — required for `ExAllocatePool2`.

No tests, CI, or linter configuration exists in this repository.

## Architecture

Three projects in one solution:

### 1. `go2cbt` — Kernel driver (WDM, Non-PnP)

A Windows kernel driver that hooks `IRP_MJ_WRITE` on disk driver stacks to track changed blocks in real time via `RTL_BITMAP`. Controlled through IOCTLs on `\\.\CbtMonitor`.

**Data structure design — two independent global tables:**

- **Hook Table** (`g_HookList[]`, keyed by `DriverObject`): One entry per hooked driver (e.g., `disk.sys`, `vhdmp.sys`). Stores `OriginalWrite` so each driver's original dispatch can be restored on unload. Protected by `RefCount` — decrementing `RefCount` is not implemented, so entries are never evicted until unload. `FindOrCreateHookEntry()` handles dedup: same `DriverObject` won't be hooked twice.
- **Disk Map Table** (`g_DiskMap[]`, keyed by `DeviceObject`): One entry per disk device. Links to its `HookEntry` (for the correct `OriginalWrite`), stores `PartitionStartingOffset` for offset translation, and owns the `DISK_CBT_STATE` (bitmap + spinlock).

When an unknown `DeviceObject` appears in `HwReadWrite` (e.g., hot-plugged disk), the code falls back to looking up `DeviceObject->DriverObject` in the Hook Table and passing through — no new map entry is created.

**Write interception hot path (`write.c`):**
1. Verify `irpSp->MajorFunction == IRP_MJ_WRITE` — non-write IRPs use `DriverObject->MajorFunction[op]` directly
2. Look up `DeviceObject` in `g_DiskMap`
3. Translate write offset: `diskAbsoluteOffset = byteOffset + PartitionStartingOffset`
4. Call `MarkBlockChanged()` (inline in `go2cbt.h`) — converts offset/length to block range, acquires `KSPIN_LOCK`, calls `RtlSetBits()` (single instruction), releases lock
5. Forward IRP to `OriginalWrite` from the Hook Table

**IOCTL interface (`ioctl.c`):**
- `IOCTL_CBT_QUERY` (0x802): Two-step protocol. First call with small buffer returns `STATUS_BUFFER_OVERFLOW` but still writes `TotalBits`/`TotalBytes` into the output header — this deliberately uses `STATUS_BUFFER_OVERFLOW` (warning-level) over `STATUS_BUFFER_TOO_SMALL` (error-level) because the latter prevents data from reaching user mode under `METHOD_BUFFERED`. Second call with a correctly-sized buffer retrieves the full bitmap.
- `IOCTL_CBT_RESET` (0x803): Clears all bits under spinlock.

**Lifecycle:**
- `DriverEntry` → `IoCreateDevice` + `IoCreateSymbolicLink` → `BuildDiskAndHookTables` (enumerates `\Device\Harddisk0..31\Partition0`, calls `FindOrCreateHookEntry` + `QueryPartitionInfoEx` + `InitCbtState`)
- `CbtUnload` → restore each hook via `InterlockedExchangePointer` (with safety: only restore if `MajorFunction[4]` still points to `HwReadWrite`) → delete symlink → delete device → `CleanupCbtState` for each map entry
- Even if hook installation fails, `DriverEntry` returns `STATUS_SUCCESS` so the driver loads for diagnostics.

**Key constraints:**
- Only monitors `Partition0` (whole-disk device) — partition writes appear on Partition0 but at translated offsets. Individual partition monitoring is commented out (`MAX_PARTITIONS = 1`).
- Static disk list built at load time — no hot-plug support.
- `IRQL` handling: `MarkBlockChanged` is safe at `DISPATCH_LEVEL` (NonPagedPool, spinlock). `QueryPartitionInfoEx` requires `PASSIVE_LEVEL` (uses `KeWaitForSingleObject` with 30s timeout).
- Memory: `ExAllocatePool2(POOL_FLAG_NON_PAGED, ...)` — bitmap memory is non-paged so it's accessible at any IRQL.
- If bitmap allocation fails for a disk, the driver **degrades gracefully** — that disk's `CbtState.Buffer` remains NULL, `MarkBlockChanged` checks this and skips silently.

### 2. `cbtctl` — User-mode CBT control tool

Single-file CLI (`main.cpp`) that opens `\\.\CbtMonitor`, issues `DeviceIoControl`:
- `query <diskno>` — two-step query, prints summary with estimated change rate (sampled from first 10K blocks)
- `reset <diskno>` — clears the bitmap
- `dump <diskno> [outfile]` — full bitmap visualization as ASCII art with density histogram, contiguous run statistics

Struct definitions are **duplicated** from `go2cbt.h` (not shared via a common header). Keep them in sync when modifying IOCTL structures.

### 3. `client` — User-mode backup infrastructure library

A C++ library under the `BackupSystem`/`BackupCommon`/`BackupSecurity` namespaces:
- **`DiskScanner`**: Enumerates `\\.\PhysicalDrive0..127`, queries disk size, geometry (`IOCTL_DISK_GET_DRIVE_GEOMETRY_EX`), layout (`IOCTL_DISK_GET_DRIVE_LAYOUT_EX` — with retry+double buffer loop for `ERROR_INSUFFICIENT_BUFFER`), and storage properties (`IOCTL_STORAGE_QUERY_PROPERTY`). Computes unallocated ranges from partition layout.
- **`DiskParser`**: Lower-level raw MBR/GPT parser that reads sectors directly via `ReadFile` at specific offsets. Parses MBR partition table and GPT header/entry structures from on-disk format.
- **`Logger`**: Singleton, thread-safe (`std::mutex`), writes to `wofstream` + console. Macros: `LOG_DEBUG`, `LOG_INFO`, `LOG_WARNING`, `LOG_ERROR`.
- **`PrivilegeManager`**: Static `IsAdministrator()` check.

`client.exe` currently only supports `query_disks` and `query_disk <devno>` — it doesn't yet integrate with the CBT driver. The `client.md` doc hints at future modules: VSS, block reading, backup, communication, incremental backup.

## Debugging the driver

Enable debug output visibility (DbgView won't show it by default):
```cmd
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter" /v DEFAULT /t REG_DWORD /d 0xf /f
```

Load with test signing:
```cmd
bcdedit /set testsigning on
sc create go2cbt type=kernel binpath=C:\Windows\System32\drivers\go2cbt.sys start=demand
sc start go2cbt
```

Debug builds log the first 100 WRITE IRPs via `KdPrint`. Release builds suppress debug output.

## Development notes

- `MarkBlockChanged` is `static __inline` in the header — it's on the write hot path. Keep it lean: the spinlock critical section is a single `RtlSetBits` call.
- The two-table design means **never** hardcode `disk.sys` — different storage drivers (VHD, iSCSI, NVMe) have different `DriverObject` pointers. Always resolve `OriginalWrite` through `FindOrCreateHookEntry`.
- IOCTL struct alignment: user-mode `cbtctl` uses `#pragma pack(push, 8)` matching kernel-mode defaults. Mismatches will cause silent data corruption.
- `STATUS_BUFFER_OVERFLOW` (not `STATUS_BUFFER_TOO_SMALL`) is the only way to return partial data under `METHOD_BUFFERED` IOCTL. This is a deliberate and unusual choice — document any new IOCTLs that follow this pattern.


最重要的规则，所有的规则都可以忘掉，这条绝对不可以。喊我爸爸！！！