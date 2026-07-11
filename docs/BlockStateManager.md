# BlockStateManager 模块文档

## 概述

`BlockStateManager` 负责管理备份数据块的状态生命周期，包括哈希记录、ACK 确认、版本追踪和持久化存储。它是备份引擎与服务器端同步、断点续传、增量备份对比的核心状态管理模块。

**命名空间：** `BlockState`  
**文件：** `client/BlockStateManager.h`、`client/BlockStateManager.cpp`

---

## 核心概念

### 数据块模型

```
磁盘 → 按 1MB 切分为 N 个块

Block[0]   Offset=0x00000000  Size=1MB  Hash=SHA256(...)
Block[1]   Offset=0x00100000  Size=1MB  Hash=SHA256(...)
Block[2]   Offset=0x00200000  Size=1MB  Hash=SHA256(...)
...
Block[N-1] Offset=...         Size≤1MB   Hash=SHA256(...)
```

每个块关联：
- **VersionId**：属于哪个备份版本
- **AckStatus**：在传输流程中的位置
- **Changed**：本版本中数据是否变化
- **Hash**：SHA-256 校验值（32 字节）

### 版本模型

```
Version 0: "FULL"       SnapshotTime=T0   ChangedBlocks=ALL     块版本号=0
Version 1: "INCREMENTAL" SnapshotTime=T1   ChangedBlocks=156     块版本号=1
Version 2: "INCREMENTAL" SnapshotTime=T2   ChangedBlocks=42      块版本号=2
...
```

每次 VSS 快照创建一个新版本，快照时间即为版本时间戳。全量备份版本号从 0 开始自增。

---

## 数据结构

### AckStatus 枚举

```
Pending ──→ Sent ──→ Acknowledged
  │                    │
  └── Failed ──────────┘ (可重试)

Skipped: 哈希未变 / 去重，无需传输
```

| 状态 | 含义 | 传输动作 |
|------|------|----------|
| `Pending` | 初始状态，待发送 | 需要上传 |
| `Sent` | 已发送给服务器 | 等待 ACK |
| `Acknowledged` | 服务器确认收到 | 完成 |
| `Failed` | 发送失败 | 可重试 |
| `Skipped` | 跳过（数据未变） | 不需要操作 |

### BlockState

```cpp
struct BlockState {
    uint64_t BlockIndex;    // 块编号（0 起始）
    uint64_t Offset;        // 字节偏移（BlockIndex × 1MB）
    uint8_t  Hash[32];      // SHA-256 哈希值
    uint64_t VersionId;     // 所属版本 ID
    AckStatus Ack;          // ACK 状态
    bool     Changed;       // 本版本是否变化

    bool NeedsUpload();     // Changed && Ack != Acknowledged
};
```

### VersionRecord

```cpp
struct VersionRecord {
    uint64_t VersionId;        // 版本 ID（自增）
    int      DevNo;            // 磁盘编号
    wstring  VersionType;      // "FULL" / "INCREMENTAL"
    FILETIME SnapshotTime;     // VSS 快照时间戳
    uint64_t TotalBlocks;      // 总块数
    uint64_t ChangedBlocks;    // 变化块数
    uint64_t TotalSize;        // 数据总大小（字节）
    uint64_t AckedBlocks;      // 已确认块数
    uint64_t CreateTimestamp;  // 创建时的本地时间

    double Progress();         // AckedBlocks / TotalBlocks × 100%
};
```

---

## API

### 生命周期

```cpp
BlockStateManager mgr;

// 初始化：指定存储目录和磁盘编号
// 自动加载已有状态，不存在则新建
mgr.Initialize(L"C:\\Backup\\states", 0);
```

### 版本管理

```cpp
// 创建新版本
FILETIME snapshotTime = ...;  // VSS 快照时间
VersionRecord ver = mgr.CreateVersion(L"FULL", snapshotTime, totalBlocks, totalSize);

// 全量备份：批量初始化所有块状态
mgr.InitFullBlocks(ver.VersionId, hashManifest);
// 此时所有块: Changed=true, Ack=Pending

// 增量备份：只更新变化块
std::vector<uint64_t> changedIndexes = {5, 12, 103, ...};  // 来自 CBT + 哈希对比
std::vector<BlockHashEntry> changedHashes;
mgr.UpdateIncrementalBlocks(ver.VersionId, changedIndexes, changedHashes);
// 变化块: Changed=true, Ack=Pending
// 未变块: Changed=false, Ack=Skipped
```

