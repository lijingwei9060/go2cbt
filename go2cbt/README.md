# go2cbt — Windows 内核级磁盘 CBT (Changed Block Tracking) 驱动

## 概述

go2cbt 是一个 **WDM (Windows Driver Model) 内核驱动**，通过 Hook 磁盘驱动的 `IRP_MJ_WRITE` 分发函数，实时追踪物理磁盘上的数据块变更。适用于增量备份、磁盘快照等场景。

### 工作原理概要

```
用户态写入文件 (例如: C:\data\file.txt)
    ↓
文件系统驱动 (NTFS) 发送 WRITE IRP
    ↓
卷管理器 / 分区管理器 转发 IRP
    ↓
disk.sys 的 MajorFunction[IRP_MJ_WRITE]  ← 被 go2cbt Hook 成 HwReadWrite()
    ↓
HwReadWrite() 截获写请求:
    ├─ 计算绝对物理偏移 (DiskOff = PartOff + PartitionStartOffset)
    ├─ 标记 CBT 位图 (记录哪些块被修改)
    └─ 调用原始 OriginalWrite() 完成实际写入
```

---

## 架构总览

```
┌─────────────────────────────────────────────────────┐
│                    用户态程序                        │
│  CreateFile("\\\\.\\CbtMonitor") → DeviceIoControl │
└──────────────────┬──────────────────────────────────┘
                   │ IOCTL
                   ↓
┌─────────────────────────────────────────────────────┐
│              go2cbt.sys (本驱动)                     │
│                                                     │
│  ┌──────────────┐   ┌────────────────────────────┐  │
│  │ 控制设备对象  │   │      全局表                │  │
│  │ \Device\     │   │ ┌──────────────────────┐  │  │
│  │ CbtMonitor   │→  │ │ g_HookList[]         │  │  │
│  │              │   │ │ 按 DriverObject 索引  │  │  │
│  │ 分发函数:     │   │ │ 保存 OriginalWrite   │  │  │
│  │ CREATE/CLOSE │   │ └──────────────────────┘  │  │
│  │ DEVICE_CONTROL│   │ ┌──────────────────────┐  │  │
│  └──────────────┘   │ │ g_DiskMap[]           │  │  │
│                     │ │ 按 DeviceObject 索引   │  │  │
│  Hook 入口:          │ │ 关联 Partition 信息   │  │  │
│  HwReadWrite()       │ └──────────────────────┘  │  │
│  (替换目标驱动的      └────────────────────────────┘  │
│   MajorFunction[4])                              │
└─────────────────────────────────────────────────────┘
                   │ OriginalWrite()
                   ↓
┌─────────────────────────────────────────────────────┐
│              目标磁盘驱动栈                           │
│  PartMgr → disk.sys → storahci → 物理磁盘            │
└─────────────────────────────────────────────────────┘
```

---

## 文件结构

| 文件 | 职责 |
|------|------|
| `go2cbt.c` | 驱动入口 (`DriverEntry`)、卸载 (`CbtUnload`)、控制设备创建与清理 |
| `go2cbt.h` | 头文件：宏定义、数据结构声明、全局变量 extern、前向声明 |
| `hook.c` | 核心 Hook 逻辑：`FindOrCreateHookEntry()` 去重安装 Hook、`BuildDiskAndHookTables()` 枚举分区并构建映射表、`QueryPartitionInfoEx()` 查询分区信息 |
| `write.c` | Hook 目标函数 `HwReadWrite()`：截获写请求、计算物理偏移、CBT 记录、透传到原函数 |
| `dispatch.c` | 辅助分发函数：`CbtDispatchCreate` / `CbtDispatchClose`（处理用户态打开/关闭控制设备） |
| `ioctl.c` | IOCTL 处理：`CbtDispatchDeviceControl`（用户态查询接口） |

---

## 核心机制：IRP_MJ_WRITE Hook

### 什么是 Hook？

Windows 每个内核驱动都有一个 `DRIVER_OBJECT` 结构体，其中包含一个**分发函数表** `MajorFunction[28]`。当上层发送 I/O 请求包 (IRP) 时，I/O Manager 根据 `MajorFunction[IRP 类型]` 找到对应的回调函数调用。

go2cbt 将目标驱动（如 `partmgr.sys` / `disk.sys`）的 `MajorFunction[IRP_MJ_WRITE]`（索引 4）**原子替换为自己的函数 `HwReadWrite`**：

```c
// hook.c:48 — 原子操作替换函数指针
InterlockedExchangePointer(
    (PVOID*)&DriverObject->MajorFunction[IRP_MJ_WRITE],  // 目标位置
    (PVOID)HwReadWrite                                    // 我们的函数
);
```

