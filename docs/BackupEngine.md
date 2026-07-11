# BackupEngine 备份引擎设计文档

## 一、命令行接口

```
client backup all|devno --cbt --serverip <ip> --port <port> [--dryrun] [--retry N] [--state-dir <path>]

参数:
  all | devno        备份目标: all=所有磁盘, devno=指定磁盘编号
  --cbt              使用 CBT 增量备份（无此参数则全量备份）
  --serverip <ip>    服务器 IP 地址
  --port <port>      服务器端口
  --dryrun           模拟模式（无服务器，立即返回 ACK）
  --retry N          重试次数（默认 3）
  --state-dir <path> 状态文件目录（默认 .\backup_states\）

示例:
  client backup 0 --serverip 192.168.1.100 --port 9000              # 全量备份 Disk0
  client backup all --serverip 192.168.1.100 --port 9000 --cbt      # 增量备份所有磁盘
  client backup 0 --dryrun                                           # 模拟测试 Disk0 全量
  client backup 1 --cbt --dryrun --retry 5                           # 模拟测试增量，重试5次
```

---

## 二、系统架构

```
┌─────────────────────────────────────────────────────────────────────┐
│                          BackupEngine (编排层)                       │
│                                                                     │
│  ForEach Disk:                                                      │
│    ┌──────────┐    ┌──────────┐    ┌──────────┐                    │
│    │DiskParser│───→│VolumeMapper│──→│VssManager│ (仅文件系统分区)   │
│    │(四分类)  │    │(卷→分区)  │    │(一致性快照)│                   │
│    └──────────┘    └──────────┘    └─────┬────┘                    │
│                                          │                         │
│         ┌────────────────────────────────┤                         │
│         │ 文件系统分区: VSS 快照卷读取    │ 裸分区: 物理磁盘读取     │
│         │ 元数据区域:   物理磁盘读取      │                         │
│         └────────────────────────────────┤                         │
│                    ┌�─────────────────────┘                         │
│              ┌─────┴─────┐                                          │
│              │BlockHasher │  SHA-256 哈希 + 清单                    │
│              └─────┬─────┘                                          │
│                    │                                                │
│         ┌──────────┴──────────┐                                     │
│         │ BlockStateManager   │  版本/ACK/断点状态                  │
│         └──────────┬──────────┘                                     │
│                    │                                                │
│         ┌──────────┴──────────┐                                     │
│         │  DataCompressor     │  deflate/zip 压缩                   │
│         └──────────┬──────────┘                                     │
│                    │                                                │
│         ┌──────────┴──────────┐                                     │
│         │  NetworkClient      │  TCP 发送 + ACK 接收                │
│         └─────────────────────┘                                     │
│                    │                                                │
└────────────────────┼────────────────────────────────────────────────┘
                     │  TCP
              ┌──────┴──────┐
              │   Server    │  ACK: {DevNo, BlockIndex, Hash}
              └─────────────┘
```

---

## 三、核心流程

### 3.1 全量备份流程 (FULL)

```
┌─ 1. DiskParser::Parse → DiskLayout (含四分类)
│
├─ 2. VolumeMapper::Map → MappedPartition[] (卷 GUID + 盘符)
│
├─ 3. VssManager::Initialize
│      ForEach 文件系统分区:
│        VssManager::AddVolumeToSnapshotSet(VolumeGuid)
│      VssManager::DoSnapshotSet → 快照卷设备路径
│
├─ 4. BlockHasher::BuildManifest (全量哈希)
│      对文件系统分区: 从快照卷读取
│      对裸分区:       从物理磁盘直接读取
│      对元数据区域:   从物理磁盘读取（单次读取，不按块拆分）
│
├─ 5. BlockStateManager::CreateVersion("FULL", snapshotTime, ...)
│      BlockStateManager::InitFullBlocks(manifest)
│
├─ 6. 传输循环（按 1MB 块）:
│      ┌→ 读取块数据（快照卷或物理磁盘）
│      │  压缩 (deflate)
│      │  NetworkClient::SendBlock(data, header)
│      │  等待 ACK
│      │  ├── ACK 成功 → BlockStateManager::SetBlockAck(idx, Ack'd)
│      │  ├── ACK 超时 → 重试 (默认 3 次)
│      │  └── ACK 失败 → BlockStateManager::SetBlockAck(idx, Failed)
│      └── 继续下一块
│      每 100 块: Save() 状态文件（支持断点续传）
│
├─ 7. VssManager::Cleanup (删除快照)
│
└─ 8. BlockStateManager::Save (最终保存)
```

### 3.2 增量备份流程 (INCREMENTAL --cbt)

```
┌─ 1-3 同全量（解析磁盘 + 创建 VSS 快照）
│
├─ 4. CbtClient::Connect → 打开 \\.\CbtMonitor
│      CbtClient::Query(devNo) → CBT 位图
│      CBT 位图解析: 哪些 1MB 块被标记为 changed
│      → changedBlockIndexes[]
│
├─ 5. BlockHasher::ComputeBlockHashes(快照卷, changedIndexes[], hashes[])
│      只对 CBT 标记的块计算哈希
│
├─ 6. 与上次 HashManifest 对比:
│      哈希相同 → CBT 误报 → 跳过
│      哈希不同 → 真正变化 → 加入传输列表
│
├─ 7. BlockStateManager::CreateVersion("INCREMENTAL", snapshotTime, ...)
│      BlockStateManager::UpdateIncrementalBlocks( changedIndexes, hashes)
│
├─ 8. 传输循环（仅传输真正变化的块）
│      流程同全量备份 Step 6
│
├─ 9. CbtClient::Reset(devNo) → 清零 CBT 位图（开始新跟踪周期）
│
├─ 10. VssManager::Cleanup
│
└─ 11. BlockStateManager::Save
```

