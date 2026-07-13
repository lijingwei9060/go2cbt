# cbt-server 服务端设计文档

## 一、概述

`cbt-server` 是 go2cbt 备份系统的 Linux 服务端程序。它通过 TCP 接收 Windows 客户端（`client.exe`）上传的磁盘备份数据块，验证完整性后写入本地 raw 磁盘镜像文件。

### 1.1 系统角色

```
┌────────────────────────────┐       TCP        ┌──────────────────────────────┐
│  Windows 客户端             │                  │  Linux 服务端 (cbt-server)    │
│                            │                  │                              │
│  go2cbt.sys ── CBT 位图    │                  │  epoll 事件循环              │
│  client.exe                │                  │   ├─ MessageParser           │
│   ├─ DiskParser            │   HELLO          │   ├─ BlockProcessor          │
│   ├─ VssManager            │   DATA_BLOCK × N │   │   ├─ inflate (zlib)       │
│   ├─ BlockHasher (SHA-256) │ ───────────────→ │   │   ├─ SHA-256 验证        │
│   ├─ DataCompressor        │   ←── ACK × N ── │   │   ├─ pwrite() 写入       │
│   ├─ BlockStateManager     │   BYE            │   │   └─ send ACK            │
│   └─ NetworkClient         │                  │   └─ DiskMap                 │
│                            │                  │       devno → backing file   │
└────────────────────────────┘                  └──────────────────────────────┘
```

### 1.2 关键约束

| 约束 | 说明 |
|------|------|
| 平台 | Linux（开发环境 WSL2 / Ubuntu 24.04） |
| 存储后端 | raw 格式文件（`truncate` 创建），按字节偏移 `pwrite()` 写入 |
| 状态管理 | **无持久化状态**。不依赖数据库，backing file 即数据 |
| 客户端数 | 单客户端（一次只接受一个 TCP 连接） |
| 上传模式 | **流水线并发**（客户端窗口 5-6 块），不等待 ACK 即发下一块 |
| ACK 语义 | **先写盘后 ACK**。ACK = 数据已持久化，可恢复 |
| 去重 | 不做 |
| 管理接口 | 暂不提供 |
| 配置方式 | 命令行参数 |

---

## 二、CLI 接口

```
cbt-server --listen <ip:port> --disk <devno>:<filepath> [--disk ...]

参数:
  --listen <ip:port>   监听的地址和端口，例如 0.0.0.0:9000
  --disk <devno:path>  磁盘编号到 backing file 的映射，可多次指定
                       例如: --disk 0:/data/disk0.raw --disk 1:/data/disk1.raw

示例:
  cbt-server --listen 0.0.0.0:9000 --disk 0:/backup/hdd0.raw

启动时行为:
  1. 解析命令行参数
  2. 打开所有 backing file (O_WRONLY)，验证文件存在且可写
  3. 绑定 listening socket
  4. 进入 epoll 事件循环
```

---

## 三、架构设计

### 3.1 线程模型：单线程 epoll

```
main()
  │
  ├── parse_cli()
  ├── DiskMap::Open(devno, filepath)  逐个打开 backing file
  ├── socket() + bind() + listen()   创建 listening socket
  └── EventLoop::Run()
        │
        └── while (running)
              epoll_wait()
              │
              ├── listening_fd 可读
              │     └── accept() → 创建 Connection 对象
              │
              ├── client_fd 可读
              │     └── Connection::OnReadable()
              │           │
              │           ├── recv() → 追加到 RingBuffer::recv_buf
              │           └── while (MessageParser::TryParse())
              │                 │
              │                 ├── HELLO  → HandleHello()
              │                 ├── DATA_BLOCK → HandleDataBlock()
              │                 └── BYE    → HandleBye()
              │
              └── client_fd 可写（有待发送 ACK）
                    └── Connection::OnWritable()
                          └── send() ACK 数据
```

**为什么单线程够用？**

