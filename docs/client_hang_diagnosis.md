# Client 卡死诊断报告

## 症状

- 备份进程执行后"卡死"，无后续输出
- System 进程 I/O 飙高
- 日志停在 `[VolumeMapper] Volume C:` 后再无任何输出

---

## 根因分析：三层问题叠加

### 问题 1（直接原因）：日志静默 — wofstream 写 Unicode 箭头失败

**位置：** `VolumeMapper.cpp:319` + `Logger.cpp:49-53,121-126`

日志停在 `Volume C:` 不是因为代码卡在这里，而是 **Logger 的文件流被一个 Unicode 字符搞坏了**。

```cpp
// VolumeMapper.cpp:319 — 完整消息应该是:
// "[VolumeMapper] Volume C: → Disk0 Offset=0x... Size=..."
swprintf_s(msg, L"[VolumeMapper] Volume %s → Disk%d Offset=0x%llx Size=%llu", ...)
```

`→` (U+2192, RIGHTWARDS ARROW) 是整个日志输出中 **第一个非 ASCII 字符**。在此之前所有日志消息都是纯 ASCII。

```cpp
// Logger.cpp:49-53 — wofstream 打开时没有设置任何 locale/codecvt
m_file.open(logFile, std::ios::out | std::ios::app);

// Logger.cpp:121-126 — 写入时不 flush
m_file << text << L"\n";
```

`std::wofstream` 默认使用 C locale 的 `codecvt<wchar_t, char, mbstate_t>`。C locale 只能处理单字节字符，遇到 `→` (U+2192) 时 codecvt 转换返回 `error`，流进入 `failbit` 状态。**此后所有 `m_file << text` 调用静默失败**——进程继续运行，但日志文件再也不会写入任何新内容。

**验证方法：** 检查 `backup.log` 文件，确认最后一行是否恰好截断在 `→` 字符位置（"Volume C:" 之后）。

**影响：** 进程实际上还在跑（VSS、读盘、传输），但用户看不到任何输出，误以为"卡死"。

---

### 问题 2（卡死主因）：VSS 快照创建无超时等待

**位置：** `VssManager.cpp:896`

```cpp
// VssManager.cpp:896 — Wait() 没有超时参数，无限等待
HRESULT hrWait = pAsync->Wait();
```

`VolumeMapper::Map()` 返回后，代码进入 VSS 流程：

```
VssManager::Initialize()
  → GatherWriterMetadata()  → pAsync->Wait()  // 无超时 (line 150)
  → 枚举所有 VSS Writer

BackupEngine 逐个添加文件系统卷到快照集
  → BitLocker 加密分区被标记为 FilesystemNTFS (DiskParser.cpp:234)
  → 被添加到 VSS 快照集 (BackupEngine.cpp:247-249)

VssManager::DoSnapshotSet()
  → PrepareForBackup()  → pAsync->Wait()  // 无超时 (line 481)
  → DoSnapshotSet()     → pAsync->Wait()  // 无超时 (line 541)
```

磁盘上有两个 BitLocker 加密分区（offset 0xd900000 和 0x190d900000）。VSS 创建快照时需要协调所有 Writer，包括 BitLocker 的 fvevol 驱动。在加密卷上：

1. VSS 冻结 I/O 时需要通知 fvevol 暂停加密操作
2. 快照的 copy-on-write 机制与 BitLocker 的加密层交互
3. 如果 Writer 响应慢或协调失败，`pAsync->Wait()` 会**无限期阻塞**

这直接导致 System 进程 I/O 飙高（VSS 服务运行在 System 进程中），进程看起来"卡死"。

---

### 问题 3（I/O 飙高 + 极慢）：300GB 全盘双重裸读

**位置：** `BackupEngine.cpp:437-441,931-935` + `BlockHasher.cpp:143-191`

即使 VSS 快照成功创建，后续操作也会导致极高的 I/O：

#### 第一遍：BuildManifest（哈希清单构建）

```cpp
// BackupEngine.cpp:381 — 读取整个磁盘 300GB
uint64_t totalSize = layout.Disk.Size;  // 322122547200 bytes
uint64_t totalBlocks = (totalSize + BlockHash::BLOCK_SIZE - 1) / BlockHash::BLOCK_SIZE;
// = 307,200 块

// BackupEngine.cpp:437-441 — 打开数据源，flags=0，无任何 I/O 优化
HANDLE hReadSource = CreateFileW(dataSource.c_str(),
    GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
    nullptr, OPEN_EXISTING, 0, nullptr);  // ← flags=0!
```

```cpp
// BlockHasher.cpp:143-191 — 307,200 次循环
for (uint64_t i = 0; i < manifest.TotalBlocks; i++)
{
    ReadAt(hSource, currentOffset, m_buffer.data(), blockSize);  // 同步阻塞读 1MB
    ComputeBlockHash(m_buffer.data(), blockSize, entry.Hash);     // SHA-256

    // 每块都输出 DEBUG 日志！
    LOG_DEBUG(dbg);  // 307,200 次！但日志已经坏了（问题1）
}
```

