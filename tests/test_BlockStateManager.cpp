// BlockStateManager 模块单元测试
// 覆盖：初始化、版本管理、ACK 状态转换、持久化 round-trip、查询、边界条件
#include "test_framework.h"
#include "../client/BlockStateManager.h"
#include "../client/BlockHasher.h"
#include "../client/Logger.h"

using namespace BlockState;

// 辅助：创建测试用 HashManifest
static BlockHash::HashManifest MakeTestManifest(uint64_t numBlocks)
{
	BlockHash::HashManifest manifest;
	manifest.BlockSize = 1024 * 1024;
	manifest.TotalBlocks = numBlocks;
	manifest.TotalSize = numBlocks * 1024 * 1024;
	manifest.VolumePath = L"test";

	for (uint64_t i = 0; i < numBlocks; i++)
	{
		BlockHash::BlockHashEntry e;
		e.BlockIndex = i;
		e.Offset = i * 1024 * 1024;
		memset(e.Hash, (uint8_t)(i & 0xFF), 32);
		manifest.Entries.push_back(e);
	}
	return manifest;
}

// ============================================================
// 初始化
// ============================================================
TEST(BlockStateManager, Construct_Defaults)
{
	BlockStateManager mgr;
	ASSERT_FALSE(mgr.IsInitialized());
	ASSERT_EQ(mgr.GetDevNo(), -1);
	ASSERT_EQ(mgr.GetTotalBlocks(), 0ULL);
}

TEST(BlockStateManager, Initialize_NewState_Succeeds)
{
	BlockStateManager mgr;
	auto tempDir = GetTempTestDir();
	bool ok = mgr.Initialize(tempDir, 0);
	ASSERT_TRUE(ok);
	ASSERT_TRUE(mgr.IsInitialized());
	ASSERT_EQ(mgr.GetDevNo(), 0);

	CleanupTestDir(tempDir);
}

TEST(BlockStateManager, Initialize_CreatesDirectory)
{
	BlockStateManager mgr;
	auto tempDir = GetTempTestDir() + L"subdir\\";
	bool ok = mgr.Initialize(tempDir, 1);
	ASSERT_TRUE(ok);

	// 验证目录已创建
	DWORD attr = GetFileAttributesW(tempDir.c_str());
	ASSERT_TRUE(attr != INVALID_FILE_ATTRIBUTES);
	ASSERT_TRUE((attr & FILE_ATTRIBUTE_DIRECTORY) != 0);

	CleanupTestDir(tempDir);
}

// ============================================================
// 版本管理
// ============================================================
TEST(BlockStateManager, CreateVersion_FirstVersion_ReturnsVersion0)
{
	BlockStateManager mgr;
	auto tempDir = GetTempTestDir();
	mgr.Initialize(tempDir, 0);

	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);

	auto ver = mgr.CreateVersion(L"FULL", ft, 100, 100 * 1024 * 1024);
	ASSERT_EQ(ver.VersionId, 0ULL);
	ASSERT_EQ(ver.DevNo, 0);
	ASSERT_EQ(ver.TotalBlocks, 100ULL);
	ASSERT_EQ(ver.TotalSize, 100ULL * 1024 * 1024);
	ASSERT_STREQ(ver.VersionType.c_str(), L"FULL");
	ASSERT_EQ(ver.ChangedBlocks, 0ULL);
	ASSERT_EQ(ver.AckedBlocks, 0ULL);

	CleanupTestDir(tempDir);
}

TEST(BlockStateManager, CreateVersion_MultipleVersions_Increments)
{
	BlockStateManager mgr;
	auto tempDir = GetTempTestDir();
	mgr.Initialize(tempDir, 0);

	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);

	auto v0 = mgr.CreateVersion(L"FULL", ft, 100, 100 * 1024 * 1024);
	auto v1 = mgr.CreateVersion(L"INCREMENTAL", ft, 100, 100 * 1024 * 1024);
	auto v2 = mgr.CreateVersion(L"INCREMENTAL", ft, 100, 100 * 1024 * 1024);

	ASSERT_EQ(v0.VersionId, 0ULL);
	ASSERT_EQ(v1.VersionId, 1ULL);
	ASSERT_EQ(v2.VersionId, 2ULL);

	CleanupTestDir(tempDir);
}

