# go2cbt 测试文档

## 概述

`tests` 项目是 go2cbt 客户端库的单元测试和集成测试套件，使用自包含的轻量测试框架（`test_framework.h`），**零外部依赖**，兼容 Google Test 命名风格，后续可平滑迁移到 GTest。

## 快速开始

### 构建

```bash
# 命令行 (MSBuild)
msbuild tests/tests.vcxproj -p:Configuration=Debug -p:Platform=x64 -t:Build

# 或在 Visual Studio 中打开 go2cbt.sln，生成 tests 项目
```

产物：`tests\x64\Debug\tests.exe`

### 运行

```bash
tests\x64\Debug\tests.exe
```

输出示例：

```
============================================================
  go2cbt Test Suite — 96 test(s) registered
============================================================

[Logger]
  [PASS] Singleton_ReturnsSameInstance
  [PASS] Initialize_WithValidPath_Succeeds
  [PASS] SetMinLevel_And_GetMinLevel_RoundTrip
  ...

[BlockHasher]
  [PASS] Initialize_Succeeds
  ...

============================================================
  Results: 96 passed, 0 failed, 0 skipped
============================================================
```

## 项目结构

```
tests/
├── README.md                  # 本文档
├── tests.vcxproj              # MSBuild 项目文件（引用 ../client/ 源码）
├── test_framework.h           # 轻量测试框架（零依赖）
├── test_main.cpp              # 入口 + 日志初始化
├── test_Logger.cpp            # Logger 模块测试
├── test_BlockHasher.cpp       # BlockHasher 模块测试
├── test_DataCompressor.cpp    # DataCompressor 模块测试
├── test_BlockStateManager.cpp # BlockStateManager 模块测试
└── test_DiskParser.cpp        # DiskParser 模块测试
```

## 测试框架 API

### 测试定义

```cpp
TEST(SuiteName, TestName)
{
    // 测试代码
}
```

### 断言宏

| 宏 | 说明 | 失败行为 |
|----|------|----------|
| `ASSERT_TRUE(expr)` | 表达式为真 | 抛出异常，终止当前测试 |
| `ASSERT_FALSE(expr)` | 表达式为假 | 同上 |
| `ASSERT_EQ(a, b)` | a == b | 同上 |
| `ASSERT_NE(a, b)` | a != b | 同上 |
| `ASSERT_GT(a, b)` | a > b | 同上 |
| `ASSERT_LT(a, b)` | a < b | 同上 |
| `ASSERT_LE(a, b)` | a <= b | 同上 |
| `ASSERT_GE(a, b)` | a >= b | 同上 |
| `ASSERT_STREQ(a, b)` | 宽字符串相等（wcscmp） | 同上 |
| `ASSERT_STR_EQ(a, b)` | 窄字符串相等（strcmp） | 同上 |
| `ASSERT_MEMEQ(a, b, n)` | 内存块相等（memcmp） | 同上 |
| `ASSERT_NO_THROW(expr)` | 表达式不抛异常 | 同上 |
| `EXPECT_TRUE(expr)` | 表达式为真 | 打印失败，继续执行 |
| `EXPECT_EQ(a, b)` | a == b | 同上 |
| `EXPECT_STREQ(a, b)` | 宽字符串相等 | 同上 |
| `EXPECT_MEMEQ(a, b, n)` | 内存块相等 | 同上 |

### 辅助工具

```cpp
// 生成指定大小的填充数据（可重现的伪随机）
std::vector<uint8_t> MakeTestData(size_t size, uint8_t seed = 0);

// 获取临时测试目录
std::wstring GetTempTestDir();

// 清理测试目录
void CleanupTestDir(const std::wstring& dir);
```

## 测试覆盖详情

### 1. Logger（12 个测试）