替换后，所有发往该驱动的写请求都会先进入 `HwReadWrite()`。

### 为什么是安全的

1. **原子操作**：使用 `InterlockedExchangePointer` 保证在多核 CPU 上不会出现指针半写状态
2. **保存原始指针**：在 Hook **之前**保存 `OriginalWrite`，确保透传时能调用真正的处理函数
3. **去重逻辑**：同一个 `DriverObject` 只被 Hook 一次（通过 `FindOrCreateHookEntry` 的查找-或-创建模式）
4. **自验证**：Hook 后立即验证 `MajorFunction[4]` 是否确实等于 `HwReadWrite`
5. **安全卸载**：卸载时只在自己安装的 Hook 上恢复，检测到其他驱动的 Hook 时跳过

---

## 数据结构设计

### 双表架构

驱动维护两个关联的全局数组表：

#### 表 1：HOOK_ENTRY[] — Hook 表（按 DriverObject）

```c
typedef struct _HOOK_ENTRY {
    PDRIVER_OBJECT   DriverObject;        // 被 Hook 的驱动对象
    PDRIVER_DISPATCH OriginalWrite;        // 该驱动的原始写函数指针
    BOOLEAN          HookInstalled;        // Hook 安装是否成功
    ULONG            RefCount;             // 多少个设备引用此驱动
    LARGE_INTEGER    HookInstallTime;      // 安装时间戳
} HOOK_ENTRY;
```

**作用**：解决"多个不同驱动各有自己的原始写函数"问题。每个被 Hook 的驱动（如 disk.sys、vhdmp.sys）在此表中有一条记录，保存各自的 `OriginalWrite`。

#### 表 2：DISK_MAP_ENTRY[] — 磁盘映射表（按 DeviceObject）

```c
typedef struct _DISK_MAP_ENTRY {
    PDEVICE_OBJECT   DeviceObject;              // IRP 中的设备对象 (查找键)
    PHOOK_ENTRY      HookEntry;                 // ← 指向 Hook 表的对应条目
    ULONG            DiskNumber;                // Harddisk%d 中的 %d
    ULONG            PartitionNumber;             // Partition%d 中的 %d
    LARGE_INTEGER    PartitionStartingOffset;     // 分区起始字节偏移
    LARGE_INTEGER    PartitionLength;             // 分区总大小(字节)
    BOOLEAN          IsPartition0;               // 是否为整盘(Partition0)
} DISK_MAP_ENTRY;
```

**作用**：解决"如何从 DeviceObject 反查 DiskNumber 和分区偏移"。`HwReadWrite` 收到的参数只有 `DeviceObject`，通过此表找到对应的分区信息进行偏移转换。

#### 两表关系

```
g_HookList[]                    g_DiskMap[]
┌────────────────┐              ┌────────────────────────────────┐
│ [0] DrvObj=    │◄────────────│ [0] DevObj=Part0, HookEntry→[0]│
│ partmgr.sys    │              │ [1] DevObj=Part1, HookEntry→[0]│
│ OrigWrite=xxx  │              │ [2] DevObj=Part2, HookEntry→[0]│
│ RefCount=6     │              │ [3] DevObj=Part3, HookEntry→[0]│
├────────────────┤              │ [4] DevObj=Part4, HookEntry→[0]│
│ (空闲...)      │              │ [5] DevObj=Part5, HookEntry→[0]│
└────────────────┘              └────────────────────────────────┘
```

**关键点**：同一块磁盘的 6 个分区（Part0~5）共享同一个 `DriverObject`（partmgr.sys），因此共享同一条 `HOOK_ENTRY`（RefCount=6），但各自有独立的 `DISK_MAP_ENTRY`（不同的 StartingOffset）。

---

## 初始化流程 (DriverEntry)