TEST(BlockStateManager, GetVersionHistory_ReturnsSorted)
{
	BlockStateManager mgr;
	auto tempDir = GetTempTestDir();
	mgr.Initialize(tempDir, 0);

	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);

	mgr.CreateVersion(L"FULL", ft, 10, 10 * 1024 * 1024);
	mgr.CreateVersion(L"INCREMENTAL", ft, 10, 10 * 1024 * 1024);

	auto history = mgr.GetVersionHistory();
	ASSERT_EQ((int)history.size(), 2);
	ASSERT_EQ(history[0].VersionId, 0ULL);
	ASSERT_EQ(history[1].VersionId, 1ULL);

	CleanupTestDir(tempDir);
}

TEST(BlockStateManager, GetVersion_Existing_ReturnsCorrect)
{
	BlockStateManager mgr;
	auto tempDir = GetTempTestDir();
	mgr.Initialize(tempDir, 0);

	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);

	auto created = mgr.CreateVersion(L"FULL", ft, 50, 50 * 1024 * 1024);
	auto retrieved = mgr.GetVersion(created.VersionId);

	ASSERT_EQ(retrieved.VersionId, created.VersionId);
	ASSERT_STREQ(retrieved.VersionType.c_str(), L"FULL");

	CleanupTestDir(tempDir);
}

TEST(BlockStateManager, GetVersion_NonExisting_ReturnsEmpty)
{
	BlockStateManager mgr;
	auto tempDir = GetTempTestDir();
	mgr.Initialize(tempDir, 0);

	auto ver = mgr.GetVersion(999);
	ASSERT_EQ(ver.VersionId, 0ULL);
	ASSERT_EQ(ver.DevNo, 0);

	CleanupTestDir(tempDir);
}

// ============================================================
// InitFullBlocks
// ============================================================
TEST(BlockStateManager, InitFullBlocks_InitializesAllBlocks)
{
	BlockStateManager mgr;
	auto tempDir = GetTempTestDir();
	mgr.Initialize(tempDir, 0);

	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);

	auto ver = mgr.CreateVersion(L"FULL", ft, 10, 10 * 1024 * 1024);
	auto manifest = MakeTestManifest(10);

	ASSERT_TRUE(mgr.InitFullBlocks(ver.VersionId, manifest));
	ASSERT_EQ(mgr.GetTotalBlocks(), 10ULL);

	// 所有块应该是 Pending + Changed
	auto pending = mgr.GetPendingBlocks();
	ASSERT_EQ((int)pending.size(), 10);

	// 第一个块验证
	auto bs = mgr.GetBlockState(0);
	ASSERT_TRUE(bs.Changed);
	ASSERT_EQ((int)bs.Ack, (int)AckStatus::Pending);

	CleanupTestDir(tempDir);
}

TEST(BlockStateManager, InitFullBlocks_WrongVersionId_Fails)
{
	BlockStateManager mgr;
	auto tempDir = GetTempTestDir();
	mgr.Initialize(tempDir, 0);

	auto manifest = MakeTestManifest(5);
	ASSERT_FALSE(mgr.InitFullBlocks(999, manifest));  // 不存在的版本

	CleanupTestDir(tempDir);
}

TEST(BlockStateManager, InitFullBlocks_ZeroBlocks)
{
	BlockStateManager mgr;
	auto tempDir = GetTempTestDir();
	mgr.Initialize(tempDir, 0);

	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);

	auto ver = mgr.CreateVersion(L"FULL", ft, 0, 0);
	auto manifest = MakeTestManifest(0);

	ASSERT_TRUE(mgr.InitFullBlocks(ver.VersionId, manifest));
	ASSERT_EQ(mgr.GetTotalBlocks(), 0ULL);

	auto pending = mgr.GetPendingBlocks();
	ASSERT_EQ((int)pending.size(), 0);

	CleanupTestDir(tempDir);
}

// ============================================================
// ACK 管理
// ============================================================
TEST(BlockStateManager, SetBlockAck_SingleBlock_Transitions)
{
	BlockStateManager mgr;
	auto tempDir = GetTempTestDir();
	mgr.Initialize(tempDir, 0);

	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);

	auto ver = mgr.CreateVersion(L"FULL", ft, 5, 5 * 1024 * 1024);
	mgr.InitFullBlocks(ver.VersionId, MakeTestManifest(5));

	// Pending → Acknowledged
	ASSERT_TRUE(mgr.SetBlockAck(0, AckStatus::Acknowledged));
	auto bs = mgr.GetBlockState(0);
	ASSERT_EQ((int)bs.Ack, (int)AckStatus::Acknowledged);

	// Pending → Failed
	ASSERT_TRUE(mgr.SetBlockAck(1, AckStatus::Failed));
	bs = mgr.GetBlockState(1);
	ASSERT_EQ((int)bs.Ack, (int)AckStatus::Failed);

	// Pending → Skipped
	ASSERT_TRUE(mgr.SetBlockAck(2, AckStatus::Skipped));
	bs = mgr.GetBlockState(2);
	ASSERT_EQ((int)bs.Ack, (int)AckStatus::Skipped);

	// 版本统计应更新
	auto updatedVer = mgr.GetVersion(ver.VersionId);
	ASSERT_EQ(updatedVer.AckedBlocks, 1ULL);

	CleanupTestDir(tempDir);
}

