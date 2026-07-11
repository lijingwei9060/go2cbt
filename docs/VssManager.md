# VssManager 模块文档

## 概述

`VssManager` 负责创建 Windows VSS（Volume Shadow Copy Service）快照集，保证同一磁盘上所有文件系统分区在同一个时间点被冻结，从而实现应用级一致性备份。

**命名空间：** `VssSnapshot`  
**文件：** `client/VssManager.h`、`client/VssManager.cpp`

---

## 核心概念

### VSS 快照流程

```
                应用 (SQL Server / Exchange / ...)
                         │
              ┌──────────▼──────────┐
              │   VSS Writers       │  ← Prepare: flush cache, pause transactions
              └──────────┬──────────┘
                         │
              ┌──────────▼──────────┐
              │   VSS Provider      │  ← 创建 COW (Copy-on-Write) 快照
              │   (Microsoft SW)    │
              └──────────┬──────────┘
                         │
              ┌──────────▼──────────┐
              │   Snapshot Volume   │  ← \\?\GLOBALROOT\Device\HarddiskVolumeShadowCopyN
              │   (只读，静态)      │
              └─────────────────────┘
```

### 为什么需要 VSS

| 备份方式 | 问题 |
|----------|------|
| 直接从卷读取 | Writer 正在写入 → 数据不完整 |
| 锁定卷后读取 | 应用不可用，无法接受 |
| **VSS 快照** | Writer 协调 → 一致性时间点 → 从快照副本读取 |

---

## API

```cpp
VssSnapshot::VssManager vss;
```

### 生命周期

```
┌─────────────────────────────────────────────────────┐
│ 1. vss.Initialize()          初始化 COM + VSS       │
│ 2. vss.AddVolumeToSnapshotSet(...) × N 次           │
│ 3. vss.SetBackupState()      通知 Writer            │
│ 4. vss.DoSnapshotSet()       创建快照               │
│ 5. vss.GetSnapshotDevicePath(...) × N 次            │
│ 6. [从快照设备读取数据]                             │
│ 7. vss.Cleanup()             通知完成 + 删除快照    │
└─────────────────────────────────────────────────────┘
```

### 方法说明

```cpp
// 初始化 VSS：创建 IVssBackupComponents，收集 Writer 元数据
// 列出所有 Writer 名称到日志（SQL Writer, System Writer 等）
bool Initialize();

// 将文件系统卷添加到快照集
// volumeGuid: \\?\Volume{GUID}\  格式（来自 VolumeMapper）
// snapshotSetId: [输出] 快照集 ID
bool AddVolumeToSnapshotSet(const std::wstring& volumeGuid, VSS_ID& snapshotSetId);

// 设置备份状态为 VSS_BT_FULL（全量备份）
bool SetBackupState();

// 执行快照：
// 1. PrepareForBackup → 等待所有 Writer flush
// 2. DoSnapshotSet → 同一时刻冻结所有卷
// 3. GetSnapshotProperties → 获取每个卷的快照设备路径
bool DoSnapshotSet();

// 获取快照设备的可读路径
// 返回: \\?\GLOBALROOT\Device\HarddiskVolumeShadowCopyN
bool GetSnapshotDevicePath(const std::wstring& volumeGuid, std::wstring& snapshotPath);

// 清理：
// 1. BackupComplete → 通知 Writer 备份结束
// 2. DeleteSnapshots → 删除快照
// 3. 释放 IVssBackupComponents
// 4. CoUninitialize
void Cleanup();

// 静态方法：检测 VSS 服务是否可用
static bool IsVssAvailable();
```

### 状态查询

```cpp
bool IsInitialized() const;           // 是否初始化成功
std::wstring GetProviderName() const; // Provider 名称
std::wstring GetLastError() const;    // 最后一次错误信息
```

---

## 数据结构

```cpp
struct SnapshotVolumeInfo {
    std::wstring OriginalVolumeGuid;   // 原始卷 GUID
    std::wstring SnapshotDevicePath;   // 快照设备路径
    VSS_ID SnapshotId;                 // 快照 ID
    VSS_ID SnapshotSetId;              // 快照集 ID
};
```

内部维护 `map<OriginalVolumeGuid → SnapshotVolumeInfo>` 映射，供 `GetSnapshotDevicePath` 查询。

---

## Writer 交互

### 参与备份的典型 Writer

| Writer | 角色 |
|--------|------|
| System Writer | 系统文件、注册表 |
| SQL Server Writer | SQL 数据库一致性 |
| Exchange Writer | Exchange 邮箱数据库 |
| Hyper-V Writer | 虚拟机一致性 |
| Registry Writer | 注册表文件 |
| COM+ REGDB Writer | COM+ 注册数据库 |

### 超时与错误处理

- `PrepareForBackup` 超时：Writer 无响应时跳过（`LOG_WARNING`），不阻塞
- `DoSnapshotSet` 失败：终止备份流程，返回 `false`
- `GetSnapshotProperties` 失败：跳过该卷（`LOG_WARNING`），继续其他卷

---

## 使用示例

```cpp
#include "VolumeMapper.h"
#include "VssManager.h"

// 已有 DiskLayout 和 VolumeMapper 输出
VssSnapshot::VssManager vss;

if (vss.Initialize()) {
    // 将文件系统分区加入快照集
    for (const auto& mp : mapper.GetMappedPartitions()) {
        if (mp.Partition.Content == Disk::PartitionContent::FilesystemNTFS) {
            VSS_ID setId;
            vss.AddVolumeToSnapshotSet(mp.VolumeGuid, setId);
        }
    }

    vss.SetBackupState();

    if (vss.DoSnapshotSet()) {
        // 从快照读取每个卷
        for (const auto& mp : mapper.GetMappedPartitions()) {
            std::wstring snapshotPath;
            if (vss.GetSnapshotDevicePath(mp.VolumeGuid, snapshotPath)) {
                // 打开 snapshotPath 读取数据
                HANDLE h = CreateFileW(snapshotPath.c_str(), GENERIC_READ, ...);
                // ... 读取备份数据 ...
                CloseHandle(h);
            }
        }
    }

    vss.Cleanup();
}
```

---

## 限制

| 项目 | 说明 |
|------|------|
| 权限 | 需要管理员权限 |
| VSS 服务 | 需 VSS 服务运行（`vssvc.exe`） |
| 快照持久性 | 当前为非持久快照（备份完成后删除） |
| 快照 Provider | 使用系统默认（Microsoft Software Shadow Copy provider） |
| 多磁盘 | 每个磁盘需要独立的 VssManager 实例 |
