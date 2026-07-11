# BlockHasher 模块文档

## 概述

`BlockHasher` 负责按 1MB 块计算 SHA-256 哈希值，生成哈希清单，支持增量对比和完整性校验。它在备份流程中承担"判断数据是否真正变化"的关键角色。

**命名空间：** `BlockHash`  
**文件：** `client/BlockHasher.h`、`client/BlockHasher.cpp`  
**依赖：** Windows BCrypt API（`bcrypt.lib`）

---

## 核心概念

### 为什么需要块哈希

```
CBT 位图标记块 → 但 CBT 不能完全确定数据是否变化:

场景 1: CBT 标记 changed，数据确实变了 → 需要传输
场景 2: CBT 标记 changed，但只是元数据更新（atime/mtime）→ 数据未变，可跳过
场景 3: CBT 丢失标记（极端情况） → 哈希对比发现不一致

块哈希提供精确的"有无变化"判断，消除误报。
```

### 与 CBT 配合的增量流程

```
上次备份的 HashManifest
    +
CBT 返回的 changed block indexes
    ↓
只读取 changed blocks → 计算哈希
    ↓
与上次 Manifest 对比
    ├── 哈希相同 → CBT 误报，跳过
    └── 哈希不同 → 数据真正变化 → 写入增量备份
```

---

## 数据结构

```cpp
// 单个块的哈希记录
struct BlockHashEntry {
    uint64_t BlockIndex;              // 块序号（从 0 开始）
    uint64_t Offset;                  // 字节偏移
    uint8_t  Hash[32];                // SHA-256 哈希（32 字节）
};

// 哈希清单
struct HashManifest {
    uint64_t BlockSize;               // 固定 1MB
    uint64_t TotalBlocks;             // 总块数
    uint64_t TotalSize;               // 总数据大小（字节）
    std::vector<BlockHashEntry> Entries;  // 所有块的哈希
    uint64_t Timestamp;               // 备份时间戳（FILETIME）
    std::wstring VolumePath;          // 数据来源
};
```

### 内存估算

| 磁盘大小 | 块数 | 清单大小 |
|----------|------|----------|
| 100 GB | ~102,400 | ~4.1 MB |
| 300 GB | ~307,200 | ~12.3 MB |
| 1 TB | ~1,048,576 | ~41.9 MB |
| 4 TB | ~4,194,304 | ~167.8 MB |

每个条目 = 8（BlockIndex）+ 32（Hash）= 40 字节

---

## API

### 初始化

```cpp
BlockHash::BlockHasher hasher;
hasher.Initialize();  // 打开 BCrypt SHA-256 Provider，分配 1MB 缓冲区
```

### 全量哈希

```cpp
// 从数据源构建完整哈希清单
HashManifest manifest;
hasher.BuildManifest(
    hSource,        // 数据源句柄（快照卷或物理磁盘）
    offset,         // 起始偏移
    totalSize,      // 总大小
    L"C:",          // 来源描述
    manifest        // [输出] 哈希清单
);
```

### 增量哈希（仅变化块）

```cpp
// 只计算指定块的哈希（配合 CBT 使用）
std::vector<uint64_t> changedIndexes = {5, 12, 103, ...}; // 来自 CBT
std::vector<BlockHashEntry> changedHashes;
hasher.ComputeBlockHashes(hSource, manifest, changedIndexes, changedHashes);
```

### 清单对比

```cpp
// 比较新旧清单，找出所有变化的块
std::vector<uint64_t> changedBlocks;
BlockHasher::CompareManifests(oldManifest, currentManifest, changedBlocks);
// changedBlocks 包含所有哈希不同的块索引
```

### 序列化

```cpp
// 序列化为二进制（用于存储/传输）
std::vector<uint8_t> binary = BlockHasher::SerializeManifest(manifest);

// 从二进制恢复
HashManifest restored;
BlockHasher::DeserializeManifest(binary.data(), binary.size(), restored);
```

### 工具方法

```cpp
// 哈希 → 十六进制字符串（调试用）
// 输出: "A1B2C3D4E5F6..."
std::wstring hex = BlockHasher::HashToHex(entry.Hash);
```

---

## 二进制序列化格式

```
┌─────────────────────────────────────┐
│ Header (32 bytes)                   │
│   Magic:    4 bytes  "BHMF" (0x464D4842 LE)
│   Version:  4 bytes  (1)
│   BlockSize: 8 bytes (1048576)
│   TotalBlocks: 8 bytes
│   TotalSize: 8 bytes
├─────────────────────────────────────┤
│ Entry 0 (40 bytes)                  │
│   BlockIndex: 8 bytes               │
│   Hash:      32 bytes (SHA-256)     │
├─────────────────────────────────────┤
│ Entry 1 (40 bytes)                  │
│   ...                               │
├─────────────────────────────────────┤
│ Entry N (40 bytes)                  │
└─────────────────────────────────────┘

Total size = 32 + TotalBlocks × 40 bytes
```

---

## BCrypt 使用说明

### 算法句柄复用

```cpp
BCryptOpenAlgorithmProvider(&m_hAlg, BCRYPT_SHA256_ALGORITHM, ...);
// m_hAlg 在整个生命周期中复用

// 每次哈希操作
BCryptCreateHash(m_hAlg, &hHash, ...);
BCryptHashData(hHash, data, size, 0);
BCryptFinishHash(hHash, hashOut, 32, 0);
BCryptDestroyHash(hHash);
```

### 性能特征

| 指标 | 值 |
|------|-----|
| 算法 | SHA-256 |
| 输出长度 | 32 字节 |
| 单块耗时 | ~5ms（1MB 数据 + BCrypt 开销） |
| 吞吐量 | ~200 MB/s（单线程，CPU 依赖） |
| 内存 | 1MB 固定缓冲区 |

---

## 使用示例

### 全量备份场景

```cpp
BlockHash::BlockHasher hasher;
hasher.Initialize();

// 打开快照设备
HANDLE hSnapshot = CreateFileW(snapshotPath, GENERIC_READ, ...);

// 构建全量清单
BlockHash::HashManifest manifest;
hasher.BuildManifest(hSnapshot, 0, totalSize, L"C:", manifest);

// 保存清单到文件
auto data = BlockHasher::SerializeManifest(manifest);
WriteFile(hManifestFile, data.data(), data.size(), ...);

CloseHandle(hSnapshot);
```

### 增量备份场景

```cpp
// 加载上次清单
BlockHash::HashManifest oldManifest;
BlockHasher::DeserializeManifest(oldData.data(), oldData.size(), oldManifest);

// CBT 返回变化的块
std::vector<uint64_t> cbtChanged = {5, 12, 103};

// 只验证这些块
std::vector<BlockHash::BlockHashEntry> newHashes;
hasher.ComputeBlockHashes(hSnapshot, oldManifest, cbtChanged, newHashes);

// 找出哈希真正变化的块
for (auto& entry : newHashes) {
    if (memcmp(entry.Hash, oldManifest.Entries[entry.BlockIndex].Hash, 32) != 0) {
        // 数据真正变化，需要备份
        BackupBlock(entry.BlockIndex, entry.Offset);
    }
    // else: CBT 误报，跳过
}
```
