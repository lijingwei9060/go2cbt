# cbt-server 迭代实施计划

## 开发环境

| 项目 | 说明 |
|------|------|
| 目标平台 | Linux (x86_64) |
| 编译器 | g++ (C++17) |
| 构建系统 | CMake ≥ 3.16 |
| 依赖 | OpenSSL (libssl-dev) + zlib (zlib1g-dev) |
| 源码位置 | `server/` 目录，与现有 Windows 项目并列 |
| 协议同步 | `server/Protocol.h` 需与 `client/NetworkClient.h` 保持一致 |

### 环境准备

```bash
# Ubuntu/Debian
sudo apt install -y build-essential cmake g++ libssl-dev zlib1g-dev

# 验证
g++ --version       # ≥ 9.0 (C++17)
cmake --version     # ≥ 3.16
```

---

## 迭代 0：环境验证

> **目标：CMake + OpenSSL + zlib 编译通过**

### 产出

```
server/
├── CMakeLists.txt
└── main.cpp        # 最小骨架：include 三个头文件，打印 OK 退出
```

### 验收标准

- [ ] `mkdir build && cd build && cmake .. && make -j$(nproc)` 零警告零错误
- [ ] `./cbt-server` 输出 `[cbt-server] epoll + OpenSSL + zlib OK` 后正常退出

### main.cpp 骨架

```cpp
#include <iostream>
#include <sys/epoll.h>
#include <openssl/sha.h>
#include <zlib.h>

int main() {
    std::cout << "[cbt-server] epoll + OpenSSL + zlib OK" << std::endl;
    return 0;
}
```

---

## 迭代 1：启动就绪

> **目标：解析 CLI 参数 → 打开 backing file → 创建 listening socket → 控制台确认就绪**

### 产出

```
server/
├── main.cpp          # 修改：CLI 解析 + 启动编排
├── DiskMap.h
└── DiskMap.cpp
```

### CLI 接口

```
cbt-server --listen <ip:port> --disk <devno>:<filepath> [--disk ...]

示例:
  cbt-server --listen 0.0.0.0:9000 --disk 0:/data/disk0.raw
  cbt-server --listen 0.0.0.0:9000 --disk 0:/data/hdd0.raw --disk 1:/data/hdd1.raw
```

### 启动时行为

1. 解析 `--listen`（必需）和 `--disk`（至少一个）
2. 参数不足 → 打印 usage 并 `exit(1)`
3. 逐个打开 backing file（`O_WRONLY`），`fstat()` 记录大小
4. 打开失败 → 打印错误并 `exit(1)`
5. `socket()` + `setsockopt(SO_REUSEADDR)` + `bind()` + `listen()`
6. 打印就绪信息

### 验收标准

- [ ] `--listen` 和 `--disk` 解析正确
- [ ] 多个 `--disk` 参数均生效
- [ ] backing file 不存在时报错退出（不静默失败）
- [ ] 启动后 `ss -tlnp | grep <port>` 可见 listening socket
- [ ] 预期输出：

```
[cbt-server] Disk 0: /data/disk0.raw (1073741824 bytes, 1.0 GiB)
[cbt-server] Disk 1: /data/disk1.raw (5368709120 bytes, 5.0 GiB)
[cbt-server] Listening on 0.0.0.0:9000
```

---

## 迭代 2：协议解析

> **目标：定义协议结构体 → 实现零拷贝环形缓冲 → 从 TCP 字节流中正确切出每条消息**

### 产出

```
server/
├── Protocol.h         # 与 client/NetworkClient.h 同步
├── RingBuffer.h
├── RingBuffer.cpp
├── MessageParser.h
└── MessageParser.cpp
```

### 2.1 Protocol.h — 协议常量 & 结构体

- [ ] Magic `0x474F3243`（"C2OG" LE）
- [ ] `MessageType` 枚举：DATA_BLOCK=1, ACK=2, HELLO=3, BYE=4, ERROR=5
- [ ] `AckStatus` 枚举：OK=0, HASH_MISMATCH=1, DECOMPRESS_ERROR=2, WRITE_ERROR=3, INVALID_DEV=4, OUT_OF_RANGE=5
- [ ] `#pragma pack(push, 1)` 包裹所有结构体
- [ ] `MsgHeader` (16B): Magic + Type + DevNo + Reserved
- [ ] `BlockHeader` (52B): BlockIndex + DataSize + CompressedSize + Hash[32] + VersionId
- [ ] `AckBody` (48B): BlockIndex + Status + DevNo + Hash[32]
- [ ] `HelloBody` (可变): VersionId + DevNo + TotalBlocks + TotalSize + VersionType[64B UTF-16LE]
- [ ] `CBT_BLOCK_SIZE = 1048576`

### 2.2 RingBuffer — 循环缓冲区

- [ ] `WriteFromFd(int fd)`：`recv()` 零拷贝写入
- [ ] `WriteFrom(data, len)`：内存写入
- [ ] `ReadPtr()` → `const uint8_t*`（不拷贝）
- [ ] `ReadableBytes()` / `WritableBytes()`
- [ ] `Consume(n)`：推进读指针
- [ ] `Compact()`：数据前移
- [ ] `Reset()`