TEST(BlockStateManager, SetBlockAck_OutOfRange_Fails)
{
	BlockStateManager mgr;
	auto tempDir = GetTempTestDir();
	mgr.Initialize(tempDir, 0);

	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);

	auto ver = mgr.CreateVersion(L"FULL", ft, 5, 5 * 1024 * 1024);
	mgr.InitFullBlocks(ver.VersionId, MakeTestManifest(5));

	ASSERT_FALSE(mgr.SetBlockAck(100, AckStatus::Acknowledged));  // 越界

	CleanupTestDir(tempDir);
}

TEST(BlockStateManager, SetBlockAckRange_BatchUpdate)
{
	BlockStateManager mgr;
	auto tempDir = GetTempTestDir();
	mgr.Initialize(tempDir, 0);

	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);

	auto ver = mgr.CreateVersion(L"FULL", ft, 10, 10 * 1024 * 1024);
	mgr.InitFullBlocks(ver.VersionId, MakeTestManifest(10));

	// 批量标记前 5 个块为 ACK
	ASSERT_TRUE(mgr.SetBlockAckRange(0, 5, AckStatus::Acknowledged));

	auto updatedVer = mgr.GetVersion(ver.VersionId);
	ASSERT_EQ(updatedVer.AckedBlocks, 5ULL);

	// 验证每个块
	for (uint64_t i = 0; i < 5; i++)
	{
		auto bs = mgr.GetBlockState(i);
		ASSERT_EQ((int)bs.Ack, (int)AckStatus::Acknowledged);
	}
	for (uint64_t i = 5; i < 10; i++)
	{
		auto bs = mgr.GetBlockState(i);
		ASSERT_EQ((int)bs.Ack, (int)AckStatus::Pending);
	}

	CleanupTestDir(tempDir);
}

TEST(BlockStateManager, SetBlockAckList_NonContiguous)
{
	BlockStateManager mgr;
	auto tempDir = GetTempTestDir();
	mgr.Initialize(tempDir, 0);

	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);

	auto ver = mgr.CreateVersion(L"FULL", ft, 10, 10 * 1024 * 1024);
	mgr.InitFullBlocks(ver.VersionId, MakeTestManifest(10));

	std::vector<uint64_t> indices = { 0, 2, 4, 6, 8 };
	ASSERT_TRUE(mgr.SetBlockAckList(indices, AckStatus::Acknowledged));

	auto updatedVer = mgr.GetVersion(ver.VersionId);
	ASSERT_EQ(updatedVer.AckedBlocks, 5ULL);

	CleanupTestDir(tempDir);
}

// ============================================================
// 查询
// ============================================================
TEST(BlockStateManager, GetBlockState_ValidIndex)
{
	BlockStateManager mgr;
	auto tempDir = GetTempTestDir();
	mgr.Initialize(tempDir, 0);

	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);

	auto ver = mgr.CreateVersion(L"FULL", ft, 5, 5 * 1024 * 1024);
	mgr.InitFullBlocks(ver.VersionId, MakeTestManifest(5));

	auto bs = mgr.GetBlockState(3);
	ASSERT_EQ(bs.BlockIndex, 3ULL);
	ASSERT_EQ(bs.Offset, 3ULL * 1024 * 1024);
	ASSERT_TRUE(bs.Changed);

	CleanupTestDir(tempDir);
}

TEST(BlockStateManager, GetBlockState_OutOfRange_ReturnsEmpty)
{
	BlockStateManager mgr;
	auto tempDir = GetTempTestDir();
	mgr.Initialize(tempDir, 0);

	auto bs = mgr.GetBlockState(999);
	ASSERT_EQ(bs.BlockIndex, 0ULL);
	ASSERT_FALSE(bs.Changed);

	CleanupTestDir(tempDir);
}

