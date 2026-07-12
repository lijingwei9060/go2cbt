#pragma once
#include <windows.h>
#include <compressapi.h>
#include <cstdint>
#include <vector>


namespace DataCompress
{

//
// DataCompressor: 使用 Windows Compression API (deflate) 进行数据压缩
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
	// output: [输出] 压缩后数据
	// 返回 true 表示压缩成功
	//
	bool Compress(const uint8_t* input, uint32_t inputSize, std::vector<uint8_t>& output);

	//
	// 获取压缩后数据的最大可能大小（用于预分配缓冲区）
	//
	static size_t GetMaxCompressedSize(size_t inputSize)
	{
		// deflate 极端情况: 比原始数据稍大
		return inputSize + (inputSize / 1024) + 128;
	}

	//
	// 解压数据（静态方法，不依赖实例）
	// input: 压缩数据
	// inputSize: 压缩数据大小
	// originalSize: 原始数据大小（压缩前已知）
	// output: [输出] 解压后数据
	//
	static bool Decompress(const uint8_t* input, uint32_t inputSize,
		uint32_t originalSize, std::vector<uint8_t>& output);

private:

	COMPRESSOR_HANDLE m_hCompressor;
	bool m_initialized;
};

} // namespace DataCompress