单块处理耗时 ~3.3ms（inflate 1ms + SHA-256 2ms + pwrite 0.2ms + send 0.03ms）。单线程吞吐 ~300 块/秒 = 300 MB/s。千兆网络物理上限 ~125 MB/s，单线程有 2.4x 余量。

TCP 内核缓冲区 + 应用层 RingBuffer 自动提供流水线缓冲：客户端发第 5 个块时，前 4 个已经在内核缓冲区里排队处理了。

### 3.2 模块划分

| 模块 | 文件 | 职责 |
|------|------|------|
| CLI + 入口 | `main.cpp` | 参数解析、启动、信号处理 |
| 事件循环 | `Server.h/.cpp` | epoll 管理、accept、Connection 生命周期 |
| 协议解析 | `MessageParser.h/.cpp` | 从字节流中切出完整消息帧 |
| 块处理 | `BlockProcessor.h/.cpp` | inflate → SHA-256 校验 → pwrite → 构造 ACK |
| 环形缓冲 | `RingBuffer.h/.cpp` | 零拷贝读写缓冲 |
| 磁盘映射 | `DiskMap.h/.cpp` | devno → backing file fd + 文件大小 |
| 协议定义 | `Protocol.h` | 消息结构体、常量（与 client 侧同步） |

### 3.3 消息解析状态机

`MessageParser` 从 TCP 字节流中识别消息边界：

```
状态转换:
  NEED_HEADER (16 bytes)
    │
    ├── 收到 16B → 验证 Magic (0x474F3243)
    │     ├── Magic OK → 根据 Type 转入对应状态
    │     │     ├── HELLO → NEED_HELLO_BODY (可变长)
    │     │     ├── DATA_BLOCK → NEED_BLOCK_HEADER (52B) → NEED_BLOCK_DATA (CompressedSize)
    │     │     ├── ACK → NEED_ACK_BODY (48B)
    │     │     ├── BYE → NEED_BYE (0B extra, 直接处理)
    │     │     └── ERROR → NEED_ERROR_BODY
    │     └── Magic Bad → 丢弃缓冲区，等待重新同步
    │
    └── 不足 16B → 返回，等待更多数据
```

### 3.4 块处理流程

```
HandleDataBlock(msg):
  │
  ├── 1. 查找 backing file
  │      disk_fd = DiskMap::GetFd(msg.header.DevNo)
  │      不存在 → send ACK(status=INVALID_DEV) → return
  │
  ├── 2. 验证偏移范围
  │      block_offset = msg.block_header.BlockIndex * 1MB
  │      block_offset + msg.block_header.DataSize ≤ DiskMap::GetSize(devno)
  │      超出 → send ACK(status=OUT_OF_RANGE) → return
  │
  ├── 3. 解压 (inflate)
  │      zlib inflate(compressed_data, raw_buffer_1mb)
  │      失败 → send ACK(status=DECOMPRESS_ERROR) → return
  │
  ├── 4. 校验哈希
  │      SHA256(raw_buffer, actual_size) == msg.block_header.Hash ?
  │      不匹配 → send ACK(status=HASH_MISMATCH) → return
  │
  ├── 5. 写入磁盘
  │      pwrite(disk_fd, raw_buffer, actual_size, block_offset)
  │      失败 → send ACK(status=WRITE_ERROR) → return
  │
  └── 6. 回复 ACK
         send ACK(status=OK, block_index, hash)
```

---

## 四、数据结构

### 4.1 Protocol.h — 协议结构体（与 client/NetworkClient.h 同步）

