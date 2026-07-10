# go2cbt — Windows CBT (Changed Block Tracking) 内核驱动

Windows WDM 内核驱动，通过 Hook 磁盘驱动的 `IRP_MJ_WRITE` 分发函数，实时追踪磁盘写操作，以位图（RTL_BITMAP）方式记录哪些数据块发生了变更。配合用户态工具 `cbtctl.exe` 查询、重置、可视化 CBT 位图。

适用于增量备份、实时同步等需要精确感知磁盘变更场景的底层支撑组件。

---

## 项目结构

```
go2cbt/
├── go2cbt/                    # 内核驱动 (WDM KMDF)
│   ├── go2cbt.h               # 公共头文件：结构体、IOCTL 定义、函数声明
│   ├── go2cbt.c               # DriverEntry / CbtUnload (设备创建/销毁)
│   ├── hook.c                 # 构建磁盘映射表 + 安装 Write Hook
│   ├── write.c                # HwReadWrite 分发函数 + MarkBlockChanged
│   ├── ioctl.c                # IOCTL_CBT_QUERY / IOCTL_CBT_RESET 处理
│   ├── bitmap.c               # InitCbtState / CleanupCbtState (位图生命周期)
│   └── dispatch.c             # IRP_MJ_CREATE / IRP_MJ_CLOSE 分发
│
├── cbtctl/                    # 用户态控制程序
│   └── main.cpp               # query / reset / dump 三条命令
│
├── go2cbt.sln                 # Visual Studio 解决方案文件
└── README.md                  # 本文件
```

---

## 工作原理

### 整体架构

```
应用程序 (备份软件/同步工具)
    │
    ▼  DeviceIoControl(IOCTL_CBT_QUERY / _RESET)
┌─────────────┐
│  cbtctl.exe │ ◄──────── 用户态控制工具
└─────────────┘
    │
    ▼  \\.\CbtMonitor (控制设备)
┌──────────────────────────────────────────┐
│            go2cbt.sys (内核驱动)          │
│                                          │
│  ┌──────────┐   ┌────────────────────┐  │
│  │ Hook 表   │   │ 磁盘映射表         │  │
│  │ (按驱动)  │──▶│ DiskNumber → 设备  │  │
│  └──────────┘   │ CBT 位图状态       │  │
│                  └────────────────────┘  │
│                          │               │
│                   IRP_MJ_WRITE 拦截      │
│                   MarkBlockChanged()     │
│                   RtlSetBits()           │
└──────────────────────────────────────────┘
    │
    ▼  InterlockedExchangePointer 替换 MajorFunction[IRP_MJ_WRITE]
┌──────────────────────────────────────────┐
│  文件系统 → VolMgr → PartMgr → disk.sys  │
│              ↓                           │
│        \Device\Harddisk0\Partition0      │  ← 仅监控此设备
└──────────────────────────────────────────┘
```

### 核心设计决策

| 决策 | 选择 | 原因 |
|------|------|------|
| 监控目标 | **仅 Partition0**（整盘） | 日志验证：所有分区写入在 Partition0 上都有对应写入，DiskOff 完全一致 |
| 数据结构 | **RTL_BITMAP**（每个磁盘一个） | 固定内存开销、天然去重、O(n) 范围查询、行业标准（Veeam/VMware 同方案） |
| 并发保护 | **KSPIN_LOCK** | 保护 RtlSetBits/RtlClearAllBits 的原子性，丢失 set = 备份缺块 = 数据损坏 |
| 追踪粒度 | **1 MB per block**（`CBT_BLOCK_SIZE`） | 可在 `go2cbt.h` 中调整；越小越精确但内存越大 |

### 内存占用估算

| 磁盘大小 | 总块数 | 位图内存占用 |
|----------|--------|-------------|
| 100 GB   | ~102,400 | ~12.8 KB |
| 300 GB   | ~307,200 | ~38.4 KB |
| 1 TB     | ~1,048,576 | ~128 KB |
| 4 TB     | ~4,194,304 | ~512 KB |
| 16 TB    | ~16,777,216 | ~2 MB |

---

## 安装依赖

### 开发环境

- **Visual Studio 2022** (v17.0+)
- **Windows Driver Kit (WDK) 11** — 与 VS 版本匹配
  - 要求 SDK 版本 >= Windows 10 Version 2004 (10.0.19041.0)，否则 `ExAllocatePool2` 不可用
- 目标平台：**x64**

> 注意：本项目仅支持 x64。ARM64 配置存在于解决方案中但未经验证。

### 运行环境

- Windows 10 / Windows Server 2016 及以上版本（64 位）
- 需要管理员权限安装/卸载驱动
- 测试模式签名或正式代码签名证书

---

## 编译

1. 用 Visual Studio 打开 `go2cbt.sln`
2. 选择平台配置：
   - `Debug|x64` 或 `Release|x64`