```
DriverEntry(DriverObject)
    │
    ├── 1. IoCreateDevice("\Device\CbtMonitor")     创建控制设备对象
    │
    ├── 2. IoCreateSymbolicLink("\DosDevices\CbtMonitor")
    │       创建符号链接 (用户态可访问)
    │
    ├── 3. 设置分发函数:
    │       IRP_MJ_CREATE      → CbtDispatchCreate
    │       IRP_MJ_CLOSE       → CbtDispatchClose
    │       IRP_MJ_DEVICE_CONTROL → CbtDispatchDeviceControl
    │       DriverUnload       → CbtUnload
    │
    ├── 4. 设置 DO_BUFFERED_IO + 清除 DO_DEVICE_INITIALIZING
    │
    └── 5. BuildDiskAndHookTables(DriverObject)
            │
            └── for diskNum = 0..7:
                └── for partNum = 0..16:
                    │
                    ├── 构造设备名 "\Device\Harddisk{N}\Partition{M}"
                    │
                    ├── IoGetDeviceObjectPointer(name)
                    │   获取设备对象 (失败则该磁盘/分区不存在, break)
                    │
                    ├── FindOrCreateHookEntry(drvObj)
                    │   ├── 在 g_HookList 中查找已有条目?
                    │   │   └── 有 → RefCount++, 返回已有条目 (去重!)
                    │   └── 无 → 保存 OriginalWrite
                    │           InterlockedExchangePointer(Hook HwReadWrite)
                    │           自验证 → 返回新条目
                    │
                    ├── QueryPartitionInfoEx(devObj)
                    │   通过 IOCTL_DISK_GET_PARTITION_INFO_EX
                    │   获取分区的 StartingOffset + PartitionLength
                    │
                    └── 填充 g_DiskMap[] 条目
```

---

## 写拦截流程 (HwReadWrite)

每次有写请求到达被 Hook 的驱动时，`HwReadWrite` 被调用：

```
HwReadWrite(DeviceObject, Irp)
    │
    ├── Step 0: 检查 MajorFunction == IRP_MJ_WRITE?
    │   └── 否 → 直接透传到当前 MajorFunction (非WRITE请求不干预)
    │
    ├── Step 1: 从 IRP 提取写参数:
    │   byteOffset = irpSp->Parameters.Write.ByteOffset
    │   byteCount  = irpSp->Parameters.Write.Length
    │
    ├── Step 2: 在 g_DiskMap 中查找 DeviceObject:
    │   └── 找到? → diskEntry = 匹配项
    │   └── 未找到?
    │       ├── 在 g_HookList 中按 DriverObject 查找?
    │       │   └── 有 Hook 条目? → 安全透传 OriginalWrite
    │       │       (这是 VolMgr 卷层设备等已知但未注册的设备)
    │       └── 完全未知? → 紧急透传 + 日志警告
    │
    ├── Step 3: 获取 OriginalWrite:
    │   originalWrite = diskEntry->HookEntry->OriginalWrite
    │
    ├── Step 4: 偏移转换:
    │   DiskAbsoluteOffset = byteOffset + PartitionStartingOffset
    │
    │   示例 (Part3):
    │   用户态写入 C 盘 offset 0x10000
    │   Part3.StartingOffset = 0xD900000 (~217MB)
    │   DiskAbsoluteOffset = 0x10000 + 0xD900000 = 0xD910000
    │
    ├── Step 5: CBT 记录 (TODO: RTL_BITMAP 标记变更块)
    │
    └── Step 6: 透传完成:
        return originalWrite(DeviceObject, Irp)
```

### 关于 "Unknown 设备"

日志中可能出现 `Unknown disk device` 的设备对象（如 `partmgr.sys` 创建的无下层栈卷入口设备）。这些设备的特征：

- 与已注册分区共享同一个 `DriverObject`（因此已被 Hook）
- 收到的写请求与某个分区的写入**完全一致**（相同的 Offset + Length）
- 是 I/O 转发的中间节点，不是物理 I/O 终点

**结论**：不需要追踪这些设备。每笔物理块变更都会被底层的 Partition 设备完整捕获，Unknown 设备只是转发噪音。当前实现对这些设备执行静默透传（调试日志已注释掉）。

---

## 卸载流程 (CbtUnload)

```
CbtUnload(DriverObject)
    │
    ├── Step 1: 遍历 g_HookList[], 逐个恢复 Hook:
    │   for each entry in g_HookList:
    │     if entry.HookInstalled:
    │       currentWrite = DriverObject->MajorFunction[IRP_MJ_WRITE]
    │       if currentWrite == HwReadWrite:
    │         InterlockedExchangePointer(→ OriginalWrite)  // 安全恢复
    │       elif currentWrite == OriginalWrite:
    │         已被他人恢复, skip
    │       else:
    │         另一个驱动在我们之后也 Hook 了, skip (避免破坏对方)
    │
    ├── Step 2: 删除符号链接:
    │   IoDeleteSymbolicLink("\DosDevices\CbtMonitor")
    │
    ├── Step 3: 删除控制设备:
    │   IoDeleteDevice(g_ControlDevice)
    │
    └── Step 4: 清零全局状态:
        RtlZeroMemory(g_HookList), RtlZeroMemory(g_DiskMap)
```

---

## IOCTL 接口

