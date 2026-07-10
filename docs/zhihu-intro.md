# 写了一个 Windows 内核级的磁盘变更追踪工具 (go2cbt)

> **项目地址**：[go2cbt](https://github.com/lijingwei9060/go2cbt)
>
> **当前阶段**：早期原型 / 可运行 Demo，距离生产可用还有不少路要走。
>
> **适用场景**：增量备份、实时同步等需要精确感知"磁盘哪些块被写过"的场景。

---

## 这东西是干什么的？

简单说：**它是一个 Windows 内核驱动，能在底层实时记录每一块被写入过的磁盘数据。**

为什么要做这个？
- 做增量备份时，最核心的问题就是——**怎么知道哪些数据变了？**
- 做windows机器的迁移的时候，使用vss做一次全盘备份，增量的数据如何查找？ 看了腾讯云的windows 在线迁移，有个前提是关闭业务，多次遍历磁盘，使用hash计算变化量，理论行。
- 看到ai，感觉自己又行了。其实搞了快两周，没有驱动开发基础，主要使用了chatgpt、workbuddy，都出现了带到死胡同，一个小问题兜兜转转搞了很久。不停的pua这些ai，chatgpt具备较强的反pua机制，拒绝道歉，都是你的错，他没有错。
- 使用workbuddy分析了华为云的sms-agent，它竟然直接分析出来了，而且给出了证据。真的感觉自己又可以搏一把。

go2cbt 尝试做的事情，按我的理解和商业备份软件底层的 CBT（Changed Block Tracking）机制类似，有时间和兴趣在分析一下这个差异。

有什么优势吗？
- 安装这个驱动，绝对不能要求重启，这是前提。标准的开发system 驱动、driver、volume 驱动，都会有问题，会要求重启。只能捕捉到pnp的一些请求，直接磁盘IO相关的Write IRP 捕捉不到。 workbuddy和chatgpt两个家伙在这上面开始打架。chatgpt认为我debug不够深入，没找到问题。workbuddy一语道破，这些驱动要求在开机加载的时候才能生效， chatgpt不承认。
- 使用`InterlockedExchangePointer`劫持原有驱动的`IRP_MJ_WRITE`函数，start的时候走本驱动，跟踪offset跟新bitmap。stop的时候替换会原有驱动。修改的内容非常小，蓝屏风险低。这个不会触发Windows的PatchGuard(KPP)。


PatchGuard 保护下面对象，会周期性进行CRC校验检查，发现被修改了会直接蓝屏死机:

- IDT (Interrupt Descriptor Table)
- GDT (Global Descriptor Table)
- SSDT (System Service Dispatch Table) - the KeServiceDescriptorTable
- Certain MSRs (Model Specific Registers) - like SYSENTER/SYSCALL MSR
- Kernel code sections (the .text sections of ntoskrnl.exe and hal.dll)
- Certain critical kernel data structures
- The kernel stack trace database
- KdpTraceCallTable (kernel debug trace table)
- Kernel image headers (PE headers of ntoskrnl)

---




## 工作原理

### 架构概览

```
备份软件 / 同步工具
        │
        ▼   DeviceIoControl
   ┌──────────┐
   │ cbtctl.exe│  ← 用户态控制程序（查询/重置/可视化）
   └──────────┘
        │
        ▼   \\.\CbtMonitor
   ┌──────────────────────┐
   │    go2cbt.sys        │  ← Windows WDM 内核驱动 (Ring 0)
   │                      │
   │  拦截 IRP_MJ_WRITE   │
   │  记录哪些块被写入了   │
   │  用位图标记变更区域   │
   └──────────┬───────────┘
              │  Hook 了 disk.sys 的 Write 分发函数
              ▼
   \Device\Harddisk0\Partition0   ← 只监控这里
```

### 几个关键设计决策

**1. 为什么只监控 Partition0？**

这个结论不是拍脑袋想的，而是从实际日志中发现的。在调试过程中，我发现一个有趣的现象：

> 每当某个分区（比如 C: 盘 = Partition3）有写入操作时，Partition0 上也会有**完全相同的 DiskOff 地址**的写入。

经过大量日志验证确认：**Partition0 接收到了整块物理磁盘的所有写入请求**。这意味着只需要监控 Partition0 一个设备，就能完整覆盖所有分区的变更，架构大大简化。

**2. 为什么用 RTL_BITMAP 而不是链表？**

最初考虑过用 SLIST（无锁链表）记录每条写入记录，但很快意识到问题：在高 IOPS 场景下，内存增长不可控。而位图的优点很明显：

- 内存固定（300GB 的盘只需要 ~38KB）
- 天然去重（同一块被写多次只占 1 个 bit）
- 查询高效（O(n) 扫描即可获取所有变更区间）

**3. 并发安全怎么做？**

用了 KSPIN_LOCK 自旋锁保护所有位图修改操作。这里踩过一个坑：一开始觉得 RtlSetBits 是单条指令级别的操作不需要锁，但仔细想了一下——**丢一次 set 就意味着少备份一块数据**，这对备份场景来说是不可接受的数据损坏风险。所以最终选择了保守但安全的全路径加锁策略，锁持有时间约 ~10ns，对 I/O 性能的影响可以忽略。

**4. 追踪精度如何权衡？**

默认每个追踪块大小为 1MB（`CBT_BLOCK_SIZE`），可以在头文件中调整。这是一个经典的**空间 vs 精度** trade-off：

| 块大小 | 300GB 盘的位图大小 | 增量备份的最小颗粒度 |
|--------|-------------------|---------------------|
| 64 KB  | ~576 KB           | 64 KB               |
| **1 MB** | **~38 KB**       | **1 MB**            |
| 16 MB  | ~2.4 KB           | 16 MB               |

1MB 是个比较折中的选择。

---

## 使用效果

查询命令输出示例：

```
=== Disk0 CBT Summary ===
  Total Bits (blocks):  307200
  Bitmap Bytes:         38400
  Block Size:            1024 KB
  Disk Approx Size:      292.97 GB
  Est. Change Rate:      3.52%
```

可视化 dump 输出（每行 64 块，`*` 代表已变更）：

```
[0000000000] 0x0000000000000000: 00000*** ****0*** ******** ********
[0000000064] 0x0000000000040000: ******** ******** ******** ***00000
...
```

配合密度直方图，一眼就能看出磁盘上哪些区域是热点：

```
CHANGE DENSITY MAP (64 segments):
. . o O # @ @ . . . . . . . . . . . . . . . . . . . . . .
Legend: .=<1%  :=<5%  o=<15%  O=<30%  #=<50%  @=>50%
```

---

## 目前能做到的

- ✅ 正确拦截磁盘 Write IRP 并记录到 RTL_BITMAP
- ✅ IOCTL 两步查询协议（支持任意大小的磁盘）
- ✅ 用户态工具支持 query / reset / dump 三条命令
- ✅ 可视化输出含统计信息和密度直方图
- ✅ KSPIN_LOCK 保护并发安全
- ✅ 正确的资源清理（卸载时不泄漏）

## 已知问题和局限

1. **仅支持 x64 平台** —— ARM64 配置存在但未经验证
2. **不支持热插拔磁盘** —— 启动时扫描并固定磁盘列表，运行中新插入的磁盘不会被自动监控
3. **不支持多路径存储 (MPIO)** —— 同一物理磁盘多路径暴露时可能产生重复追踪
4. **METHOD_BUFFERED 缓冲区大小限制** —— 对于超大磁盘（>32TB），位图可能超过单次 IOCTL 传输上限
5. **测试签名限制** —— 默认使用测试模式签名，需要 `bcdedit /set testsigning on`
6. **BSOD 风险** —— 这是内核驱动，代码缺陷可能导致蓝屏。请在虚拟机中先测试
7. **未经过长时间稳定性压测** —— 当前只在短时间的功能验证中使用过

**总结来说：它能跑起来，能完成基本的变更追踪功能，但要放到生产环境，还需要大量的测试和完善。**

---

## 关于开发过程

这个项目的开发过程**大量依赖了 AI 工具辅助**

### 使用的 AI 工具

- **ChatGPT** —— 大量的内核驱动概念咨询、API 使用方法确认、代码审查
- **WorkBuddy** —— 主要的结对编程伙伴，负责实际的代码编写、调试分析、编译错误修复
- **DeepSeek** —— 部分算法逻辑的讨论和备选方案的评估

它们帮助我做了很多事情：

- 从 BSOD 崩溃堆栈中定位到 `DriverEntry` 缺少 `IoCreateDevice` 调用
- 分析 WinDbg 日志确定 "Unknown Device" 的真实身份（partmgr.sys 的卷代理设备）
- 发现 Partition0 与各分区写入的 DiskOff 完全一致的规律
- 解决 METHOD_BUFFERED 模式下 `STATUS_BUFFER_TOO_SMALL` 数据不回传的问题（最终采用 `STATUS_BUFFER_OVERFLOW` 方案）
- 无数个编译警告修复（类型不匹配、废弃 API 替换等）

### 参考项目

设计上参考了华为 **SMS-Agent 迁移工具** 中的一些思路（特别是关于磁盘 I/O 层面的处理方式）。当然，go2cbt 的定位更聚焦——只做 CBT 变追一件事，不做迁移的其他部分。

---

## 未来可能的方向（如果有人感兴趣的话）

- [ ] 支持动态磁盘发现（热插拔）
- [ ] 支持可配置的追踪粒度（运行时可调，不需重新编译）
- [ ] 用户态通知机制（事件/回调，而非轮询）
- [ ] 多路径去重
- [ ] 超大磁盘的分页传输支持
- [ ] 正式代码签名 + WHQL 认证
- [ ] 更完善的测试套件（压力测试、 fuzzing 等）

---

*最后再说一句：这是一个学习性项目，还只是一个demo版本，代码质量、架构设计、工程完整性都还有很大的提升空间。欢迎fork、star和提出问题，谢谢各位老板！*

