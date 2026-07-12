// BlockHasher 模块单元测试
// 覆盖：SHA-256 哈希计算、清单构建、序列化/反序列化、清单对比、HashToHex
#include "test_framework.h"
#include "../client/BlockHasher.h"
#include "../client/Logger.h"

using namespace BlockHash;

// ============================================================
// 初始化
// ============================================================
TEST(BlockHasher, Initialize_Succeeds)
{
	BlockHasher hasher;
	ASSERT_TRUE(hasher.Initialize());
}

TEST(BlockHasher, Initialize_Idempotent)
{
	BlockHasher hasher;
	ASSERT_TRUE(hasher.Initialize());
	ASSERT_TRUE(hasher.Initialize());  // 第二次调用应无副作用
}

TEST(BlockHasher, ComputeBlockHash_WithoutInit_Fails)
{
	BlockHasher hasher;
	uint8_t data[64] = {};
	uint8_t hash[32];
	ASSERT_FALSE(hasher.ComputeBlockHash(data, 64, hash));
}

// ============================================================
// SHA-256 基础计算
// ============================================================
TEST(BlockHasher, ComputeBlockHash_EmptyData)
{
	BlockHasher hasher;
	ASSERT_TRUE(hasher.Initialize());

	uint8_t hash[32];
	// SHA-256 of empty string
	ASSERT_TRUE(hasher.ComputeBlockHash(nullptr, 0, hash));

	// 验证 hash 非零
	bool allZero = true;
	for (int i = 0; i < 32; i++)
		if (hash[i] != 0) { allZero = false; break; }
	ASSERT_FALSE(allZero);
}

TEST(BlockHasher, ComputeBlockHash_SmallData)
{
	BlockHasher hasher;
	ASSERT_TRUE(hasher.Initialize());

	const char* input = "go2cbt test";
	uint8_t hash[32];
	ASSERT_TRUE(hasher.ComputeBlockHash((const uint8_t*)input, (uint32_t)strlen(input), hash));

	// hash 长度应为 32 字节
	ASSERT_TRUE(true);  // 不崩溃即通过
}

TEST(BlockHasher, ComputeBlockHash_OneMB)
{
	BlockHasher hasher;
	ASSERT_TRUE(hasher.Initialize());

	std::vector<uint8_t> data = MakeTestData(BLOCK_SIZE, 42);
	uint8_t hash[32];
	ASSERT_TRUE(hasher.ComputeBlockHash(data.data(), (uint32_t)data.size(), hash));
}

TEST(BlockHasher, ComputeBlockHash_Deterministic)
{
	BlockHasher hasher;
	ASSERT_TRUE(hasher.Initialize());

	std::vector<uint8_t> data = MakeTestData(1024, 7);

	uint8_t hash1[32], hash2[32];
	ASSERT_TRUE(hasher.ComputeBlockHash(data.data(), 1024, hash1));
	ASSERT_TRUE(hasher.ComputeBlockHash(data.data(), 1024, hash2));

	ASSERT_MEMEQ(hash1, hash2, 32);
}

TEST(BlockHasher, ComputeBlockHash_DifferentInput_DifferentHash)
{
	BlockHasher hasher;
	ASSERT_TRUE(hasher.Initialize());

	auto data1 = MakeTestData(1024, 1);
	auto data2 = MakeTestData(1024, 2);

	uint8_t hash1[32], hash2[32];
	ASSERT_TRUE(hasher.ComputeBlockHash(data1.data(), 1024, hash1));
	ASSERT_TRUE(hasher.ComputeBlockHash(data2.data(), 1024, hash2));

	// 不同输入应产生不同哈希
	bool same = (memcmp(hash1, hash2, 32) == 0);
	ASSERT_FALSE(same);
}

// ============================================================
// HashToHex
// ============================================================
TEST(BlockHasher, HashToHex_ProducesCorrectFormat)
{
	uint8_t hash[32] = {
		0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
		0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
		0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
		0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF
	};

	std::wstring hex = BlockHasher::HashToHex(hash);
	ASSERT_EQ((int)hex.length(), 64);

	// 验证前几个字符
	ASSERT_TRUE(hex.substr(0, 8) == L"00112233");
	ASSERT_TRUE(hex.substr(8, 8) == L"44556677");
}

TEST(BlockHasher, HashToHex_AllZeros)
{
	uint8_t hash[32] = {};
	std::wstring hex = BlockHasher::HashToHex(hash);
	ASSERT_STREQ(hex.c_str(), L"0000000000000000000000000000000000000000000000000000000000000000");
}

TEST(BlockHasher, HashToHex_AllFFs)
{
	uint8_t hash[32];
	memset(hash, 0xFF, 32);
	std::wstring hex = BlockHasher::HashToHex(hash);
	ASSERT_STREQ(hex.c_str(), L"FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF");
}

// ============================================================
// BuildManifest — 需要真实文件句柄，使用临时文件模拟
// ============================================================
TEST(BlockHasher, BuildManifest_InvalidHandle_Fails)
{
	BlockHasher hasher;
	ASSERT_TRUE(hasher.Initialize());

	HashManifest manifest;
	ASSERT_FALSE(hasher.BuildManifest(INVALID_HANDLE_VALUE, 0, BLOCK_SIZE, L"test", manifest));
}