```cpp
// 消息类型
enum class MessageType : uint32_t {
    DATA_BLOCK = 0x01,
    ACK        = 0x02,
    HELLO      = 0x03,
    BYE        = 0x04,
    ERROR      = 0x05,
};

// ACK 状态码
enum class AckStatus : uint32_t {
    OK               = 0,
    HASH_MISMATCH    = 1,
    DECOMPRESS_ERROR = 2,
    WRITE_ERROR      = 3,
    INVALID_DEV      = 4,
    OUT_OF_RANGE     = 5,
};

#pragma pack(push, 1)

// 通用消息头 (16 bytes)
struct MsgHeader {
    uint32_t Magic;      // 0x474F3243 ("C2OG" LE)
    uint32_t Type;       // MessageType
    uint32_t DevNo;      // 磁盘编号
    uint32_t Reserved;
};

// 块消息头 (52 bytes)
struct BlockHeader {
    uint64_t BlockIndex;
    uint32_t DataSize;        // 压缩前原始大小
    uint32_t CompressedSize;  // 压缩后大小
    uint8_t  Hash[32];        // SHA-256
    uint32_t VersionId;
};

// HELLO 消息体
struct HelloBody {
    uint32_t VersionId;
    uint32_t DevNo;
    uint64_t TotalBlocks;
    uint64_t TotalSize;
    wchar_t  VersionType[32];  // UTF-16LE "FULL" / "INCREMENTAL"
};

// ACK 消息体 (48 bytes)
struct AckBody {
    uint64_t BlockIndex;
    uint32_t Status;
    uint32_t DevNo;
    uint8_t  Hash[32];
};

#pragma pack(pop)
```

### 4.2 DiskMap

```cpp
struct BackingFile {
    int fd;                  // 打开的文件描述符
    uint64_t size_bytes;     // 文件大小
};

class DiskMap {
    std::map<uint32_t, BackingFile> map_;
public:
    bool Add(uint32_t devno, const char* path);  // open() + fstat()
    int  GetFd(uint32_t devno) const;
    uint64_t GetSize(uint32_t devno) const;
    bool Contains(uint32_t devno) const;
    void CloseAll();
};
```

### 4.3 RingBuffer

```cpp
class RingBuffer {
    std::vector<uint8_t> buf_;
    size_t read_pos_;    // 下次读取位置
    size_t write_pos_;   // 下次写入位置
    size_t capacity_;
public:
    explicit RingBuffer(size_t capacity);

    // 写入: 从 socket recv() 追加到写指针后
    size_t WriteFromFd(int fd);     // recv() 直接写入环形缓冲区
    size_t WriteFrom(const uint8_t* data, size_t len);

    // 读取: 查看/消费数据
    const uint8_t* ReadPtr() const; // 返回读指针地址
    size_t ReadableBytes() const;   // 可读字节数
    void   Consume(size_t bytes);   // 消费 n 字节（推进读指针）

    // 内部
    size_t WritableBytes() const;
    void   Compact();               // 数据移到缓冲区头部

    // 重置
    void   Reset();
};
```

### 4.4 Connection

```cpp
struct Connection {
    int fd;
    RingBuffer recv_buf;   // 2MB
    RingBuffer send_buf;   // 64KB（ACK 消息缓冲）

    // 当前正在解析的消息状态
    enum State { NEED_HEADER, NEED_BODY } parse_state;
    MsgHeader current_header;

    // 统计
    uint64_t blocks_received;
    uint64_t bytes_written;
    bool     hello_received;
};
```

---

## 五、协议交互时序

### 5.1 正常备份流程

```
Client                                      Server
  │                                           │
  │── HELLO ─────────────────────────────────→│  验证 devno + 文件大小
  │                        ←── ACK(status=OK) │
  │                                           │
  │── DATA_BLOCK[0] ─────────────────────────→│
  │── DATA_BLOCK[1] ─────────────────────────→│  inflate → hash check → pwrite
  │── DATA_BLOCK[2] ─────────────────────────→│  (客户端不等待，继续发)
  │── DATA_BLOCK[3] ─────────────────────────→│
  │── DATA_BLOCK[4] ─────────────────────────→│
  │                        ←── ACK[0] ───────│  处理完块 0
  │── DATA_BLOCK[5] ─────────────────────────→│  客户端释放槽位，发块 5
  │                        ←── ACK[1] ───────│
  │                        ←── ACK[2] ───────│
  │  ...                                       │
  │── BYE ───────────────────────────────────→│
  │                        ←── ACK(status=OK) │
  │                                           │  close(connection)
```

