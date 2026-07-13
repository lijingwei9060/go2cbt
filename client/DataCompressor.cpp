#include "DataCompressor.h"
#include "Logger.h"
#include <algorithm>


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
		uint32_t a = 1, b = 0;
		for (size_t i = 0; i < len; i++)
		{
			a = (a + data[i]) % 65521;
			b = (b + a) % 65521;
		}
		return (b << 16) | a;
	}

	// ============================================================
	// 构造 zlib stored block（不压缩，服务端一定可解压的兜底方案）
	// 格式: zlib_header(2) + deflate_stored_blocks + adler32(4)
	// 每个 stored block: BFINAL(1bit)+BTYPE(2bits)+padding(5bits) + LEN(2B) + NLEN(2B) + data
	// ============================================================
	bool DataCompressor::CreateStoredZlib(const uint8_t* input, uint32_t inputSize,
		std::vector<uint8_t>& output)
	{
		uint32_t adler = ComputeAdler32(input, inputSize);

		// 计算总大小: zlib头(2) + 每块头(5) * 块数 + 数据 + adler32(4)
		// 每块最多 65535 字节（deflate stored block 限制）
		uint32_t numChunks = (inputSize + 65534) / 65535;
		size_t totalSize = 2 + (size_t)numChunks * 5 + inputSize + 4;
		output.resize(totalSize);
		size_t pos = 0;

		// zlib 头: CMF=0x78 (deflate + 32K window), FLG=0x01 (fastest, no compression)
		// (0x78 * 256 + 0x01) % 31 == 0，满足 RFC 1950 校验
		output[pos++] = 0x78;
		output[pos++] = 0x01;

		// 写入 stored deflate 块
		uint32_t offset = 0;
		while (offset < inputSize)
		{
			uint32_t chunkSize = (std::min)(inputSize - offset, (uint32_t)65535);
			bool isFinal = (offset + chunkSize >= inputSize);

			// 块头字节: bit0=BFINAL, bit1-2=BTYPE(00=stored), bit3-7=padding
			output[pos++] = isFinal ? 0x01 : 0x00;

			// LEN (little-endian)
			output[pos++] = static_cast<uint8_t>(chunkSize);
			output[pos++] = static_cast<uint8_t>(chunkSize >> 8);

			// NLEN = ~LEN (little-endian)
			uint16_t nlen = static_cast<uint16_t>(~chunkSize);
			output[pos++] = static_cast<uint8_t>(nlen);
			output[pos++] = static_cast<uint8_t>(nlen >> 8);

			// 数据
			memcpy(output.data() + pos, input + offset, chunkSize);
			pos += chunkSize;
			offset += chunkSize;
		}

		// Adler-32 校验和（大端序）
		output[pos++] = static_cast<uint8_t>(adler >> 24);
		output[pos++] = static_cast<uint8_t>(adler >> 16);
		output[pos++] = static_cast<uint8_t>(adler >> 8);
		output[pos++] = static_cast<uint8_t>(adler);

		output.resize(pos);
		return true;
	}

	// ============================================================
	// 压缩：MSZIP → zlib 格式转换，失败回退 stored block
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
		std::vector<uint8_t> mszipBuf(maxSize);

		SIZE_T compressedSize = 0;
		BOOL result = ::Compress(
			m_hCompressor,
			const_cast<uint8_t*>(input),
			inputSize,
			mszipBuf.data(),
			mszipBuf.size(),
			&compressedSize
		);

		if (!result)
		{
			DWORD err = GetLastError();
			if (err == ERROR_INSUFFICIENT_BUFFER)
			{
				mszipBuf.resize(maxSize * 2);
				result = ::Compress(
					m_hCompressor,
					const_cast<uint8_t*>(input),
					inputSize,
					mszipBuf.data(),
					mszipBuf.size(),
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

		// ---- Step 2: 尝试将 MSZIP 格式转换为 zlib 格式 ----
		// MSZIP 格式: [0x43, 0x4B "CK"签名] + raw_deflate_data
		// zlib  格式: [0x78, 0x9C CMF+FLG头] + raw_deflate_data + adler32(4B big-endian)

		if (compressedSize >= 2 && mszipBuf[0] == 0x43 && mszipBuf[1] == 0x4B)
		{
			// MSZIP "CK" 签名验证通过，执行标准转换
			size_t deflateSize = compressedSize - 2;
			uint32_t adler = ComputeAdler32(input, inputSize);

			// zlib 输出: header(2) + deflate_data + adler32(4)
			output.resize(2 + deflateSize + 4);

			// zlib 头: CMF=0x78, FLG=0x9C (default compression)
			output[0] = 0x78;
			output[1] = 0x9C;

			// 复制 deflate 数据（跳过 MSZIP 的 "CK" 签名）
			memcpy(output.data() + 2, mszipBuf.data() + 2, deflateSize);

			// Adler-32 校验和（大端序）
			output[2 + deflateSize]     = static_cast<uint8_t>(adler >> 24);
			output[2 + deflateSize + 1] = static_cast<uint8_t>(adler >> 16);
			output[2 + deflateSize + 2] = static_cast<uint8_t>(adler >> 8);
			output[2 + deflateSize + 3] = static_cast<uint8_t>(adler);

			wchar_t dbgCvt[256];
			swprintf_s(dbgCvt, L"[DataCompressor] MSZIP->zlib conversion: raw=%u compressed=%u",
				inputSize, (uint32_t)output.size());
			LOG_DEBUG(dbgCvt);
			return true;
		}

		// ---- Step 3: MSZIP 签名不匹配，回退到 zlib stored block ----
		// stored block 不压缩数据，但保证服务端 uncompress2 一定可解压
		{
			wchar_t msg[256];
			swprintf_s(msg, L"[DataCompressor] MSZIP signature mismatch (got 0x%02X 0x%02X), falling back to stored block",
				compressedSize >= 1 ? mszipBuf[0] : 0,
				compressedSize >= 2 ? mszipBuf[1] : 0);
			LOG_WARNING(msg);
		}

		return CreateStoredZlib(input, inputSize, output);
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