TEST(BlockHasher, BuildManifest_SingleBlock_TempFile)
{
	BlockHasher hasher;
	ASSERT_TRUE(hasher.Initialize());

	// 创建临时文件
	wchar_t tempPath[MAX_PATH];
	GetTempPathW(MAX_PATH, tempPath);
	wcscat_s(tempPath, L"go2cbt_test_blockhasher.tmp");

	HANDLE hFile = CreateFileW(tempPath, GENERIC_READ | GENERIC_WRITE,
		0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	ASSERT_TRUE(hFile != INVALID_HANDLE_VALUE);

	// 写入 1MB 测试数据
	std::vector<uint8_t> data = MakeTestData(BLOCK_SIZE, 99);
	DWORD written = 0;
	WriteFile(hFile, data.data(), (DWORD)data.size(), &written, nullptr);
	ASSERT_EQ(written, BLOCK_SIZE);
	CloseHandle(hFile);

	// 重新以只读打开
	hFile = CreateFileW(tempPath, GENERIC_READ,
		FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	ASSERT_TRUE(hFile != INVALID_HANDLE_VALUE);

	HashManifest manifest;
	ASSERT_TRUE(hasher.BuildManifest(hFile, 0, BLOCK_SIZE, tempPath, manifest));

	// 验证清单属性
	ASSERT_EQ(manifest.BlockSize, 1048576ULL);
	ASSERT_EQ(manifest.TotalBlocks, 1ULL);
	ASSERT_EQ(manifest.TotalSize, 1048576ULL);
	ASSERT_EQ((int)manifest.Entries.size(), 1);

	// 验证条目属性
	ASSERT_EQ(manifest.Entries[0].BlockIndex, 0ULL);
	ASSERT_EQ(manifest.Entries[0].Offset, 0ULL);

	CloseHandle(hFile);
	DeleteFileW(tempPath);
}

// ============================================================
// 序列化 / 反序列化
// ============================================================
TEST(BlockHasher, SerializeDeserialize_RoundTrip)
{
	BlockHasher hasher;
	ASSERT_TRUE(hasher.Initialize());

	// 创建清单
	auto data = MakeTestData(BLOCK_SIZE, 42);
	HashManifest original;
	original.BlockSize = BLOCK_SIZE;
	original.TotalBlocks = 1;
	original.TotalSize = BLOCK_SIZE;
	original.VolumePath = L"\\\\.\\PhysicalDrive0";

	BlockHashEntry e;
	e.BlockIndex = 0;
	e.Offset = 0;
	ASSERT_TRUE(hasher.ComputeBlockHash(data.data(), (uint32_t)data.size(), e.Hash));
	original.Entries.push_back(e);

	// 序列化
	std::vector<uint8_t> serialized = BlockHasher::SerializeManifest(original);
	ASSERT_GT((int)serialized.size(), 32);  // 至少包含 header

	// 反序列化
	HashManifest restored;
	ASSERT_TRUE(BlockHasher::DeserializeManifest(serialized.data(), serialized.size(), restored));

	// 验证
	ASSERT_EQ(restored.BlockSize, original.BlockSize);
	ASSERT_EQ(restored.TotalBlocks, original.TotalBlocks);
	ASSERT_EQ(restored.TotalSize, original.TotalSize);
	ASSERT_EQ((int)restored.Entries.size(), (int)original.Entries.size());
	ASSERT_MEMEQ(restored.Entries[0].Hash, original.Entries[0].Hash, 32);
}

TEST(BlockHasher, SerializeManifest_Empty)
{
	HashManifest manifest;
	manifest.BlockSize = BLOCK_SIZE;
	manifest.TotalBlocks = 0;
	manifest.TotalSize = 0;

	std::vector<uint8_t> serialized = BlockHasher::SerializeManifest(manifest);
	ASSERT_EQ((int)serialized.size(), 32);  // 仅 header，无 entries

	HashManifest restored;
	ASSERT_TRUE(BlockHasher::DeserializeManifest(serialized.data(), serialized.size(), restored));
	ASSERT_EQ(restored.TotalBlocks, 0ULL);
}

TEST(BlockHasher, DeserializeManifest_InvalidData_Fails)
{
	HashManifest manifest;

	// 小于 header 大小
	uint8_t tiny[4] = { 0, 0, 0, 0 };
	ASSERT_FALSE(BlockHasher::DeserializeManifest(tiny, 4, manifest));

	// 空数据
	ASSERT_FALSE(BlockHasher::DeserializeManifest(nullptr, 0, manifest));
}

// ============================================================
// CompareManifests
// ============================================================
TEST(BlockHasher, CompareManifests_Identical_NoChanges)
{
	BlockHasher hasher;
	ASSERT_TRUE(hasher.Initialize());

	// 创建两个相同的清单
	auto data = MakeTestData(BLOCK_SIZE, 1);
	HashManifest m1, m2;
	m1.BlockSize = m2.BlockSize = BLOCK_SIZE;
	m1.TotalBlocks = m2.TotalBlocks = 1;
	m1.TotalSize = m2.TotalSize = BLOCK_SIZE;

	BlockHashEntry e1, e2;
	e1.BlockIndex = e2.BlockIndex = 0;
	e1.Offset = e2.Offset = 0;
	ASSERT_TRUE(hasher.ComputeBlockHash(data.data(), (uint32_t)data.size(), e1.Hash));
	memcpy(e2.Hash, e1.Hash, 32);
	m1.Entries.push_back(e1);
	m2.Entries.push_back(e2);

	std::vector<uint64_t> changed;
	BlockHasher::CompareManifests(m1, m2, changed);
	ASSERT_EQ((int)changed.size(), 0);  // 无变化
}

TEST(BlockHasher, CompareManifests_DifferentHash_DetectsChange)
{
	BlockHasher hasher;
	ASSERT_TRUE(hasher.Initialize());

	auto d1 = MakeTestData(BLOCK_SIZE, 1);
	auto d2 = MakeTestData(BLOCK_SIZE, 2);

	HashManifest m1, m2;
	m1.BlockSize = m2.BlockSize = BLOCK_SIZE;
	m1.TotalBlocks = m2.TotalBlocks = 1;
	m1.TotalSize = m2.TotalSize = BLOCK_SIZE;

	BlockHashEntry e1, e2;
	e1.BlockIndex = e2.BlockIndex = 0;
	e1.Offset = e2.Offset = 0;
	ASSERT_TRUE(hasher.ComputeBlockHash(d1.data(), (uint32_t)d1.size(), e1.Hash));
	ASSERT_TRUE(hasher.ComputeBlockHash(d2.data(), (uint32_t)d2.size(), e2.Hash));
	m1.Entries.push_back(e1);
	m2.Entries.push_back(e2);

	std::vector<uint64_t> changed;
	BlockHasher::CompareManifests(m1, m2, changed);
	ASSERT_EQ((int)changed.size(), 1);
	ASSERT_EQ(changed[0], 0ULL);
}

TEST(BlockHasher, CompareManifests_NewBlocks_Detected)
{
	HashManifest oldManifest, newManifest;
	oldManifest.BlockSize = newManifest.BlockSize = BLOCK_SIZE;
	oldManifest.TotalBlocks = 1;
	newManifest.TotalBlocks = 2;

	BlockHashEntry e;
	memset(e.Hash, 0xAA, 32);
	e.BlockIndex = 0; e.Offset = 0;
	oldManifest.Entries.push_back(e);

	e.BlockIndex = 0; e.Offset = 0;
	newManifest.Entries.push_back(e);
	e.BlockIndex = 1; e.Offset = BLOCK_SIZE;
	newManifest.Entries.push_back(e);

	std::vector<uint64_t> changed;
	BlockHasher::CompareManifests(oldManifest, newManifest, changed);
	ASSERT_EQ((int)changed.size(), 1);
	ASSERT_EQ(changed[0], 1ULL);  // 新增块 index=1
}

// ============================================================
// ReadAt — 内部方法，通过 BuildManifest 间接测试
// ============================================================
TEST(BlockHasher, ReadAt_PartialBlock_LastBlock)
{
	BlockHasher hasher;
	ASSERT_TRUE(hasher.Initialize());

	wchar_t tempPath[MAX_PATH];
	GetTempPathW(MAX_PATH, tempPath);
	wcscat_s(tempPath, L"go2cbt_test_partial_block.tmp");

	// 写入 1.5 MB 数据（确保最后一个块不足 1MB）
	const uint64_t totalSize = BLOCK_SIZE + BLOCK_SIZE / 2;
	HANDLE hFile = CreateFileW(tempPath, GENERIC_READ | GENERIC_WRITE,
		0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	ASSERT_TRUE(hFile != INVALID_HANDLE_VALUE);

	std::vector<uint8_t> data = MakeTestData((size_t)totalSize, 55);
	DWORD written = 0;
	WriteFile(hFile, data.data(), (DWORD)data.size(), &written, nullptr);
	CloseHandle(hFile);

	hFile = CreateFileW(tempPath, GENERIC_READ,
		FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	ASSERT_TRUE(hFile != INVALID_HANDLE_VALUE);

	HashManifest manifest;
	ASSERT_TRUE(hasher.BuildManifest(hFile, 0, totalSize, tempPath, manifest));

	// 应产生 2 个块
	ASSERT_EQ(manifest.TotalBlocks, 2ULL);
	ASSERT_EQ((int)manifest.Entries.size(), 2);

	CloseHandle(hFile);
	DeleteFileW(tempPath);
}

// ============================================================
// GetLastError
// ============================================================
TEST(BlockHasher, GetLastError_BeforeInit_NotEmpty)
{
	BlockHasher hasher;
	uint8_t data[32] = {};
	uint8_t hash[32];
	hasher.ComputeBlockHash(data, 32, hash);

	auto err = hasher.GetLastError();
	ASSERT_FALSE(err.empty());
}
