# DiskParser 模块文档

## 概述

`DiskParser` 负责从物理磁盘原始读取 MBR/GPT 分区表，输出磁盘布局信息，并对每个分区进行内容检测和四分类标记。它是备份系统的磁盘分析入口。

**命名空间：** `Disk`  
**文件：** `client/DiskParser.h`、`client/DiskParser.cpp`

---

## 数据结构

### 枚举

| 枚举 | 值 | 说明 |
|------|-----|------|
| `PartitionStyle` | `Unknown` / `MBR` / `GPT` | 磁盘分区类型 |
| `PartitionContent` | `Unknown` / `FilesystemNTFS` / `FilesystemFAT32` / `FilesystemExFAT` / `FilesystemReFS` / `RawPartition` / `Reserved` | 分区内容四分类 |

### 结构体

```
DiskLayout
├── DiskInfo          磁盘基本信息
│   ├── DeviceNumber  PhysicalDrive 编号
│   ├── Size          磁盘总大小（字节）
│   ├── SectorSize    扇区大小（字节）
│   ├── Style         分区类型（MBR/GPT）
│   ├── GPT           GPT 元数据位置信息
│   └── MBR           MBR 签名
├── Partitions[]      分区列表
│   └── PartitionInfo
│       ├── Index / Offset / Size   分区位置
│       ├── MbrType / TypeGuid      分区类型原始值
│       ├── Content                 四分类结果
│       ├── IsEncrypted             BitLocker 标记
│       └── FsName                 文件系统名称
├── FreeRanges[]      未分配空间范围
└── MetadataRanges[]  磁盘元数据区域
```

---

## API

### 基本操作

```cpp
DiskParser parser;

// 打开物理磁盘（\\\\.\\PhysicalDriveN）
bool parser.Open(int deviceNumber);

// 解析磁盘布局
DiskLayout layout;
bool parser.Parse(layout);

// 关闭
void parser.Close();
```

### 内部方法

| 方法 | 功能 | IOCTL |
|------|------|-------|
| `QueryDiskSize()` | 查询磁盘总大小 | `IOCTL_DISK_GET_LENGTH_INFO` |
| `QuerySectorSize()` | 查询扇区大小（失败回退512） | `IOCTL_DISK_GET_DRIVE_GEOMETRY_EX` |
| `ParseMBR()` | 解析 MBR 分区表 | 原始扇区读取 LBA 0 |
| `ParseGPT()` | 解析 GPT Header + 分区表 | 原始扇区读取 LBA 1~33 |
| `CalculateFreeSpace()` | 计算未分配空间 | 排序分区，检测间隙 |
| `DetectPartitionContent()` | VBR 文件系统签名检测 | 读取分区第一个扇区 |
| `DetectPartitionContentGpt()` | GPT TypeGuid 预分类 | MSR/ESP 识别 |

---

## 分区内容检测逻辑

### 检测流程

```
分区条目（MBR Type 或 GPT TypeGuid）
    │
    ├── GPT: DetectPartitionContentGpt()
    │   ├── MSR (E3C9E316) → Content=Reserved，跳过 VBR
    │   └── ESP (C12A7328) → 预标 FsName="ESP"，继续检测
    │
    └── DetectPartitionContent()
        ├── 读取分区第一个扇区（VBR）
        ├── 检查加密: "-FVE-FS-" at 0x03 → BitLocker
        ├── 检查 NTFS: "NTFS    " at 0x03
        ├── 检查 ReFS: "ReFS" at 0x03
        ├── 检查 exFAT: "EXFAT   " at 0x03
        ├── 检查 FAT32: "FAT32   " at 0x52
        ├── 检查 FAT16: "FAT16   " at 0x36
        └── 无匹配 → RawPartition
```

### 备份策略对应

| PartitionContent | 备份策略 | 原因 |
|-----------------|----------|------|
| `FilesystemNTFS/FAT32/ExFAT/ReFS` | VSS 快照 | 需要应用级一致性 |
| `RawPartition` | 物理磁盘直接读取 | 无文件系统，VSS 不适用 |
| `Reserved` | 归入磁盘元数据 | MSR 等系统保留区域 |
| `FreeRanges` | 跳过 | 未分配空间不备份 |

---

## GPT 元数据区域

解析 GPT 磁盘时，自动记录 5 个元数据区域到 `MetadataRanges`：

| 区域 | 位置 |
|------|------|
| Protective MBR | LBA 0 |
| GPT Header | LBA 1 |
| 主分区表条目数组 | LBA 2 ~ 33 |
| 备份分区表条目数组 | 磁盘末尾 - (EntryCount × EntrySize) |
| 备份 GPT Header | 最后一个 LBA |

这些区域总量 < 2 MB，每次做全量备份即可，无需增量。

---

## 兼容性

| 项目 | 支持情况 |
|------|----------|
| MBR 磁盘 | ✅ |
| GPT 磁盘 | ✅ |
| 512B 扇区 | ✅（自动检测） |
| 4K Native 扇区 | ✅（IOCTL 查询后使用实际值） |
| 动态磁盘 | ❌ 不支持 |
| BitLocker 加密 | ✅ 检测并标记 |
| 管理员权限 | 需要（打开 PhysicalDrive） |

---

## 使用示例

```cpp
Disk::DiskParser parser;
Disk::DiskLayout layout;

if (parser.Open(0)) {
    if (parser.Parse(layout)) {
        // 遍历分区
        for (const auto& p : layout.Partitions) {
            // p.Content → 四分类标记
            // p.FsName → "NTFS", "FAT32", "Raw", "MSR" ...
            // p.IsEncrypted → BitLocker 标记
        }
        // 获取元数据区域
        for (const auto& m : layout.MetadataRanges) {
            // m.Offset, m.Size
        }
        // 获取未分配空间
        for (const auto& f : layout.FreeRanges) {
            // f.Offset, f.Size
        }
    }
    parser.Close();
}
```
