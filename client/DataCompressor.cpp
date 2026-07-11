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
			COMPRESS_ALGORITHM_MSZIP,   // deflate 算法
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
		LOG_INFO(L"[DataCompressor] MSZIP compressor initialized");
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

	bool DataCompressor::Compress(const uint8_t* input, uint32_t inputSize, std::vector<uint8_t>& output)
	{
		if (!m_initialized)
		{
			LOG_ERROR(L"[DataCompressor] Not initialized");
			return false;
		}

		// 分配输出缓冲区（最大可能大小）
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

		output.resize(compressedSize);
		return true;
	}

	bool DataCompressor::Decompress(const uint8_t* input, uint32_t inputSize,
		uint32_t originalSize, std::vector<uint8_t>& output)
	{
		// 使用 Decompressor Handle (每次创建，无需保留)
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
			const_cast<uint8_t*>(input),
			inputSize,
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