### 2.3 MessageParser — 状态机解析

- [ ] 状态：`NEED_HEADER(16B)` → `NEED_BODY(按消息类型定长)`
- [ ] `TryParse(RingBuffer&)` 返回三种结果：
  - `std::nullopt` — 数据不完整，等多收点数据
  - `ParsedMessage` — 完整消息 { `MsgHeader`, `std::vector<uint8_t> body` }
  - 异常 — Magic 不匹配（返回错误标记，让上层决定丢弃重同步）
- [ ] DATA_BLOCK body 长度 = 52B(BlockHeader) + CompressedSize
- [ ] HELLO body 长度 = 固定大小（或有字段指定）
- [ ] BYE body 长度 = 0（仅消息头）

### 验收标准

- [ ] 完整消息一次性到达 → 正确解析
- [ ] 消息分两段到达（10B + 余下）→ 第一次返回 nullopt，第二次返回完整消息
- [ ] 两条消息粘在一起 → 分别解析出两条
- [ ] Magic 不匹配 → 返回错误标记，不崩溃
- [ ] 可手动用 hex 数据构造测试用例

---

## 迭代 3：功能完成

> **目标：epoll 事件循环 → HELLO 握手 → DATA_BLOCK 处理(inflate→SHA256→pwrite) → ACK 回复 → BYE 断开**
>
> 🏁 **里程碑：服务端端到端功能可用**

### 产出

```
server/
├── main.cpp            # 修改：创建 EventLoop 并启动
├── Server.h
├── Server.cpp          # EventLoop + Connection 管理
├── BlockProcessor.h
└── BlockProcessor.cpp  # inflate + SHA-256 + pwrite
```

### 3.1 BlockProcessor — 单块处理流水线

```
ProcessBlock(devno, compressed_data, block_header):
  │
  ├─ 1. 查 DiskMap → fd, file_size
  │     └─ devno 不存在 → ACK(INVALID_DEV)
  │
  ├─ 2. 校验偏移
  │     offset = block_header.BlockIndex * 1MB
  │     offset + block_header.DataSize > file_size → ACK(OUT_OF_RANGE)
  │
  ├─ 3. inflate 解压 (zlib)
  │     └─ 失败 → ACK(DECOMPRESS_ERROR)
  │
  ├─ 4. SHA-256 校验
  │     SHA256(raw_data, DataSize) == block_header.Hash ?
  │     └─ 不匹配 → ACK(HASH_MISMATCH)
  │
  ├─ 5. pwrite 写入
  │     pwrite(fd, raw_data, DataSize, offset)
  │     └─ 失败 → ACK(WRITE_ERROR)
  │
  └─ 6. 返回 ACK(OK, block_index, hash)
```

### 3.2 Server (EventLoop) — 事件循环

```
epoll_wait 循环:
  │
  ├─ listening_fd 可读 → accept4(SOCK_NONBLOCK) → 创建 Connection
  │
  ├─ client_fd 可读:
  │      recv() → RingBuffer::WriteFromFd()
  │      while (msg = MessageParser::TryParse()):
  │          ├─ HELLO      → HandleHello()
  │          ├─ DATA_BLOCK → HandleDataBlock()
  │          └─ BYE        → HandleBye()
  │
  └─ client_fd 可写 (有待发 ACK):
         send() 从 Connection::send_buf 发送
```

### 3.3 HandleHello

- [ ] 验证 `devno` 在 DiskMap 中
- [ ] 验证 backing file 大小 ≥ TotalSize
- [ ] 验证通过 → ACK(OK)
- [ ] 验证失败 → ACK(ERROR) + 断开连接

### 3.4 HandleBye

- [ ] 发送 ACK(OK)
- [ ] 关闭连接 fd
- [ ] 打印统计（块数、字节数）

### 验收标准

- [ ] 完整备份链路：HELLO → 收 N 块 → 每块验哈希 → pwrite 写盘 → 逐个 ACK → BYE
- [ ] backing file 中写入的数据与原始数据逐字节一致
- [ ] HASH_MISMATCH 时正确返回错误 ACK，数据不写入
- [ ] OUT_OF_RANGE 时拒绝写入
- [ ] 客户端重传同一块 → 覆盖写入，幂等
- [ ] 单个消息分多段到达 → 正确处理
- [ ] 多条消息粘包 → 正确拆分

---

## 迭代 4：生产加固

> **目标：日志系统、异常恢复、优雅关闭、运行统计**

### 产出

```
server/
├── Logger.h / .cpp     # 新增：日志模块
├── Server.cpp          # 修改：错误处理增强
└── main.cpp            # 修改：信号处理
```

### 4.1 Logger

- [ ] 级别：DEBUG / INFO / WARNING / ERROR
- [ ] `--verbose` 开启 DEBUG
- [ ] 格式：`[2026-07-13 14:30:05] [INFO] 消息内容`
- [ ] 输出到 stdout（兼容 systemd journal / docker logs / 重定向到文件）
- [ ] 宏：`LOG_DEBUG(...)`, `LOG_INFO(...)`, `LOG_WARNING(...)`, `LOG_ERROR(...)`