| 测试 | 覆盖内容 |
|------|----------|
| `Singleton_ReturnsSameInstance` | 单例模式：两次获取返回同一实例 |
| `Initialize_WithValidPath_Succeeds` | 日志文件创建和初始化 |
| `SetMinLevel_And_GetMinLevel_RoundTrip` | 日志级别的设置/获取 round-trip |
| `MinLevel_FiltersMessages` | 级别过滤：低于 minLevel 的消息被忽略 |
| `Write_AllLevels_NoCrash` | Debug / Info / Warning / Error 四个级别写入 |
| `Write_EmptyMessage_NoCrash` | 空消息边界条件 |
| `Write_LongMessage_NoCrash` | 长消息（4096 字符）边界条件 |
| `Write_UnicodeMessage_NoCrash` | Unicode / emoji 消息 |
| `Shutdown_And_Reinitialize` | 关闭后重新初始化 |
| `ThreadSafety_MultipleWriters_NoCrash` | 4 线程并发写入烟雾测试 |
| `LogLevel_EnumValues` | 枚举值顺序验证 |
| `Macros_NoCrash` | LOG_DEBUG / LOG_INFO / LOG_WARNING / LOG_ERROR 宏 |

### 2. BlockHasher（21 个测试）

| 测试 | 覆盖内容 |
|------|----------|
| `Initialize_Succeeds` | BCrypt SHA-256 provider 初始化 |
| `Initialize_Idempotent` | 重复初始化无副作用 |
| `ComputeBlockHash_WithoutInit_Fails` | 未初始化时调用失败 |
| `ComputeBlockHash_EmptyData` | 空数据哈希（边界条件） |
| `ComputeBlockHash_SmallData` | 小数据哈希 |
| `ComputeBlockHash_OneMB` | 完整 1MB 块哈希 |
| `ComputeBlockHash_Deterministic` | 相同输入 → 相同哈希（幂等性） |
| `ComputeBlockHash_DifferentInput_DifferentHash` | 不同输入 → 不同哈希（碰撞抗性） |
| `HashToHex_ProducesCorrectFormat` | 十六进制格式验证 |
| `HashToHex_AllZeros` | 全零哈希值转 hex |
| `HashToHex_AllFFs` | 全 FF 哈希值转 hex |
| `BuildManifest_InvalidHandle_Fails` | 无效句柄错误处理 |
| `BuildManifest_SingleBlock_TempFile` | 从临时文件构建单块清单 |
| `SerializeDeserialize_RoundTrip` | 清单序列化/反序列化 round-trip |
| `SerializeManifest_Empty` | 空清单序列化 |
| `DeserializeManifest_InvalidData_Fails` | 无效数据反序列化失败 |
| `CompareManifests_Identical_NoChanges` | 相同清单对比 → 无变化 |
| `CompareManifests_DifferentHash_DetectsChange` | 不同哈希 → 检测到变化 |
| `CompareManifests_NewBlocks_Detected` | 新增块 → 标记为变化 |
| `ReadAt_PartialBlock_LastBlock` | 末尾块不足 1MB 的处理 |
| `GetLastError_BeforeInit_NotEmpty` | 未初始化时的错误消息 |

### 3. DataCompressor（11 个测试）

| 测试 | 覆盖内容 |
|------|----------|
| `Initialize_Succeeds` | MSZIP 压缩器初始化 |
| `Initialize_Idempotent` | 重复初始化无副作用 |
| `Compress_WithoutInit_Fails` | 未初始化时压缩失败（**发现 bug：构造函数缺失**） |
| `CompressDecompress_RoundTrip_SmallData` | 1KB 数据压缩解压 round-trip |
| `CompressDecompress_RoundTrip_OneMB` | 1MB 数据压缩解压 round-trip |
| `Compress_RepeatedData_CompressesWell` | 重复数据压缩比验证（>10:1） |
| `Compress_CompressedSize_LessThanMax` | 压缩后大小不超过最大可能值 |
| `Decompress_InvalidData_Fails` | 无效压缩数据解压失败 |
| `Decompress_WrongOriginalSize_Fails` | 错误的原始大小参数导致解压失败 |
| `GetMaxCompressedSize_GreaterThanInput` | 最大压缩大小 ≥ 原始大小 |
| `Shutdown_NoCrash` | 关闭压缩器（可重复调用） |

