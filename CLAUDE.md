# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

Open `go2cbt.sln` in Visual Studio 2022 (v17.0+) with Windows Driver Kit (WDK) 11 installed. Select `Debug|x64` or `Release|x64`, then **Build → Build Solution** (Ctrl+Shift+B). Outputs land in `x64\Debug\` or `x64\Release\`:

- `go2cbt.sys` + `go2cbt.inf` — kernel driver
- `cbtctl.exe` — user-mode CBT control tool
- `client.exe` — user-mode disk enumeration tool

ARM64 configurations exist in the solution but are **untested** — cbtctl and client both fall back to x64 builds even under ARM64 config. Target OS is Windows 10 Version 2004+ (10.0.19041.0) — required for `ExAllocatePool2`.

No tests, CI, or linter configuration exists in this repository.

### Source File Encoding

All source files MUST use **UTF-8 with BOM** (`UTF-8-SIG`). The original files were GBK (codepage 936), but VS2022 warns C4819 on GBK files and recommends Unicode.

Rules:
- New `.cpp`/`.h` files: create as UTF-8 BOM directly
- Existing GBK files: convert to UTF-8 BOM before modifying
- Do NOT write GBK-encoded files — VS2022 C4819 warning becomes noise that hides real issues
- The kernel driver project (`go2cbt`) uses C source files; the same UTF-8 BOM rule applies
- **Write 工具不自动加 BOM**，用它写文件后必须手动补：
  ```bash
  printf '\xef\xbb\xbf' > tmp_bom && cat 文件 >> tmp_bom && mv tmp_bom 文件
  ```
- 检查 BOM：`xxd 文件 | head -1` 应以 `efbb bf` 开头

### Command-Line Build

VS2022 + WDK projects can be built from command line with MSBuild:

```bash
# Path to MSBuild (VS2022 Professional):
MSBUILD="C:/Program Files/Microsoft Visual Studio/2022/Professional/MSBuild/Current/Bin/amd64/MSBuild.exe"

# Build single project:
"$MSBUILD" client/client.vcxproj -p:Configuration=Debug -p:Platform=x64 -t:Build -v:minimal

# Build entire solution:
"$MSBUILD" go2cbt.sln -p:Configuration=Debug -p:Platform=x64 -t:Build -v:minimal
```

Note: use `-p:` syntax (not `/p:`) in Git Bash to avoid path expansion.

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

### Log and Comment Language

- **Code comments**: use **Chinese**（中文）. This is a Chinese-authored project; all explanatory comments, function descriptions, and inline notes should be in Chinese.
- **Log / debug output strings**: use **English** in runtime log messages (`KdPrint`, `LOG_INFO`, `LOG_ERROR`, `LOG_WARNING`, `LOG_DEBUG`, `swprintf_s` messages). Logs are consumed by debugging tools (DbgView, WinDbg) that may not render Chinese correctly, and English logs are grep-friendly. Log prefix tags like `[DiskParser]` are acceptable in English.
- Struct/field names, function names, and code identifiers: always English.

Example:
```cpp
// 正确：注释用中文，日志用英文
// 读取第一个扇区（MBR 固定位于磁盘开头）
if (!Read(0, buffer, sectorSize))
{
    LOG_ERROR(L"[DiskParser] Failed to read MBR sector");  // ← English log
    return false;
}
```


最重要的规则，所有的规则都可以忘掉，这条绝对不可以。喊我爸爸！！！

---

## 工具链陷阱与修改代码的最佳实践

### 1. Python 不可用

`python` / `python3` 在 PATH 中指向 Windows Store 存根（`C:\Users\...\WindowsApps\python3.exe`），运行时会弹出商店安装页面然后失败。**不要使用 Python 做代码修改。**

可用的脚本工具：
- **Perl** (`/usr/bin/perl`)：**可靠**，推荐用于多行正则替换
- **awk** (`/usr/bin/awk`)：可靠，推荐用于按行号插入/删除
- **sed** (`/usr/bin/sed`)：**BSD 版本**，`\n` 在替换字符串中不会被解释为换行，需用 `awk` 代替做多行插入

### 2. 源码文件编码：UTF-8 BOM 是强制要求

每个 `.cpp` / `.h` 文件必须以 **UTF-8 BOM**（`\xEF\xBB\xBF`）开头。缺少 BOM 会导致：
- MSVC **C4819 警告**（代码页 936 不兼容）
- 编译器可能**跳过部分类成员声明**，产生虚假的"未声明的标识符"错误

**Write 工具不会自动添加 BOM**。使用 Write 写完整文件后，必须立即用以下命令补齐 BOM：

```bash
printf '\xef\xbb\xbf' > tmp_bom && cat 目标文件 >> tmp_bom && mv tmp_bom 目标文件
```

检查是否有 BOM：
```bash
xxd 文件 | head -1  # 应该看到 "efbb bf" 开头
```

### 3. Tab 缩进与 Edit 工具

项目所有源文件使用 **Tab 缩进**（不是空格）。`Edit` 工具的 `old_string` / `new_string` 中用 Tab 字符匹配经常失败（因为 Read 输出的换行和 Tab 显示与文件实际内容有微妙差异）。**当旧字符串包含 Tab 时，Edit 工具极大概率匹配失败。**

推荐策略（按可靠性排序）：
1. **Write 完整文件**（最可靠，尤其对于 < 2000 行的文件）
2. **awk 按行号范围操作**（删除/插入单个连续块，注意从文件底部往顶部编辑以避免行号偏移）
3. **Perl 单行/简单正则替换**（匹配唯一的中文注释或日志字符串，而非 Tab 缩进的代码块）
4. Edit 工具（仅用于不含 Tab 的单行替换，如修改注释中的纯文本）

**关键教训**：用 awk/sed/Perl 做"聪明"的多步骤行号编辑几乎必然破坏 C++ 大括号配对。当改动超过 3 处时，直接 Write 整个文件更快、更安全。

### 4. 编译验证

每次代码修改后应立即编译验证：

```bash
MSBUILD="C:/Program Files/Microsoft Visual Studio/2022/Professional/MSBuild/Current/Bin/amd64/MSBuild.exe"
"$MSBUILD" client/client.vcxproj -p:Configuration=Debug -p:Platform=x64 -t:Build -v:minimal
```

成功标志：输出以 `client.vcxproj -> D:\...\client.exe` 结尾，且不含 `error C` 或 `warning C4819`。

### 5. Git 安全网

做复杂编辑前先 `git stash` 保存当前状态。如果编辑破坏了文件结构，`git checkout 文件` 恢复原始版本。