3. 菜单：**生成 → 生成解决方案**（或 Ctrl+Shift+B）
4. 输出位置：

```
x64\Debug\ 或 x64\Release\
├── go2cbt.sys          # 内核驱动
├── go2cbt.pdb          # 调试符号
├── go2cbt.inf          # 安装 INF
├── cbtctl.exe          # 用户态控制程序
└── cbtctl.pdb          # 调试符号
```

---

## 安装

需要debug，修改一下注册表，Sysinternal Suits中的dbgview 看不到日志：
```cmd
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter" /v DEFAULT /t REG_DWORD /d 0xf /f
```

### 手动服务注册

```cmd
:: 以管理员身份运行
copy go2cbt.sys C:\Windows\System32\drivers\go2cbt.sys
sc create go2cbt type=kernel binpath=C:\Windows\System32\drivers\go2cbt.sys start=demand

:: 启动驱动
sc start go2cbt
```


### 卸载

```cmd
:: 以管理员身份运行
sc stop go2cbt
sc delete go2cbt
```

---

## 使用方法

### cbtctl.exe 命令行工具

```cmd
cbtctl.exe <command> <diskno> [参数]

Commands:
  query  <diskno>              查询 CBT 位图摘要信息
  reset  <diskno>              重置 CBT 位图（开始新的快照周期）
  dump   <diskno> [outfile]    将位图可视化为 ASCII 艺术（输出到屏幕或文件）

Examples:
  cbtctl.exe query 0           # 查看 Harddisk0 的 CBT 状态
  cbtctl.exe reset 0           # 备份完成后重置 Harddisk0 的位图
  cbtctl.exe dump 0            # 在屏幕上显示位图可视化
  cbtctl.exe dump 0 report.txt # 将报告保存到文件
```

### Query 输出示例

```
=== Disk0 CBT Summary ===
  Total Bits (blocks):  307200
  Bitmap Bytes:         38400
  Block Size:            1024 KB
  Disk Approx Size:      292.97 GB
  Est. Change Rate:      3.52% (sampled first 10000 blocks)

Use 'dump' command for full visualization and exact statistics.
```

### Dump 输出格式

每行 64 bit（8 组 × 8 bit）：
- `'0'` = 未变更的块
- `'*'` = 已变更的块

```
================================================================================
                    CBT BITMAP VISUALIZATION - Disk Report
================================================================================

  Total bits (blocks): 307200
  Block size:          1024 KB (1048576 bytes)
  Bitmap data size:    38400 bytes
  Disk approx size:    292.97 GB

  Legend: '0' = unchanged    '*' = changed
  Each row = 64 blocks (65536 KB)
  Each group of 8 chars = 8 blocks (8192 KB for 1MB block size)

  [0000000000] 0x0000000000000000: 00000*** ****0*** ******** ********
  [0000000064] 0x0000000000040000: ******** ******** ******** ***00000

                              SUMMARY STATISTICS
================================================================================

  Changed Blocks:           10,813  / 307200  (3.5200%)
  Unchanged Blocks:        296,387  / 307200  (96.4800%)
  Changed Data Size:        10.56 GB
  Changed Range Count:         142  contiguous region(s)
  Max Contiguous Run:        512.00 MB  (512 blocks)
  Avg Run Length:             76.14 blocks

  CHANGE DENSITY MAP (64 segments, each = 1.5625% of disk):
  . . o O # @ @ . . . . . . . . . . . . . . . . . . . . . . .
  Legend: .=<1%  :=<5%  o=<15%  O=<30%  #=<50%  @=>50%
```

---

## 典型工作流程（增量备份集成）

```
                    备份周期开始
                        │
              ┌─────────▼─────────┐
              │  cbtctl reset 0    │  ← 清零位图，开始新周期
              └─────────┬─────────┘
                        │
              ┌─────────▼─────────┐
              │  正常业务运行...   │  ← 驱动自动追踪所有写操作
              │  (持续数小时/天)    │
              └─────────┬─────────┘
                        │
              ┌─────────▼─────────┐
              │  cbtctl dump 0     │  ← 读取变更位图
              └─────────┬─────────┘
                        │
              ┌─────────▼─────────┐
              │  备份软件读取位图   │  ← 只备份标记为 * 的块
              │  执行增量传输      │
              └─────────┬─────────┘
                        │
              ┌─────────▼─────────┐
              │  cbtctl reset 0    │  ← 备份完成，清零进入下一周期
              └──────────────────┘
```

---

## IOCTL 接口说明

| IOCTL | 功能 | 输入 | 输出 |
|-------|------|------|------|
| `IOCTL_CBT_QUERY` (0x802) | 查询 CBT 位图 | `CBT_IOCTL_INPUT{DeviceNumber}` | `CBT_QUERY_OUTPUT{TotalBits, TotalBytes, BitmapData[]}` |
| `IOCTL_CBT_RESET` (0x803) | 重置 CBT 位图 | `CBT_IOCTL_INPUT{DeviceNumber}` | 无 |