---

## 四、新增模块设计

### 4.1 CbtClient — CBT 驱动客户端

**命名空间：** `CbtDriver`  
**文件：** `client/CbtClient.h`、`client/CbtClient.cpp`

```
职责:
  - 打开 \\.\CbtMonitor 设备
  - 启动 go2cbt 服务 (sc start go2cbt)
  - Query: 两步协议读取 CBT 位图
  - Reset: 清零 CBT 位图

API:
  bool Connect()           // 打开设备并验证可用
  bool Query(devNo, bitmap) // 两步查询，返回位图数据
  bool Reset(devNo)         // 清零位图
  void Disconnect()         // 关闭设备

内部:
  - IOCTL_CBT_QUERY (0x802) + IOCTL_CBT_RESET (0x803)
  - 结构体定义与 go2cbt.h 保持一致（#pragma pack(push, 8)）
  - 两步查询: 小缓冲区获取 TotalBits → 分配精确大小 → 再查
```

### 4.2 DataCompressor — 数据压缩

**命名空间：** `DataCompress`  
**文件：** `client/DataCompressor.h`、`client/DataCompressor.cpp`

```
职责:
  - 使用 Windows Compression API 进行 deflate 压缩
  - 压缩 1MB 数据块

API (静态方法):
  bool Compress(input, inputSize, output, outputSize)
  bool Decompress(input, inputSize, output, outputSize)
  size_t GetMaxCompressedSize(inputSize)  // 压缩后最大可能大小
```

### 4.3 NetworkClient — 网络传输

**命名空间：** `Network`  
**文件：** `client/NetworkClient.h`、`client/NetworkClient.cpp`

```
职责:
  - TCP 连接管理
  - 数据块发送 / ACK 接收
  - 超时重试
  - 断线重连

API:
  bool Connect(serverIp, port)
  bool SendBlock(header, data, dataSize) → WaitForAck() → ACK
  void Disconnect()
```

---

## 五、TCP 网络协议

### 5.1 消息类型

| Type | 值 | 方向 | 说明 |
|------|-----|------|------|
| DATA_BLOCK | 0x01 | C→S | 数据块 |
| ACK | 0x02 | S→C | 确认应答 |
| HELLO | 0x03 | C→S | 握手（含版本清单） |
| BYE | 0x04 | C→S | 结束通知 |
| ERROR | 0x05 | S→C | 服务端错误 |

### 5.2 通用消息头 (16 bytes)

```
Offset  Size  Field
0       4     Magic     0x474F3243 ("C2OG" LE)
4       4     Type      消息类型
8       4     DevNo     磁盘编号
12      4     Reserved  保留
```

### 5.3 DATA_BLOCK 消息 (Client → Server)

```
[通用消息头 16B]
[Block Header 52B]
[Compressed Data 变长]

Block Header:
Offset  Size  Field
0       8     BlockIndex   块编号
8       4     DataSize     压缩前原始大小 (≤1MB)
12      4     CompressedSize 压缩后大小
16      32    Hash          SHA-256 哈希 (32 bytes)
48      4     VersionId    版本 ID

Total: 16 + 52 + CompressedSize bytes
```

### 5.4 ACK 消息 (Server → Client)

```
[通用消息头 16B]
[Ack Body 48B]

Ack Body:
Offset  Size  Field
0       8     BlockIndex   确认的块编号
8       4     Status       0=OK, 非0=服务端错误
12      4     DevNo        Echo 磁盘编号
16      32    Hash         Echo 哈希值 (服务端校验后回传)

Total: 16 + 48 = 64 bytes
```

### 5.5 HELLO 消息 (Client → Server)

```
[通用消息头 16B]
[Hello Body 变长]

Hello Body:
Offset  Size  Field
0       4     VersionId
4       4     DevNo
8       8     TotalBlocks
16      8     TotalSize
24      32    VersionType   UTF-16LE 定长 64B ("FULL" / "INCREMENTAL")

Server 应立即回复 ACK（Status=0），表示准备接收。
```

### 5.6 BYE 消息 (Client → Server)

```
[通用消息头 16B]

Server 应立即回复 ACK 并关闭连接。
```

---

## 六、dryrun 模拟模式

```
NetworkClient (dryrun=true):
  Connect()    → 模拟连接成功
  SendBlock()  → 立即返回模拟 ACK
                 {DevNo=echo, BlockIndex=echo, Status=0, Hash=echo}
  Disconnect() → no-op
```

---

## 七、错误处理和重试

```
每个块的传输状态机:

Pending → [发送] → Sent → [等待ACK] ─┬→ ACK OK    → Acknowledged
                         │            ├→ ACK 超时  → Sent (重试, retryCount--)
                         │            └→ ACK Error → Failed (retryCount=3 次后)
                         
超时时间: 30 秒 (可配置)
重试次数: 3  次 (--retry N 可覆盖)
```

---

## 八、文件清单

| 文件 | 模块 | 类型 |
|------|------|------|
| `client/CbtClient.h` | CBT 驱动客户端 | 新增 |
| `client/CbtClient.cpp` | | 新增 |
| `client/DataCompressor.h` | 数据压缩 | 新增 |
| `client/DataCompressor.cpp` | | 新增 |
| `client/NetworkClient.h` | 网络传输 | 新增 |
| `client/NetworkClient.cpp` | | 新增 |
| `client/BackupEngine.h` | 备份引擎编排 | 新增 |
| `client/BackupEngine.cpp` | | 新增 |
| `client/main.cpp` | CLI 入口 | 修改 |