### 4. BlockStateManager（31 个测试）

| 测试 | 覆盖内容 |
|------|----------|
| `Construct_Defaults` | 默认构造：IsInitialized=false, DevNo=-1, TotalBlocks=0 |
| `Initialize_NewState_Succeeds` | 新状态文件初始化 |
| `Initialize_CreatesDirectory` | 自动创建状态目录 |
| `CreateVersion_FirstVersion_ReturnsVersion0` | 首个版本 ID 为 0 |
| `CreateVersion_MultipleVersions_Increments` | 连续创建版本 ID 自增 |
| `GetVersionHistory_ReturnsSorted` | 版本历史按 ID 升序 |
| `GetVersion_Existing_ReturnsCorrect` | 获取存在的版本 |
| `GetVersion_NonExisting_ReturnsEmpty` | 获取不存在的版本返回空 |
| `InitFullBlocks_InitializesAllBlocks` | 全量备份块初始化：全部 Pending+Changed |
| `InitFullBlocks_WrongVersionId_Fails` | 错误的版本 ID 初始化失败 |
| `InitFullBlocks_ZeroBlocks` | 零块初始化（边界条件） |
| `SetBlockAck_SingleBlock_Transitions` | 单块 ACK 状态转换（Pending→Acked/Failed/Skipped） |
| `SetBlockAck_OutOfRange_Fails` | 越界 ACK 失败 |
| `SetBlockAckRange_BatchUpdate` | 批量 ACK 范围更新 |
| `SetBlockAckList_NonContiguous` | 非连续块批量 ACK |
| `GetBlockState_ValidIndex` | 有效索引获取块状态 |
| `GetBlockState_OutOfRange_ReturnsEmpty` | 越界索引返回空状态 |
| `GetBlocksByAck_FiltersCorrectly` | 按 ACK 状态过滤 |
| `GetPendingBlocks_OnlyReturnsNeedsUpload` | 仅返回 Changed && !Acked 的块 |
| `GetBlockHistory_SingleVersion` | 单版本块历史 |
| `GetLastAcknowledgedBlock_Contiguous` | 连续 ACK 断点计算 |
| `GetLastAcknowledgedBlock_Gap` | 非连续 ACK 断点计算 |
| `SaveLoad_RoundTrip_PreservesData` | 持久化 Save/Load round-trip |
| `SaveLoad_MultipleVersions` | 多版本持久化 round-trip |
| `UpdateIncrementalBlocks_SubsetChanged` | 增量备份：部分块变化 |
| `UpdateIncrementalBlocks_CountMismatch_Fails` | 索引/哈希数量不匹配失败 |
| `UpdateIncrementalBlocks_EmptyBlocks_Fails` | 空块表增量更新失败 |
| `Save_WithoutInit_Fails` | 未初始化时 Save 失败 |
| `Save_EmptyState_Succeeds` | 空状态 Save（边界条件） |
| `Destructor_AutoSave_DirtyState` | 析构函数自动保存脏状态 |
| `VersionRecord_Progress_Calculates` | 进度百分比计算 |

### 5. DiskParser（21 个测试）