### 查询协议（两步查询）

由于不同磁盘大小导致位图尺寸差异很大（12KB ~ 2MB），查询采用两步协议：

1. **第一次调用**：传入小缓冲区（如 64 字节）。驱动返回 `STATUS_BUFFER_OVERFLOW`，但在缓冲区中填充了 `TotalBits` 和 `TotalBytes` 元数据。
2. **第二次调用**：根据元数据分配足够大的缓冲区，再次调用获取完整位图数据。

> 此设计绕过了 METHOD_BUFFERED 模式下 `STATUS_BUFFER_TOO_SMALL` 不回传数据的限制。

---

## 注意事项

### ⚠️ 安全相关

1. **测试模式签名**：默认编译出的 `.sys` 使用测试签名，需要在测试模式下加载：
   ```cmd
   bcdedit /set testsigning on
   ```
   重启后生效。生产环境需购买正式 EV 代码签名证书。

2. **管理员权限**：安装、启动、停止、卸载驱动均需**管理员权限**。

3. **BSOD 风险**：内核驱动代码错误可能导致蓝屏。建议：
   - 先在虚拟机中充分测试
   - 启用 WinDbg 内核调试以便崩溃时定位问题
   - Debug 版本包含 KdPrint 调试日志（前 100 条 WRITE 有输出）

### ⚠️ 性能相关

4. **KdPrint 性能影响**：Debug 版本的前 100 条 WRITE 会输出调试日志。高 IOPS 场景下建议使用 Release 版本或进一步减少/移除 KdPrint 调用。

5. **自旋锁持有时间**：`MarkBlockChanged()` 中持有 KSPIN_LOCK 的时间约为单次 `RtlSetBits` 调用（~10ns 级别），对 I/O 延迟的影响可忽略。

6. **内存占用**：每块 1MB 粒度下，16TB 磁盘仅需 ~2MB NonPagedPool 内存。可通过修改 `CBT_BLOCK_SIZE` 调整精度与内存的平衡。

### ⚠️ 设计约束

7. **仅支持 Partition0 监控**：当前实现仅 Hook `\Device\HarddiskN\Partition0` 的 Write。不单独监控各逻辑分区，因为 Partition0 已覆盖全盘物理写入。

8. **不支持动态热插拔磁盘**：驱动启动时扫描并固定磁盘列表，运行中新插入的磁盘不会被自动纳入监控。需要重启驱动才能识别新磁盘。

9. **不支持多路径存储**：如果同一物理磁盘通过多条路径（MPIO）暴露给系统，可能产生重复追踪。当前未做去重处理。

10. **METHOD_BUFFERED IO 大小限制**：单个 IOCTL 请求缓冲区上限取决于系统配置（通常 64KB~1MB）。对于超大磁盘（>32TB），位图可能超过此限制，需考虑分页传输或其他机制。

---

## 数据安全声明

**本软件（go2cbt 驱动及配套工具）按原样提供（AS IS），不作任何明示或暗示的担保。**

使用者须充分了解并同意以下事项：

1. **内核级风险**：本软件作为 Windows 内核驱动运行于 Ring 0 权限级别，代码缺陷可能导致操作系统崩溃（BSOD）、数据损坏或系统不稳定。

2. **备份数据完整性依赖性**：本软件提供的 CBT 变更追踪信息是增量备份的数据来源依据。如果位图追踪出现遗漏（竞态条件、极端情况等），将直接导致备份数据不完整，进而造成**数据丢失且不可恢复**。

3. **非生产就绪声明**：当前版本为开发/测试阶段软件。在任何生产环境部署前，必须经过充分的端到端验证，包括但不限于：
   - 各种 I/O 负载下的压力测试
   - 长时间运行的稳定性测试
   - 与备份软件的集成测试
   - 异常场景测试（断电模拟、强制关机、驱动卸载等）

4. **无 SLA 保证**：作者不对因使用本软件导致的任何直接或间接损失承担责任，包括但不限于业务中断、数据丢失、硬件损坏等。

**强烈建议在生产环境使用前咨询专业安全审计，并获得正式的代码签名证书。**

---

## 问题反馈

欢迎通过以下方式提出问题或建议：

- **Bug 报告**：请附上复现步骤、系统环境（Windows 版本、磁盘型号）、驱动日志（KdPrint 输出）、以及 WinDbg 崩溃转储分析结果（如有）。
- **功能建议**：描述期望的行为和使用场景。
- **安全问题**：发现潜在的安全漏洞请尽快反馈，不要公开发布细节。

---

## 许可证

见 [LICENSE.txt](LICENSE.txt) 文件。