#### 第二遍：TransferBlocks（数据传输）

```cpp
// BackupEngine.cpp:931-935 — 再次打开数据源，flags=0
HANDLE hSource = CreateFileW(dataSourcePath.c_str(),
    GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
    nullptr, OPEN_EXISTING, 0, nullptr);  // ← 又是 flags=0!

// BackupEngine.cpp:987 — 每块都分配新的 1MB 缓冲区！
std::vector<uint8_t> blockData(blockSize);  // 307,200 次分配/释放
```

#### I/O 问题汇总

| 问题 | 代码位置 | 影响 |
|------|----------|------|
| `CreateFileW` flags=0 | BackupEngine.cpp:437,931 | 无预读提示，缓存管理器无法优化 |
| 缺少 `FILE_FLAG_NO_BUFFERING` | 同上 | 300GB 数据冲刷掉所有系统缓存 |
| 缺少 `FILE_FLAG_SEQUENTIAL_SCAN` | 同上 | 无大块预读 |
| 同步阻塞 ReadFile | BlockHasher.cpp:426 | 无异步 I/O，无法重叠计算和读取 |
| 读取整个磁盘含空闲区域 | BackupEngine.cpp:381 | `layout.FreeRanges` 被计算但从未使用 |
| BitLocker 区域当普通分区读 | DiskParser.cpp:234 | `IsEncrypted=true` 但 `Content=FilesystemNTFS` |
| 双重读取 | BackupEngine.cpp:467,535 | 同一 300GB 磁盘读两遍 |
| 每块分配 1MB | BackupEngine.cpp:987 | 307,200 次堆分配/释放 |
| 每块输出 DEBUG 日志 | BlockHasher.cpp:172-178 | 307,200 次日志（虽然已静默） |

---

## 完整调用链

```
main.cpp:327  Logger::Initialize("backup.log", true)
main.cpp:498  engine.Run(config, targets, stats)
  ↓
BackupEngine::FullBackup()
  ├── DiskParser::Parse()                         ← OK
  ├── VolumeMapper::Map()                         ← OK, 但最后一条日志被 → 字符截断
  ├── VssManager::Initialize()                    ← GatherWriterMetadata, Wait() 无超时
  ├── VssManager::AddVolumeToSnapshotSet()        ← BitLocker 分区也被加入
  ├── VssManager::DoSnapshotSet()                 ← ★ 可能挂死在这里 (Wait 无超时)
  ├── BlockHasher::BuildManifest()                ← 读取 300GB (第一遍), flags=0
  ├── NetworkClient::Connect()                    ← 连接服务器
  └── TransferBlocks()                            ← 读取 300GB (第二遍), flags=0, 每块分配 1MB
```

---

## 修复建议（不修改原始文件，仅提供建议）

### 优先级 P0：修复日志静默

1. **Logger.cpp:49** — 打开文件时设置 UTF-8 locale：
   ```cpp
   m_file.imbue(std::locale(m_file.getloc(), new std::codecvt_utf8<wchar_t>));
   m_file.open(logFile, std::ios::out | std::ios::app);
   ```

2. **VolumeMapper.cpp:319** — 将 `→` 替换为 ASCII `->`

3. **Logger.cpp:121** — 写入后检查流状态，失败时重置：
   ```cpp
   m_file << text << L"\n";
   if (m_file.fail()) { m_file.clear(); m_file << text << L"\n"; }
   ```

### 优先级 P0：VSS 添加超时

4. **VssManager.cpp:896** — 用带超时的等待替代 `pAsync->Wait()`：
   ```cpp
   // 10 分钟超时
   HRESULT hrWait = pAsync->Wait(600000);
   if (hrWait == VSS_S_ASYNC_PENDING) {
       pAsync->Cancel();
       // 报告超时错误
   }
   ```

5. **VssManager.cpp:150** — GatherWriterMetadata 同样需要超时

### 优先级 P1：I/O 优化

6. **BackupEngine.cpp:437,931** — 添加 I/O 优化标志：
   ```cpp
   CreateFileW(..., OPEN_EXISTING,
       FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_NO_BUFFERING,  // 替代 0
       nullptr);
   ```

7. **BackupEngine.cpp:381** — 只读已分配分区，跳过 `FreeRanges` 和 `Reserved` 分区

8. **BackupEngine.cpp:467+535** — 合并 BuildManifest 和 TransferBlocks 为单次读取

9. **BackupEngine.cpp:987** — 循环外预分配缓冲区，循环内复用

### 优先级 P2：BitLocker 处理

10. **DiskParser.cpp:234** — BitLocker 分区不应标记为 `FilesystemNTFS`，应单独处理或跳过
11. **BackupEngine.cpp:247** — 添加 `IsEncrypted` 检查，跳过加密分区或使用 BitLocker API 解密

### 优先级 P2：日志频率

12. **BlockHasher.cpp:172** — DEBUG 日志从每块改为每 1000 块或更高
13. **Logger.cpp** — 考虑添加 `Flush()` 方法，在关键节点（如 VSS 开始/结束、Manifest 进度）主动刷新