TEST(BlockStateManager, GetBlocksByAck_FiltersCorrectly)
{
	BlockStateManager mgr;
	auto tempDir = GetTempTestDir();
	mgr.Initialize(tempDir, 0);

	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);

	auto ver = mgr.CreateVersion(L"FULL", ft, 10, 10 * 1024 * 1024);
	mgr.InitFullBlocks(ver.VersionId, MakeTestManifest(10));

	mgr.SetBlockAck(0, AckStatus::Acknowledged);
	mgr.SetBlockAck(1, AckStatus::Failed);
	mgr.SetBlockAck(2, AckStatus::Skipped);

	auto acked = mgr.GetBlocksByAck(AckStatus::Acknowledged);
	auto failed = mgr.GetBlocksByAck(AckStatus::Failed);
	auto skipped = mgr.GetBlocksByAck(AckStatus::Skipped);
	auto pending = mgr.GetBlocksByAck(AckStatus::Pending);

	ASSERT_EQ((int)acked.size(), 1);
	ASSERT_EQ((int)failed.size(), 1);
	ASSERT_EQ((int)skipped.size(), 1);
	ASSERT_EQ((int)pending.size(), 7);  // 10 - 3 = 7

	CleanupTestDir(tempDir);
}

TEST(BlockStateManager, GetPendingBlocks_OnlyReturnsNeedsUpload)
{
	BlockStateManager mgr;
	auto tempDir = GetTempTestDir();
	mgr.Initialize(tempDir, 0);

	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);

	auto ver = mgr.CreateVersion(L"FULL", ft, 5, 5 * 1024 * 1024);
	mgr.InitFullBlocks(ver.VersionId, MakeTestManifest(5));

	// ACK 2 个，应该只剩 3 个 pending
	mgr.SetBlockAck(0, AckStatus::Acknowledged);
	mgr.SetBlockAck(1, AckStatus::Skipped);

	auto pending = mgr.GetPendingBlocks();
	ASSERT_EQ((int)pending.size(), 3);

	CleanupTestDir(tempDir);
}

TEST(BlockStateManager, GetBlockHistory_SingleVersion)
{
	BlockStateManager mgr;
	auto tempDir = GetTempTestDir();
	mgr.Initialize(tempDir, 0);

	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);

	auto ver = mgr.CreateVersion(L"FULL", ft, 5, 5 * 1024 * 1024);
	mgr.InitFullBlocks(ver.VersionId, MakeTestManifest(5));

	auto history = mgr.GetBlockHistory(0);
	ASSERT_EQ((int)history.size(), 1);
	ASSERT_EQ(history[0].BlockIndex, 0ULL);

	// 越界索引
	auto emptyHistory = mgr.GetBlockHistory(999);
	ASSERT_EQ((int)emptyHistory.size(), 0);

	CleanupTestDir(tempDir);
}

// ============================================================
// GetLastAcknowledgedBlock
// ============================================================
TEST(BlockStateManager, GetLastAcknowledgedBlock_Contiguous)
{
	BlockStateManager mgr;
	auto tempDir = GetTempTestDir();
	mgr.Initialize(tempDir, 0);

	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);

	auto ver = mgr.CreateVersion(L"FULL", ft, 10, 10 * 1024 * 1024);
	mgr.InitFullBlocks(ver.VersionId, MakeTestManifest(10));

	// ACK 前 5 个
	mgr.SetBlockAckRange(0, 5, AckStatus::Acknowledged);

	uint64_t lastAcked = mgr.GetLastAcknowledgedBlock();
	ASSERT_EQ(lastAcked, 5ULL);

	CleanupTestDir(tempDir);
}

TEST(BlockStateManager, GetLastAcknowledgedBlock_Gap)
{
	BlockStateManager mgr;
	auto tempDir = GetTempTestDir();
	mgr.Initialize(tempDir, 0);

	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);

	auto ver = mgr.CreateVersion(L"FULL", ft, 10, 10 * 1024 * 1024);
	mgr.InitFullBlocks(ver.VersionId, MakeTestManifest(10));

	// ACK 0,1,2 然后跳过 3，ACK 4
	mgr.SetBlockAck(0, AckStatus::Acknowledged);
	mgr.SetBlockAck(1, AckStatus::Acknowledged);
	mgr.SetBlockAck(2, AckStatus::Acknowledged);
	mgr.SetBlockAck(4, AckStatus::Acknowledged);

	uint64_t lastAcked = mgr.GetLastAcknowledgedBlock();
	ASSERT_EQ(lastAcked, 3ULL);  // 连续到 3（即索引 0,1,2 = 3 个块）

	CleanupTestDir(tempDir);
}