### 5.2 错误处理流程

```
Client                                      Server
  │── DATA_BLOCK[k] ─────────────────────────→│
  │                        ←── ACK(HASH_MISMATCH) │
  │── DATA_BLOCK[k] (重传) ──────────────────→│  幂等覆盖写入
  │                        ←── ACK(OK) ───────│
```

---

## 六、编译与运行

### 6.1 依赖

| 依赖 | Ubuntu 包 | 用途 |
|------|-----------|------|
| CMake ≥ 3.16 | `cmake` | 构建系统 |
| g++ (C++17) | `build-essential` | 编译器 |
| OpenSSL | `libssl-dev` | SHA-256 (`<openssl/sha.h>`) |
| zlib | `zlib1g-dev` | inflate 解压 |

### 6.2 CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.16)
project(cbt-server LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

add_executable(cbt-server
    main.cpp
    Server.cpp
    BlockProcessor.cpp
    MessageParser.cpp
    RingBuffer.cpp
    DiskMap.cpp
)

target_link_libraries(cbt-server PRIVATE
    OpenSSL::Crypto
    ZLIB::ZLIB
)
```

### 6.3 编译命令

```bash
cd server
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### 6.4 开发工作流 (WSL2)

```
Windows: VS Code / VS2022 编辑 server/*.cpp
   │
   │  文件路径: D:\source\drivers\go2cbt\server\
   │  WSL2  中: /mnt/d/source/drivers/go2cbt/server/
   │
   └── WSL2 bash 中编译运行:
         cd /mnt/d/source/drivers/go2cbt/server/build
         cmake .. && make -j$(nproc)
         ./cbt-server --listen 0.0.0.0:9000 --disk 0:/data/disk0.raw
```

---

## 七、安全性 & 错误处理

### 7.1 输入验证

| 检查点 | 行为 |
|--------|------|
| Magic 不匹配 | 丢弃当前缓冲区，尝试重新同步（跳到下一个 Magic 位置） |
| DevNo 不在映射表 | ACK INVALID_DEV，不断开连接 |
| Block offset 超出文件大小 | ACK OUT_OF_RANGE |
| inflate 失败（数据损坏） | ACK DECOMPRESS_ERROR |
| SHA-256 不匹配 | ACK HASH_MISMATCH |
| pwrite 失败（磁盘满/IO错误） | ACK WRITE_ERROR + LOG_ERROR |

### 7.2 信号处理

```cpp
// SIGINT / SIGTERM → 优雅关闭
// 1. 设置 running = false
// 2. 关闭所有 connection
// 3. 关闭所有 backing file
// 4. 退出
```

### 7.3 连接异常

| 场景 | 处理 |
|------|------|
| 客户端断开 | 清理 Connection，关闭 fd，继续 accept |
| 客户端重连 | 创建新 Connection，重新 HELLO 握手 |
| 部分消息到达 | MessageParser 状态保持，等 epoll 通知继续读取 |

---

## 八、与客户端协议的同步约定

客户端 `NetworkClient.h` 和 `BlockStateManager` 中的结构体定义与服务端 `Protocol.h` 必须保持一致：

| 同步项 | 客户端文件 | 服务端文件 |
|--------|-----------|-----------|
| 消息类型枚举 | `NetworkClient.h` | `Protocol.h` |
| `MsgHeader` (16B) | `NetworkClient.h` | `Protocol.h` |
| `BlockHeader` (52B) | `NetworkClient.h` | `Protocol.h` |
| `AckBody` (48B) | `NetworkClient.h` | `Protocol.h` |
| `HelloBody` | `NetworkClient.h` | `Protocol.h` |
| Magic `0x474F3243` | `NetworkClient.h` | `Protocol.h` |
| CBT_BLOCK_SIZE (1MB) | 各模块 | `Protocol.h` |

**修改协议时的流程：** 先改 `docs/BackupEngine.md` 协议文档 → 同步修改客户端和服务端 → 联调验证。