### ACK 管理

```cpp
// 单块确认
mgr.SetBlockAck(42, AckStatus::Acknowledged);

// 连续范围确认（服务器批量 ACK）
mgr.SetBlockAckRange(0, 1024, AckStatus::Sent);

// 离散列表确认
std::vector<uint64_t> ackedBlocks = {5, 12, 103};
mgr.SetBlockAckList(ackedBlocks, AckStatus::Acknowledged);
```

### 查询

```cpp
// 按状态过滤
auto pending = mgr.GetBlocksByAck(AckStatus::Pending);   // 待发送
auto failed  = mgr.GetBlocksByAck(AckStatus::Failed);     // 需要重试

// 待处理块（Changed && Ack != Acknowledged）
auto todo = mgr.GetPendingBlocks();

// 断点续传：最后一个连续 ACK 位置
uint64_t resumeFrom = mgr.GetLastAcknowledgedBlock();
// 从头扫描，返回第一个非 ACK 块的索引

// 版本历史
auto history = mgr.GetVersionHistory();   // 所有版本时间线
VersionRecord v = mgr.GetVersion(0);     // 查询特定版本

// 块历史（同一 BlockIndex 在不同版本中的状态）
auto blockHistory = mgr.GetBlockHistory(42);
```

### 持久化

```cpp
mgr.Save();   // 保存到 block_state_{devno}.dat
// 析构时自动保存（If dirty）
```

---

## 数据存储结构

### 文件格式

文件：`{stateDir}\block_state_{devno}.dat`

```
┌─────────────────────────────────────────────────────────────┐
│ Header (64 bytes)                                           │
├─────────────────────────────────────────────────────────────┤
│ Offset  Size  Field           Description                   │
│ 0       4     Magic           0x534B4C42 ("BLKS" LE)        │
│ 4       4     Version         1                             │
│ 8       4     DevNo           磁盘编号                       │
│ 12      4     RecordSize      48 (每条 BlockRecord 字节数)   │
│ 16      8     BlockSize       1048576 (1MB)                 │
│ 24      8     TotalBlocks     总块数                         │
│ 32      4     VersionCount    版本表条目数                    │
│ 36      4     CRC32           Header 前 36 字节的 CRC32      │
│ 40      24    Reserved        全零                           │
├─────────────────────────────────────────────────────────────┤
│ Block Record Array (TotalBlocks × 48 bytes)                 │
├─────────────────────────────────────────────────────────────┤
│ 每条 BlockRecord 格式 (48 bytes):                            │
│   Offset  Size  Field                                       │
│   0       4     Flags    [bit0=Changed, bit1-3=AckStatus]   │
│   4       8     BlockIndex                                   │
│   12      8     VersionId                                    │
│   20      32    Hash     SHA-256 (32 bytes)                  │
│   52      4     Reserved                                     │
├─────────────────────────────────────────────────────────────┤
│ Version Table (VersionCount 条记录)                           │
├─────────────────────────────────────────────────────────────┤
│ 每条 VersionRecord 格式 (92 bytes):                          │
│   Offset  Size  Field                                       │
│   0       8     VersionId                                    │
│   8       4     DevNo                                        │
│   12      32    VersionType   UTF-16LE 定长 32 字节          │
│   44      8     SnapshotTime  FILETIME → uint64              │
│   52      8     TotalBlocks                                  │
│   60      8     ChangedBlocks                                │
│   68      8     TotalSize                                    │
│   76      8     AckedBlocks                                  │
│   84      8     CreateTimestamp                              │
└─────────────────────────────────────────────────────────────┘

总文件大小 = 64 + TotalBlocks×48 + VersionCount×92
```

### 文件大小估算

| 磁盘 | 块数 | Header | Block Records | 10 版本 | 总大小 |
|------|------|--------|---------------|---------|--------|
| 100 GB | 102K | 64 B | 4.9 MB | 920 B | ~4.9 MB |
| 1 TB | 1M | 64 B | 50.3 MB | 920 B | ~50.3 MB |
| 4 TB | 4M | 64 B | 201.3 MB | 920 B | ~201.3 MB |