// ============================================================
// 持久化 — Save / Load round-trip
// ============================================================
TEST(BlockStateManager, SaveLoad_RoundTrip_PreservesData)
{
	auto tempDir = GetTempTestDir();

	// 创建并填充
	{
		BlockStateManager mgr;
		mgr.Initialize(tempDir, 99);

		FILETIME ft;
		GetSystemTimeAsFileTime(&ft);

		auto ver = mgr.CreateVersion(L"FULL", ft, 5, 5 * 1024 * 1024);
		mgr.InitFullBlocks(ver.VersionId, MakeTestManifest(5));
		mgr.SetBlockAck(0, AckStatus::Acknowledged);
		mgr.SetBlockAck(1, AckStatus::Acknowledged);
		mgr.SetBlockAck(2, AckStatus::Failed);

		ASSERT_TRUE(mgr.Save());
	}

	// 重新加载
	{
		BlockStateManager mgr2;
		mgr2.Initialize(tempDir, 99);

		ASSERT_EQ(mgr2.GetTotalBlocks(), 5ULL);
		ASSERT_EQ(mgr2.GetDevNo(), 99);

		auto history = mgr2.GetVersionHistory();
		ASSERT_EQ((int)history.size(), 1);

		auto bs0 = mgr2.GetBlockState(0);
		ASSERT_EQ((int)bs0.Ack, (int)AckStatus::Acknowledged);

		auto bs2 = mgr2.GetBlockState(2);
		ASSERT_EQ((int)bs2.Ack, (int)AckStatus::Failed);

		auto pending = mgr2.GetPendingBlocks();
		ASSERT_EQ((int)pending.size(), 3);  // 5 - 2 acked = 3
	}

	CleanupTestDir(tempDir);
}

TEST(BlockStateManager, SaveLoad_MultipleVersions)
{
	auto tempDir = GetTempTestDir();

	{
		BlockStateManager mgr;
		mgr.Initialize(tempDir, 0);

		FILETIME ft;
		GetSystemTimeAsFileTime(&ft);

		auto v0 = mgr.CreateVersion(L"FULL", ft, 3, 3 * 1024 * 1024);
		mgr.InitFullBlocks(v0.VersionId, MakeTestManifest(3));

		auto v1 = mgr.CreateVersion(L"INCREMENTAL", ft, 3, 3 * 1024 * 1024);

		ASSERT_TRUE(mgr.Save());
	}

	{
		BlockStateManager mgr2;
		mgr2.Initialize(tempDir, 0);

		auto history = mgr2.GetVersionHistory();
		ASSERT_EQ((int)history.size(), 2);
		ASSERT_STREQ(history[0].VersionType.c_str(), L"FULL");
		ASSERT_STREQ(history[1].VersionType.c_str(), L"INCREMENTAL");
	}

	CleanupTestDir(tempDir);
}

// ============================================================
// UpdateIncrementalBlocks
// ============================================================
TEST(BlockStateManager, UpdateIncrementalBlocks_SubsetChanged)
{
	BlockStateManager mgr;
	auto tempDir = GetTempTestDir();
	mgr.Initialize(tempDir, 0);

	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);

	auto v0 = mgr.CreateVersion(L"FULL", ft, 5, 5 * 1024 * 1024);
	mgr.InitFullBlocks(v0.VersionId, MakeTestManifest(5));

	auto v1 = mgr.CreateVersion(L"INCREMENTAL", ft, 5, 5 * 1024 * 1024);

	// 只有块 1 和 3 变化
	std::vector<uint64_t> changedIdx = { 1, 3 };
	std::vector<BlockHash::BlockHashEntry> hashes;
	for (auto idx : changedIdx)
	{
		BlockHash::BlockHashEntry e;
		e.BlockIndex = idx;
		e.Offset = idx * 1024 * 1024;
		memset(e.Hash, 0xFF, 32);
		hashes.push_back(e);
	}

	ASSERT_TRUE(mgr.UpdateIncrementalBlocks(v1.VersionId, changedIdx, hashes));

	// 验证变化块
	auto bs1 = mgr.GetBlockState(1);
	ASSERT_TRUE(bs1.Changed);
	ASSERT_EQ((int)bs1.Ack, (int)AckStatus::Pending);

	auto bs3 = mgr.GetBlockState(3);
	ASSERT_TRUE(bs3.Changed);

	// 验证未变化块
	auto bs0 = mgr.GetBlockState(0);
	ASSERT_FALSE(bs0.Changed);
	ASSERT_EQ((int)bs0.Ack, (int)AckStatus::Skipped);

	auto bs2 = mgr.GetBlockState(2);
	ASSERT_FALSE(bs2.Changed);
	ASSERT_EQ((int)bs2.Ack, (int)AckStatus::Skipped);

	// 版本统计
	auto ver = mgr.GetVersion(v1.VersionId);
	ASSERT_EQ(ver.ChangedBlocks, 2ULL);

	CleanupTestDir(tempDir);
}