| IOCTL Code | 功能 | 方向 |
|------------|------|------|
| `IOCTL_QUERY_HOOK_STATUS` (0x800) | 查询 Hook 安装状态 | 用户态→内核 |
| `IOCTL_QUERY_WRITE_STATS` (0x801) | 查询写统计信息 | 用户态→内核 |
| `IOCTL_QUERY_CBT_DATA` (0x802) | 获取 CBT 变更数据 | 用户态←内核 |

通信方式：
- 方法：`METHOD_BUFFERED`（I/O Manager 自动缓冲）
- 设备路径：`\\\\.\\CbtMonitor`
- 访问权限：`FILE_ANY_ACCESS`

---

## 关键设计决策

### 1. 为什么只监控 Partition0 就够了？（重要发现！）

实测日志证明：**每当任何分区（Part1~5）有一次写入，Partition0 必然有对应的一次写入，且 DiskOff（绝对物理偏移）完全一致。**

原因：分区管理器将各分区的写请求统一转发到底层物理磁盘（Part0 代表整盘），所以 **Part0 层能看到所有分区的全部物理块变更**。

这意味着 CBT 追踪可以大幅简化——只需监控 Part0，直接以 `DiskOff / BlockSize` 作为位图索引，无需逐分区查找和偏移转换。

### 2. 为什么选择 Hook MajorFunction 而不是过滤驱动？

| 方案 | 优点 | 缺点 |
|------|------|------|
| **过滤驱动 (Filter Driver)** | 微软官方推荐；稳定；支持 PnP | 需要在 INF 中注册设备栈；需要处理 AttachDevice/DetachDevice；复杂度高 |
| **Hook MajorFunction** (当前方案) | 实现**极简**（~300 行核心代码）；无 INF 依赖；即装即用 | 不符合 WDM 最佳实践；可能与其他过滤驱动冲突；需要自行管理 Hook 生命周期 |

当前选择 Hook 方案是因为项目处于原型阶段，追求最小可行实现。后续可考虑迁移为 minifilter 或 legacy filter。

### 3. 为什么 CBT 数据结构推荐 RTL_BITMAP 而非 SLIST？

| 维度 | RTL_BITMAP | SLIST |
|------|-----------|-------|
| 内存占用 | 固定：1TB 磁盘 @64KB = **2 MB** | 不确定：取决于写入频率 |
| 生产安全性 | ✅ 不会 OOM | ⚠️ 高 IOPS 下必 OOM |
| 去重能力 | ✅ 同块写 N 次仍为 1 bit | ❌ N 次 = N 个节点 |
| 增量备份查询 | ✅ O(n) 区间扫描 | ❌ O(N) 全链表遍历 + 手动合并 |
| 行业标准 | Veeam、VMware、VSS | 仅用于审计/调试场景 |

---

## 编译与部署

### 前置条件

- Visual Studio + WDK (Windows Driver Kit)
- 目标平台：Windows x64 (Server 2016+)

### 安装

```bash
# 创建服务
sc create go2cbt type= kernel binPath= "C:\path\to\go2cbt.sys"

# 启动服务 (加载驱动)
sc start go2cbt

# 查看日志 (DebugView / WinDbg)
# 过滤字符串: CBT
```

### 卸载

```bash
# 停止服务 (触发 CbtUnload → 恢复 Hook + 清理资源)
sc stop go2cbt

# 删除服务
sc delete go2cbt
```

---

## 已知限制

1. **仅支持 PASSIVE_LEVEL 查询分区信息**：`QueryPartitionInfoEx` 包含 IRQL 检查，高 IRQL 下优雅拒绝而非崩溃
2. **全局表无锁保护**：`g_DiskMap[]` 和 `g_HookList[]` 当前未加自旋锁，多核并发访问存在理论风险（低概率）
3. **最大容量**：最多支持 8 块物理磁盘 × 16 分区 = 128 个设备对象
4. **CBT 记录功能待实现**：`write.c:83-85` 中 CBT 位图标记为 TODO，当前仅输出 KdPrint 调试日志
5. **不支持热插拔磁盘**：仅在 `DriverEntry` 时刻枚举磁盘，运行期间新增磁盘不会自动注册

---

## 版本历史

| 版本 | 变更 |
|------|------|
| v0.1 | 初始版本；基础 Hook 框架 |
| v0.2 | 修复 NULL 指针 BSOD (IoCreateDevice 缺失)；修正初始化顺序 |
| v0.3 | 修复无限递归风险；添加 Unknown 设备安全透传；完善 Unload 清理流程 |
| v0.4 | 添加 QueryPartitionInfoEx 获取真实分区偏移和大小；确认 Part0 完整覆盖所有写入 |