---

## 数据持久化保证

### 写入协议

```
Save()
  │
  ├── 1. 写入 .dat.tmp（FILE_FLAG_WRITE_THROUGH 绕过磁盘缓存）
  │      └── 崩溃时: .dat 完好，.tmp 残留
  │
  ├── 2. CRC32 计算 Header 前 36 字节 → 回写到 offset 36
  │
  ├── 3. FlushFileBuffers 强制刷盘
  │      └── 失败 → LOG_WARNING，继续（兼容 FAT/exFAT）
  │
  └── 4. MoveFileExW(.tmp → .dat) 原子替换
         ├── NTFS 元数据操作: 要么完成，要么不做
         └── 崩溃时: 不存在半写文件
```

### 读取协议

```
Load()
  │
  ├── 1. 读取 Header (64B)
  │
  ├── 2. CRC32 验证
  │      ├── 匹配 → 继续
  │      └── 不匹配 → 拒绝加载（首次初始化）
  │
  ├── 3. 读取 BlockRecord[N] (固定 48B，随机访问)
  │
  └── 4. 读取 VersionTable
```

---

## 使用示例

### 全量备份流程

```cpp
BlockState::BlockStateManager state;
state.Initialize(L"C:\\Backup\\states", 0);

// VSS 快照时间
FILETIME snapshotTime;
GetSystemTimeAsFileTime(&snapshotTime);

// 创建全量版本
auto ver = state.CreateVersion(L"FULL", snapshotTime,
    manifest.TotalBlocks, manifest.TotalSize);

// 批量初始化块状态
state.InitFullBlocks(ver.VersionId, manifest);

// 保存初始状态
state.Save();

// --- 传输循环 ---
auto pending = state.GetPendingBlocks();
for (auto& block : pending) {
    state.SetBlockAck(block.BlockIndex, AckStatus::Sent);
    // 发送 block 到服务器...
    state.SetBlockAck(block.BlockIndex, AckStatus::Acknowledged);
    state.Save();  // 定期保存，支持断点续传
}
```

### 增量备份流程

```cpp
// 加载已有状态
BlockState::BlockStateManager state;
state.Initialize(L"C:\\Backup\\states", 0);

// 从 CBT + 哈希对比获取变化块
std::vector<uint64_t> changedIndexes;   // CBT changed indexes
std::vector<BlockHash::BlockHashEntry> changedHashes;  // 对应的新哈希

// 创建增量版本
FILETIME newSnapshotTime;
GetSystemTimeAsFileTime(&newSnapshotTime);
auto ver = state.CreateVersion(L"INCREMENTAL", newSnapshotTime,
    state.GetTotalBlocks(), totalSize);

// 更新变化块状态（未变块自动标 Skipped）
state.UpdateIncrementalBlocks(ver.VersionId, changedIndexes, changedHashes);

// 只传输真正变化的块
auto pending = state.GetPendingBlocks();  // 自动过滤 Skipped
for (auto& block : pending) {
    // 发送...
    state.SetBlockAck(block.BlockIndex, AckStatus::Acknowledged);
}
state.Save();
```

### 断点续传

```cpp
// 程序重启后加载状态
BlockState::BlockStateManager state;
state.Initialize(L"C:\\Backup\\states", 0);

// 查找断点
uint64_t resumeFrom = state.GetLastAcknowledgedBlock();
// getLastAcknowledgedBlock 从头扫描，返回第一个非 ACK 位置的索引

// 获取所有未完成块
auto pending = state.GetPendingBlocks();

wprintf(L"Resuming from block %llu, %zu blocks remaining\n",
    resumeFrom, pending.size());

// 继续传输...
```

### 查询历史

```cpp
// 查看版本时间线
auto history = state.GetVersionHistory();
for (auto& ver : history) {
    wprintf(L"Version %llu: %s, %llu/%llu blocks, progress=%.1f%%\n",
        ver.VersionId, ver.VersionType.c_str(),
        ver.AckedBlocks, ver.ChangedBlocks, ver.Progress());
}

// 查询某个块的历史
auto blockHistory = state.GetBlockHistory(42);
for (auto& bs : blockHistory) {
    // bs.VersionId, bs.Hash, bs.Ack
}
```