TEST(BlockStateManager, UpdateIncrementalBlocks_CountMismatch_Fails)
{
	BlockStateManager mgr;
	auto tempDir = GetTempTestDir();
	mgr.Initialize(tempDir, 0);

	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);

	auto v0 = mgr.CreateVersion(L"FULL", ft, 5, 5 * 1024 * 1024);
	mgr.InitFullBlocks(v0.VersionId, MakeTestManifest(5));

	auto v1 = mgr.CreateVersion(L"INCREMENTAL", ft, 5, 5 * 1024 * 1024);

	std::vector<uint64_t> changedIdx = { 1, 2 };
	std::vector<BlockHash::BlockHashEntry> hashes;  // 空！不匹配
	ASSERT_FALSE(mgr.UpdateIncrementalBlocks(v1.VersionId, changedIdx, hashes));

	CleanupTestDir(tempDir);
}

TEST(BlockStateManager, UpdateIncrementalBlocks_EmptyBlocks_Fails)
{
	BlockStateManager mgr;
	auto tempDir = GetTempTestDir();
	mgr.Initialize(tempDir, 0);

	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);

	auto ver = mgr.CreateVersion(L"INCREMENTAL", ft, 5, 5 * 1024 * 1024);
	// 没有先调用 InitFullBlocks

	std::vector<uint64_t> changedIdx = { 1 };
	std::vector<BlockHash::BlockHashEntry> hashes;
	BlockHash::BlockHashEntry e;
	e.BlockIndex = 1;
	e.Offset = 1024 * 1024;
	memset(e.Hash, 0xFF, 32);
	hashes.push_back(e);

	ASSERT_FALSE(mgr.UpdateIncrementalBlocks(ver.VersionId, changedIdx, hashes));

	CleanupTestDir(tempDir);
}

// ============================================================
// Save — atomic write
// ============================================================
TEST(BlockStateManager, Save_WithoutInit_Fails)
{
	BlockStateManager mgr;
	ASSERT_FALSE(mgr.Save());
}

TEST(BlockStateManager, Save_EmptyState_Succeeds)
{
	auto tempDir = GetTempTestDir();
	BlockStateManager mgr;
	mgr.Initialize(tempDir, 0);

	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);
	mgr.CreateVersion(L"FULL", ft, 0, 0);

	ASSERT_TRUE(mgr.Save());

	CleanupTestDir(tempDir);
}

// ============================================================
// destructor — auto-Save
// ============================================================
TEST(BlockStateManager, Destructor_AutoSave_DirtyState)
{
	auto tempDir = GetTempTestDir();

	{
		BlockStateManager mgr;
		mgr.Initialize(tempDir, 5);

		FILETIME ft;
		GetSystemTimeAsFileTime(&ft);

		auto ver = mgr.CreateVersion(L"FULL", ft, 3, 3 * 1024 * 1024);
		mgr.InitFullBlocks(ver.VersionId, MakeTestManifest(3));
		// 不显式调用 Save()，析构函数应自动保存
	}

	// 重新加载验证
	{
		BlockStateManager mgr2;
		mgr2.Initialize(tempDir, 5);
		ASSERT_EQ(mgr2.GetTotalBlocks(), 3ULL);
	}

	CleanupTestDir(tempDir);
}

// ============================================================
// VersionRecord::Progress()
// ============================================================
TEST(BlockStateManager, VersionRecord_Progress_Calculates)
{
	VersionRecord ver;
	ver.TotalBlocks = 100;
	ver.AckedBlocks = 50;
	double pct = ver.Progress();
	ASSERT_GT(pct, 49.9);
	ASSERT_LT(pct, 50.1);

	ver.AckedBlocks = 0;
	ASSERT_EQ(ver.Progress(), 0.0);

	ver.TotalBlocks = 0;
	ASSERT_EQ(ver.Progress(), 0.0);
}
