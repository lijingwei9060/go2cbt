// DataCompressor 模块单元测试
// 覆盖：MSZIP 压缩/解压 round-trip、边界条件、错误处理
#include "test_framework.h"
#include "../client/DataCompressor.h"

using namespace DataCompress;

// ============================================================
// 初始化
// ============================================================
TEST(DataCompressor, Initialize_Succeeds)
{
	DataCompressor comp;
	ASSERT_TRUE(comp.Initialize());
}

TEST(DataCompressor, Initialize_Idempotent)
{
	DataCompressor comp;
	ASSERT_TRUE(comp.Initialize());
	ASSERT_TRUE(comp.Initialize());  // 第二次无副作用
}

TEST(DataCompressor, Compress_WithoutInit_Fails)
{
	DataCompressor comp;
	std::vector<uint8_t> input = { 1, 2, 3 };
	std::vector<uint8_t> output;
	ASSERT_FALSE(comp.Compress(input.data(), (uint32_t)input.size(), output));
}

// ============================================================
// 压缩 / 解压 Round-Trip
// ============================================================
TEST(DataCompressor, CompressDecompress_RoundTrip_SmallData)
{
	DataCompressor comp;
	ASSERT_TRUE(comp.Initialize());

	std::vector<uint8_t> input = MakeTestData(1024, 13);

	std::vector<uint8_t> compressed;
	ASSERT_TRUE(comp.Compress(input.data(), (uint32_t)input.size(), compressed));
	ASSERT_GT((int)compressed.size(), 0);

	std::vector<uint8_t> decompressed;
	ASSERT_TRUE(DataCompressor::Decompress(
		compressed.data(), (uint32_t)compressed.size(),
		(uint32_t)input.size(), decompressed));

	ASSERT_EQ((int)decompressed.size(), (int)input.size());
	ASSERT_MEMEQ(decompressed.data(), input.data(), input.size());
}

TEST(DataCompressor, CompressDecompress_RoundTrip_OneMB)
{
	DataCompressor comp;
	ASSERT_TRUE(comp.Initialize());

	std::vector<uint8_t> input = MakeTestData(1024 * 1024, 7);

	std::vector<uint8_t> compressed;
	ASSERT_TRUE(comp.Compress(input.data(), (uint32_t)input.size(), compressed));

	std::vector<uint8_t> decompressed;
	ASSERT_TRUE(DataCompressor::Decompress(
		compressed.data(), (uint32_t)compressed.size(),
		(uint32_t)input.size(), decompressed));

	ASSERT_EQ((int)decompressed.size(), (int)input.size());
	ASSERT_MEMEQ(decompressed.data(), input.data(), input.size());
}

TEST(DataCompressor, Compress_RepeatedData_CompressesWell)
{
	DataCompressor comp;
	ASSERT_TRUE(comp.Initialize());

	// 全零数据应该压缩得很好
	std::vector<uint8_t> zeros(1024 * 1024, 0);

	std::vector<uint8_t> compressed;
	ASSERT_TRUE(comp.Compress(zeros.data(), (uint32_t)zeros.size(), compressed));

	// 压缩比应 > 10:1
	double ratio = (double)zeros.size() / (double)compressed.size();
	ASSERT_GT(ratio, 10.0);
}

TEST(DataCompressor, Compress_CompressedSize_LessThanMax)
{
	DataCompressor comp;
	ASSERT_TRUE(comp.Initialize());

	std::vector<uint8_t> input = MakeTestData(1024 * 1024, 99);

	std::vector<uint8_t> compressed;
	ASSERT_TRUE(comp.Compress(input.data(), (uint32_t)input.size(), compressed));

	// 压缩后不应超过最大可能大小
	size_t maxSize = DataCompressor::GetMaxCompressedSize(input.size());
	ASSERT_LE((int)compressed.size(), (int)maxSize);
}

TEST(DataCompressor, Decompress_InvalidData_Fails)
{
	std::vector<uint8_t> invalid = { 0xFF, 0xFF, 0xFF, 0xFF };
	std::vector<uint8_t> output;
	ASSERT_FALSE(DataCompressor::Decompress(
		invalid.data(), (uint32_t)invalid.size(), 1024, output));
}

TEST(DataCompressor, Decompress_WrongOriginalSize_Fails)
{
	DataCompressor comp;
	ASSERT_TRUE(comp.Initialize());

	std::vector<uint8_t> input = MakeTestData(512, 42);
	std::vector<uint8_t> compressed;
	ASSERT_TRUE(comp.Compress(input.data(), (uint32_t)input.size(), compressed));

	// 使用错误的原始大小
	std::vector<uint8_t> output;
	ASSERT_FALSE(DataCompressor::Decompress(
		compressed.data(), (uint32_t)compressed.size(),
		99999, output));  // 错误的 originalSize
}

// ============================================================
// GetMaxCompressedSize
// ============================================================
TEST(DataCompressor, GetMaxCompressedSize_GreaterThanInput)
{
	size_t inputSize = 1024 * 1024;
	size_t maxSize = DataCompressor::GetMaxCompressedSize(inputSize);
	ASSERT_GE((int)maxSize, (int)inputSize);  // 最大可能大小 >= 原始大小
}

// ============================================================
// Shutdown
// ============================================================
TEST(DataCompressor, Shutdown_NoCrash)
{
	DataCompressor comp;
	ASSERT_TRUE(comp.Initialize());
	ASSERT_NO_THROW(comp.Shutdown());
	ASSERT_NO_THROW(comp.Shutdown());  // 可重复调用
}