| 测试 | 覆盖内容 |
|------|----------|
| `MBRPartitionEntry_Size_16` | MBR 分区表项大小 = 16 字节 |
| `GPTHeader_Size_92` | GPT 头部大小 = 92 字节 |
| `GPTEntry_Size_128` | GPT 分区表项大小 = 128 字节 |
| `PartitionContent_EnumValues_Valid` | 分区内容枚举值 |
| `PartitionStyle_EnumValues_Valid` | 分区类型枚举值 |
| `DiskLayout_DefaultConstruction` | DiskLayout 默认构造 |
| `PartitionInfo_Defaults` | PartitionInfo 默认构造 |
| `Construct_Destruct_NoCrash` | 构造/析构无崩溃 |
| `Open_InvalidDisk_Fails` | 打开不存在的磁盘失败 |
| `Open_Close_MultipleCall_NoCrash` | 重复 Open/Close |
| `Parse_NotOpen_Fails` | 未打开时 Parse 失败 |
| `MBR_Signature_Bytes` | MBR 0x55AA 签名验证 |
| `GPT_EFI_PART_Signature_Magic` | "EFI PART" 签名字符串 |
| `DiskRange_Assignment` | DiskRange 赋值 |
| `DiskInfo_GPTInfo_Members` | GPTInfo 结构体成员 |
| `GPT_TypeGuids_KnownValues` | 已知 GPT 类型 GUID 验证 |
| `MBR_PartitionTypes_KnownValues` | 已知 MBR 分区类型验证 |
| `LBA_To_Offset_Calculation` | LBA→字节偏移计算 |
| `MBRPartitionEntry_FieldOffsets` | MBR 分区表项字段偏移量 |
| `GPTEntry_FieldOffsets` | GPT 分区表项字段偏移量 |
| `GPTHeader_FieldOffsets` | GPT 头部字段偏移量 |

## 迁移到 Google Test

测试框架 API 兼容 Google Test，迁移步骤：

1. 在 `tests.vcxproj` 中添加 Google Test NuGet 包引用
2. 替换 `#include "test_framework.h"` → `#include <gtest/gtest.h>`
3. 删除 `test_main.cpp`，使用 GTest 的 `main()`
4. 宏自动兼容：`TEST()` / `ASSERT_EQ()` / `EXPECT_TRUE()` 等无需修改

## 编写新测试

```cpp
#include "test_framework.h"
#include "../client/YourModule.h"

TEST(YourModule, YourTestName)
{
    // Arrange
    YourModule obj;
    obj.Initialize();

    // Act
    bool result = obj.DoSomething();

    // Assert
    ASSERT_TRUE(result);
}

// 边界条件
TEST(YourModule, EdgeCase_EmptyInput)
{
    YourModule obj;
    ASSERT_FALSE(obj.DoSomething(nullptr));  // 预期失败
}
```

## 日志级别控制

测试默认使用 `LogLevel::Debug`（输出所有日志到 `test_output.log` 文件）。如需在测试中抑制日志：

```cpp
Logger::Instance().SetMinLevel(LogLevel::Warning);  // 仅输出 Warning 和 Error
```

## 已知问题

1. **BlockStateManager Save/Load 测试** — `SaveLoad_RoundTrip_PreservesData`、`SaveLoad_MultipleVersions`、`Destructor_AutoSave_DirtyState` 三个测试在 `CleanupTestDir` 与析构函数时序上有竞争条件。`CleanupTestDir` 在块作用域结束前删除了临时目录，而 `BlockStateManager` 析构函数在作用域结束时触发 `Save()`，此时目录已不存在。
   - **临时方案**：将 `CleanupTestDir` 移到所有 `BlockStateManager` 对象析构之后
   - **根本方案**：使用 RAII guard 自动管理临时目录生命周期

2. **DiskScanner / VolumeMapper / VssManager / CbtClient / NetworkClient / BackupEngine** — 这些模块需要真实系统资源（管理员权限、物理磁盘、VSS 服务、网络连接、驱动程序），暂未编写单元测试。计划为以下模块添加 mock 测试或用条件编译跳过需要系统权限的测试：
   - `DiskScanner` — mock `DeviceIoControl` 返回
   - `CbtClient` — mock `CreateFileW`/`DeviceIoControl`
   - `VolumeMapper` — mock `FindFirstVolumeW`/`GetVolumeDiskExtents`
   - `BackupEngine` — 集成测试（dryrun 模式）
