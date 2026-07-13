#include "DataCompressor.h"
#include "Logger.h"


namespace DataCompress
{

	bool DataCompressor::Initialize()
	{
		if (m_initialized)
		{
			return true;
		}

		BOOL result = CreateCompressor(
			COMPRESS_ALGORITHM_MSZIP,   // deflate 算法（输出需转换为 zlib 格式）
			nullptr,
			&m_hCompressor
		);

		if (!result)
		{
			DWORD err = GetLastError();
			wchar_t msg[256];
			swprintf_s(msg, L"[DataCompressor] CreateCompressor failed, error=%lu", err);
			LOG_ERROR(msg);
			return false;
		}

		m_initialized = true;
		LOG_INFO(L"[DataCompressor] MSZIP compressor initialized (zlib output)");
		return true;
	}

	void DataCompressor::Shutdown()
	{
		if (m_hCompressor)
		{
			CloseCompressor(m_hCompressor);
			m_hCompressor = nullptr;
		}
		m_initialized = false;
	}

	// ============================================================
	// Adler-32 校验和计算（RFC 1950，zlib 格式尾部校验）
	// ============================================================
	uint32_t DataCompressor::ComputeAdler32(const uint8_t* data, size_t len)
	{
		// Adler-32 = (B << 16) | A
		// A = 1 + sum(data[i]) mod 65521
		// B = sum(A_i) mod 65521
		uint32_t a = 1, b = 0;
		for (size_t i = 0; i < len; i++)
		{
			a = (a + data[i]) % 65521;
			b = (b + a) % 65521;
		}
		return (b << 16) | a;
	}

	// ============================================================
	// 压缩：MSZIP → zlib 格式转换
	// ============================================================
	bool DataCompressor::Compress(const uint8_t* input, uint32_t inputSize, std::vector<uint8_t>& output)
	{
		if (!m_initialized)
		{
			LOG_ERROR(L"[DataCompressor] Not initialized");
			return false;
		}

		// ---- Step 1: 使用 MSZIP 压缩 ----
		size_t maxSize = GetMaxCompressedSize(inputSize);
		output.resize(maxSize);

		SIZE_T compressedSize = 0;
		BOOL result = ::Compress(
			m_hCompressor,
			const_cast<uint8_t*>(input),
			inputSize,
			output.data(),
			output.size(),
			&compressedSize
		);

		if (!result)
		{
			DWORD err = GetLastError();
			if (err == ERROR_INSUFFICIENT_BUFFER)
			{
				// 重新分配更大的缓冲区再试
				output.resize(maxSize * 2);
				result = ::Compress(
					m_hCompressor,
					const_cast<uint8_t*>(input),
					inputSize,
					output.data(),
					output.size(),
					&compressedSize
				);
			}

			if (!result)
			{
				err = GetLastError();
				wchar_t msg[256];
				swprintf_s(msg, L"[DataCompressor] Compress failed, error=%lu", err);
				LOG_ERROR(msg);
				return false;
			}
		}

		// ---- Step 2: 将 MSZIP 格式转换为 zlib 格式 ----
		// MSZIP 格式: [0x43, 0x4B "CK"签名] + raw_deflate_data
		// zlib  格式: [0x78, 0x9C CMF+FLG头] + raw_deflate_data + adler32(4B big-endian)
		// 服务端使用 zlib 的 uncompress2() 解压，必须提供 zlib 格式

		if (compressedSize < 2)
		{
			LOG_ERROR(L"[DataCompressor] MSZIP output too short");
			return false;
		}

		// 验证 MSZIP 签名 "CK" (0x43, 0x4B)
		if (output[0] != 0x43 || output[1] != 0x4B)
		{
			wchar_t msg[256];
			swprintf_s(msg, L"[DataCompressor] Invalid MSZIP signature: 0x%02X 0x%02X (expected 0x43 0x4B)",
				output[0], output[1]);
			LOG_ERROR(msg);
			return false;
		}

		// 剥离 "CK" 头部，构建 zlib 格式
		size_t deflateSize = compressedSize - 2;
		uint32_t adler = ComputeAdler32(input, inputSize);

		// zlib 输出: header(2) + deflate_data + adler32(4)
		std::vector<uint8_t> zlibData(2 + deflateSize + 4);

		// zlib 头: CMF=0x78 (deflate + 32K window), FLG=0x9C (default compression)
		// (0x78 * 256 + 0x9C) % 31 == 0，满足 RFC 1950 校验要求
		zlibData[0] = 0x78;
		zlibData[1] = 0x9C;

		// 复制 deflate 数据（跳过 MSZIP 的 "CK" 签名）
		memcpy(zlibData.data() + 2, output.data() + 2, deflateSize);

		// Adler-32 校验和（大端序）
		zlibData[2 + deflateSize]     = static_cast<uint8_t>(adler >> 24);
		zlibData[2 + deflateSize + 1] = static_cast<uint8_t>(adler >> 16);
		zlibData[2 + deflateSize + 2] = static_cast<uint8_t>(adler >> 8);
		zlibData[2 + deflateSize + 3] = static_cast<uint8_t>(adler);

		output = std::move(zlibData);

		// 调试日志：压缩结果统计
		{
			wchar_t dbg[256];
			swprintf_s(dbg, L"[DataCompressor] MSZIP->zlib: raw=%u compressed=%zu adler32=0x%08X",
				inputSize, output.size(), adler);
			LOG_DEBUG(dbg);
		}

		return true;
	}

	// ============================================================
	// 解压：zlib 格式 → MSZIP 格式转换后使用 Windows API
	// ============================================================
	bool DataCompressor::Decompress(const uint8_t* input, uint32_t inputSize,
		uint32_t originalSize, std::vector<uint8_t>& output)
	{
		// zlib 格式: [CMF, FLG] + raw_deflate + adler32(4B big-endian)
		// MSZIP 格式: [0x43, 0x4B] + raw_deflate

		if (inputSize < 6)  // 至少 2(zlib头) + 4(adler32)
		{
			return false;
		}

		// 验证 zlib 头: CMF 低 4 位 = 8 表示 deflate
		if ((input[0] & 0x0F) != 8)
		{
			return false;
		}

		// 提取 deflate 数据（跳过 2 字节 zlib 头，去掉 4 字节 Adler-32 尾部）
		size_t deflateSize = inputSize - 2 - 4;

		// 构建 MSZIP 格式数据
		std::vector<uint8_t> mszipData(2 + deflateSize);
		mszipData[0] = 0x43;  // 'C'
		mszipData[1] = 0x4B;  // 'K'
		memcpy(mszipData.data() + 2, input + 2, deflateSize);

		// 使用 Windows API 解压
		DECOMPRESSOR_HANDLE hDecompressor = nullptr;

		BOOL result = CreateDecompressor(
			COMPRESS_ALGORITHM_MSZIP,
			nullptr,
			&hDecompressor
		);

		if (!result)
		{
			return false;
		}

		output.resize(originalSize);
		SIZE_T decompressedSize = 0;

		result = ::Decompress(
			hDecompressor,
			mszipData.data(),
			static_cast<SIZE_T>(mszipData.size()),
			output.data(),
			output.size(),
			&decompressedSize
		);

		CloseDecompressor(hDecompressor);

		if (!result)
		{
			return false;
		}

		output.resize(decompressedSize);
		return (decompressedSize == originalSize);
	}

} // namespace DataCompress
