#pragma once
#include <windows.h>
#include <compressapi.h>
#include <cstdint>
#include <vector>


namespace DataCompress
{

//
// DataCompressor: 使用 Windows Compression API (MSZIP) 进行 deflate 压缩
// 输出自动转换为 zlib 格式（与服务端 uncompress2 兼容）
// 支持新旧 Windows 版本：
//   旧版输出 "CK" + raw_deflate（.cab MSZIP 格式）
//   新版直接输出 raw_deflate（无 "CK" 前缀）
// 两种情况均正确转换为 zlib 格式
//
class DataCompressor
{

public:

	//
	// 初始化压缩器
	// 返回 true 表示成功创建 Compressor Handle
	//
	DataCompressor() : m_hCompressor(nullptr), m_initialized(false) {}

	bool Initialize();

	//
	// 销毁压缩器（析构时自动调用）
	//
	void Shutdown();

	//
	// 压缩数据
	// input: 原始数据
	// inputSize: 原始大小 (<= BLOCK_SIZE)
	// output: [输出] 压缩后数据（zlib 格式，与服务端兼容）
	// 返回 true 表示压缩成功
	//
	bool Compress(const uint8_t* input, uint32_t inputSize, std::vector<uint8_t>& output);

	//
	// 获取压缩后数据的最大可能大小（用于预分配缓冲区）
	//
	static size_t GetMaxCompressedSize(size_t inputSize)
	{
		// deflate 极端情况: 比原始数据稍大
		// MSZIP → zlib 转换后比 MSZIP 多 4 字节（-2 CK头 +2 zlib头 +4 Adler32）
		return inputSize + (inputSize / 1024) + 128 + 4;
	}

	//
	// 解压数据（静态方法，不依赖实例）
	// input: zlib 格式压缩数据
	// inputSize: 压缩数据大小
	// originalSize: 原始数据大小（压缩前已知）
	// output: [输出] 解压后数据
	//
	static bool Decompress(const uint8_t* input, uint32_t inputSize,
		uint32_t originalSize, std::vector<uint8_t>& output);

	//
	// 计算 Adler-32 校验和（zlib 格式尾部校验，RFC 1950）
	//
	static uint32_t ComputeAdler32(const uint8_t* data, size_t len);

private:

	COMPRESSOR_HANDLE m_hCompressor;
	bool m_initialized;

	// 构造 zlib stored block（不压缩的兜底方案，保证服务端可解压）
	static bool CreateStoredZlib(const uint8_t* input, uint32_t inputSize,
		std::vector<uint8_t>& output);
};

} // namespace DataCompress