### 4.2 健壮性

- [ ] 客户端意外断开（`recv()==0` / ECONNRESET） → `LOG_INFO`，清理连接，继续 accept
- [ ] Magic 不匹配 → `LOG_WARNING`，丢弃 1 字节重新扫描
- [ ] `SIGPIPE` 屏蔽（`signal(SIGPIPE, SIG_IGN)`）
- [ ] EPOLLERR / EPOLLHUP → 清理连接
- [ ] 所有 HandleDataBlock 失败路径都有 `LOG_ERROR` + ACK 错误状态码

### 4.3 统计 & 进度

- [ ] 每处理 100 块 → `LOG_INFO` 进度（已处理块数 / 字节数 / 速度）
- [ ] BYE 时 → 打印摘要：`Total blocks: 10240, Bytes: 10.0 GiB, Errors: 0`
- [ ] SIGINT/SIGTERM 时 → 打印摘要后退出

### 4.4 优雅关闭

```
收到 SIGINT 或 SIGTERM:
  1. 设置 running = false（epoll_wait 返回）
  2. 关闭所有 client connection
  3. 打印运行统计摘要
  4. DiskMap::CloseAll()
  5. close(listening_fd)
  6. exit(0)
```

### 验收标准

- [ ] `--verbose` 输出 DEBUG 日志，默认输出 INFO
- [ ] 处理错误块时日志级别正确（HASH_MISMATCH → WARNING，WRITE_ERROR → ERROR）
- [ ] Ctrl+C 优雅退出，打印统计
- [ ] 客户端意外断开后服务端不崩溃，可接受新连接
- [ ] `valgrind` 无内存泄漏（正常流程 + 异常流程）

---

## 迭代 5：联调测试

> **目标：Windows 客户端 ↔ Linux 服务端真实联调，全部场景通过**
>
> 🏁 **里程碑：系统可投产**

### 测试场景

| # | 场景 | 预期结果 |
|---|------|---------|
| A | HELLO 握手 | 服务端验证 devno + 文件大小，ACK OK |
| B | HELLO devno 不存在 | ACK ERROR，连接断开 |
| C | HELLO 文件太小 | ACK ERROR，连接断开 |
| D | 10 块小规模传输 | 全部 ACK OK，backing file 数据正确 |
| E | SHA-256 不匹配 | ACK HASH_MISMATCH，数据不写入 |
| F | 客户端重传错误块 | 覆盖写入，ACK OK |
| G | 流水线并发（窗口=5） | 客户端不等待 ACK 连续发，服务端按序处理 |
| H | 100 块传输 + BYE | 统计正确，连接正常关闭 |
| I | 客户端中途断开 | 服务端不崩溃，统计当前进度 |
| J | 重连后重新 HELLO | 服务端接受新连接，正常处理 |

### 测试准备

```bash
# 服务端侧：创建测试 backing file
truncate -s 1G /tmp/test_disk0.raw

# 启动服务端
./cbt-server --listen 0.0.0.0:19000 --disk 0:/tmp/test_disk0.raw --verbose

# 客户端侧（Windows）：备份到 Linux 服务端
client.exe backup 0 --serverip <linux-ip> --port 19000

# 验证写入正确性
# 在 Linux 侧提取 backing file 中的块，与客户端原始数据对比 SHA-256
```

---

## 迭代总览

```
迭代 0          迭代 1          迭代 2          迭代 3          迭代 4          迭代 5
编译通过   →   启动就绪    →   协议解析    →   功能完成    →   生产加固    →   联调测试
                                    🏁 里程碑                   🏁 里程碑
15min          30min           45min           60min           30min           45min
                                                                           ≈ 3.75h
```

### 文件清单（最终）

```
server/
├── CMakeLists.txt         # 迭代 0
├── main.cpp               # 迭代 0 → 1 → 3 → 4 渐进修改
├── Protocol.h             # 迭代 2（只读，与 client/NetworkClient.h 同步）
├── RingBuffer.h / .cpp    # 迭代 2
├── MessageParser.h / .cpp # 迭代 2
├── DiskMap.h / .cpp       # 迭代 1
├── BlockProcessor.h / .cpp# 迭代 3
├── Server.h / .cpp        # 迭代 3 → 4 修改
└── Logger.h / .cpp        # 迭代 4
```

### 明确不做的

- ❌ 多客户端并发
- ❌ 数据恢复协议（Server→Client）
- ❌ 去重
- ❌ SQLite / 任何持久化元数据
- ❌ HTTP 管理 API
- ❌ systemd service 文件（后续加）
- ❌ Docker 镜像
- ❌ 单元测试框架（迭代期间手动验证，后续补）

### 待同步的客户端改动

服务端完成后，`client/NetworkClient` 需要从停等协议改为流水线窗口模式（窗口大小 5-6），以匹配服务端的流水线处理能力。
