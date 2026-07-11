#include "BlockHasher.h"
#include "Logger.h"
#include <string.h>


namespace BlockHash
{

	BlockHasher::BlockHasher()
		: m_initialized(false)
		, m_hAlg(nullptr)
	{
	}

	BlockHasher::~BlockHasher()
	{
		if (m_hAlg)
		{
			BCryptCloseAlgorithmProvider(m_hAlg, 0);
			m_hAlg = nullptr;
		}
	}

	// ============================================================
	// 初始化 BCrypt SHA-256 Provider
	// ============================================================
	bool BlockHasher::Initialize()
	{
		if (m_initialized)
		{
			return true;
		}

		NTSTATUS status = BCryptOpenAlgorithmProvider(
			&m_hAlg,
			BCRYPT_SHA256_ALGORITHM,
			nullptr,
			0
		);

		if (!BCRYPT_SUCCESS(status))
		{
			wchar_t msg[256];
			swprintf_s(msg, L"[BlockHasher] BCryptOpenAlgorithmProvider(SHA256) failed: 0x%08X", status);
			LOG_ERROR(msg);
			m_lastError = L"Failed to open SHA-256 algorithm provider";
			return false;
		}

		// 分配 1MB 数据缓冲区（整个模块复用）
		m_buffer.resize(BLOCK_SIZE);

		m_initialized = true;
		LOG_INFO(L"[BlockHasher] SHA-256 BCrypt provider initialized");
		return true;
	}

	// ============================================================
	// 计算单个块的 SHA-256 哈希
	// ============================================================
	bool BlockHasher::ComputeBlockHash(const uint8_t* data, uint32_t size, uint8_t hashOut[SHA256_HASH_SIZE])
	{
		if (!m_initialized || !m_hAlg)
		{
			m_lastError = L"BlockHasher not initialized";
			return false;
		}

		BCRYPT_HASH_HANDLE hHash = nullptr;
		NTSTATUS status = BCryptCreateHash(
			m_hAlg,
			&hHash,
			nullptr, 0,
			nullptr, 0,
			0
		);

		if (!BCRYPT_SUCCESS(status))
		{
			wchar_t msg[256];
			swprintf_s(msg, L"[BlockHasher] BCryptCreateHash failed: 0x%08X", status);
			LOG_ERROR(msg);
			return false;
		}

		// 喂数据
		status = BCryptHashData(hHash, const_cast<PUCHAR>(data), size, 0);

		if (!BCRYPT_SUCCESS(status))
		{
			wchar_t msg[256];
			swprintf_s(msg, L"[BlockHasher] BCryptHashData failed: 0x%08X", status);
			LOG_ERROR(msg);
			BCryptDestroyHash(hHash);
			return false;
		}

		// 完成哈希计算，获取 32 字节摘要
		status = BCryptFinishHash(hHash, hashOut, SHA256_HASH_SIZE, 0);
		BCryptDestroyHash(hHash);

		if (!BCRYPT_SUCCESS(status))
		{
			wchar_t msg[256];
			swprintf_s(msg, L"[BlockHasher] BCryptFinishHash failed: 0x%08X", status);
			LOG_ERROR(msg);
			return false;
		}

		return true;
	}

	// ============================================================
	// 批量构建哈希清单
	// ============================================================
	bool BlockHasher::BuildManifest(HANDLE hSource, uint64_t offset, uint64_t totalSize,
		const std::wstring& volumePath, HashManifest& manifest)
	{
		if (!m_initialized || !hSource || hSource == INVALID_HANDLE_VALUE)
		{
			m_lastError = L"BlockHasher not initialized or invalid handle";
			return false;
		}

		manifest = {};
		manifest.BlockSize = BLOCK_SIZE;
		manifest.TotalSize = totalSize;
		manifest.TotalBlocks = (totalSize + BLOCK_SIZE - 1) / BLOCK_SIZE;
		manifest.VolumePath = volumePath;

		// 获取当前时间戳
		FILETIME ft;
		GetSystemTimeAsFileTime(&ft);
		manifest.Timestamp = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;

		manifest.Entries.reserve(manifest.TotalBlocks);

		uint64_t remaining = totalSize;
		uint64_t currentOffset = offset;

		LOG_INFO(L"[BlockHasher] Starting manifest build");

		for (uint64_t i = 0; i < manifest.TotalBlocks; i++)
		{
			uint32_t blockSize = (remaining >= BLOCK_SIZE) ? (uint32_t)BLOCK_SIZE : (uint32_t)remaining;

			if (!ReadAt(hSource, currentOffset, m_buffer.data(), blockSize))
			{
				wchar_t msg[256];
				swprintf_s(msg, L"[BlockHasher] ReadAt failed at offset 0x%llx, block %llu",
					currentOffset, i);
				LOG_ERROR(msg);
				m_lastError = L"Failed to read data block";
				return false;
			}

			BlockHashEntry entry;
			entry.BlockIndex = i;
			entry.Offset = currentOffset;

			if (!ComputeBlockHash(m_buffer.data(), blockSize, entry.Hash))
			{
				wchar_t msg[256];
				swprintf_s(msg, L"[BlockHasher] ComputeBlockHash failed at block %llu", i);
				LOG_ERROR(msg);
				return false;
			}

			manifest.Entries.push_back(entry);

			currentOffset += blockSize;
			remaining -= blockSize;

			// 每 1000 个块输出一次进度
			if (i % 1000 == 0 && i > 0)
			{
				wchar_t msg[256];
				swprintf_s(msg, L"[BlockHasher] Progress: %llu/%llu blocks hashed", i, manifest.TotalBlocks);
				LOG_INFO(msg);
			}
		}

		wchar_t msg[256];
		swprintf_s(msg, L"[BlockHasher] Manifest complete: %llu blocks, %u bytes/block",
			manifest.TotalBlocks, (uint32_t)BLOCK_SIZE);
		LOG_INFO(msg);

		return true;
	}

	// ============================================================
	// 仅读取指定块的哈希
	// ============================================================
	bool BlockHasher::ComputeBlockHashes(HANDLE hSource, const HashManifest& manifest,
		const std::vector<uint64_t>& blockIndexes,
		std::vector<BlockHashEntry>& hashes)
	{
		if (!m_initialized || !hSource || hSource == INVALID_HANDLE_VALUE)
		{
			m_lastError = L"BlockHasher not initialized or invalid handle";
			return false;
		}

		hashes.clear();
		hashes.reserve(blockIndexes.size());

		for (uint64_t idx : blockIndexes)
		{
			if (idx >= manifest.Entries.size())
			{
				continue;
			}

			const auto& entry = manifest.Entries[idx];
			uint32_t blockSize = BLOCK_SIZE;

			// 最后一个块可能不足 1MB
			if (idx == manifest.TotalBlocks - 1)
			{
				uint64_t lastBlockSize = manifest.TotalSize % BLOCK_SIZE;
				if (lastBlockSize > 0)
				{
					blockSize = (uint32_t)lastBlockSize;
				}
			}

			if (!ReadAt(hSource, entry.Offset, m_buffer.data(), blockSize))
			{
				wchar_t msg[256];
				swprintf_s(msg, L"[BlockHasher] ReadAt failed for block %llu at offset 0x%llx",
					idx, entry.Offset);
				LOG_ERROR(msg);
				return false;
			}

			BlockHashEntry newEntry;
			newEntry.BlockIndex = idx;
			newEntry.Offset = entry.Offset;

			if (!ComputeBlockHash(m_buffer.data(), blockSize, newEntry.Hash))
			{
				return false;
			}

			hashes.push_back(newEntry);
		}

		return true;
	}

	// ============================================================
	// 比较两份哈希清单，返回变更的块索引
	// ============================================================
	void BlockHasher::CompareManifests(const HashManifest& old, const HashManifest& current,
		std::vector<uint64_t>& changedBlocks)
	{
		changedBlocks.clear();

		// 新清单多于旧清单的部分 = 新增块（视为变化）
		uint64_t maxBlocks = (std::max)(old.Entries.size(), current.Entries.size());

		for (uint64_t i = 0; i < maxBlocks; i++)
		{
			// 新增块
			if (i >= old.Entries.size())
			{
				changedBlocks.push_back(i);
				continue;
			}

			// 删除块（一般不会发生，但处理一下）
			if (i >= current.Entries.size())
			{
				changedBlocks.push_back(i);
				continue;
			}

			// 比较哈希
			if (memcmp(old.Entries[i].Hash, current.Entries[i].Hash, SHA256_HASH_SIZE) != 0)
			{
				changedBlocks.push_back(i);
			}
		}

		wchar_t msg[256];
		swprintf_s(msg, L"[BlockHasher] Manifest comparison: %llu/%llu blocks changed",
			(uint64_t)changedBlocks.size(), (uint64_t)current.Entries.size());
		LOG_INFO(msg);
	}

	// ============================================================
	// 序列化清单为二进制
	//
	// 二进制格式:
	//   [Header: 32 bytes]
	//     - Magic:    4 bytes ("BHMF")
	//     - Version:  4 bytes (1)
	//     - BlockSize: 8 bytes
	//     - TotalBlocks: 8 bytes
	//     - TotalSize: 8 bytes
	//
	//   [Entry Array: TotalBlocks * 40 bytes]
	//     - BlockIndex: 8 bytes
	//     - Hash[32]: 32 bytes
	// ============================================================
	std::vector<uint8_t> BlockHasher::SerializeManifest(const HashManifest& manifest)
	{
		std::vector<uint8_t> out;

		// Header: 32 bytes
		const uint32_t magic = 0x464D4842;  // "BHMF" little-endian
		const uint32_t version = 1;

		auto append = [&out](const void* data, size_t size) {
			const uint8_t* p = static_cast<const uint8_t*>(data);
			out.insert(out.end(), p, p + size);
		};

		append(&magic, 4);
		append(&version, 4);
		append(&manifest.BlockSize, 8);
		append(&manifest.TotalBlocks, 8);
		append(&manifest.TotalSize, 8);

		// Entries
		for (const auto& entry : manifest.Entries)
		{
			append(&entry.BlockIndex, 8);
			append(entry.Hash, SHA256_HASH_SIZE);
		}

		return out;
	}

	// ============================================================
	// 从二进制反序列化清单
	// ============================================================
	bool BlockHasher::DeserializeManifest(const uint8_t* data, size_t size, HashManifest& manifest)
	{
		if (!data || size < 32)
		{
			return false;
		}

		size_t offset = 0;

		auto read = [&](void* buf, size_t len) -> bool {
			if (offset + len > size) return false;
			memcpy(buf, data + offset, len);
			offset += len;
			return true;
		};

		uint32_t magic = 0, version = 0;
		if (!read(&magic, 4) || magic != 0x464D4842) return false;
		if (!read(&version, 4) || version != 1) return false;

		manifest = {};
		if (!read(&manifest.BlockSize, 8)) return false;
		if (!read(&manifest.TotalBlocks, 8)) return false;
		if (!read(&manifest.TotalSize, 8)) return false;

		// Entries
		size_t expectedSize = 32 + manifest.TotalBlocks * (8 + SHA256_HASH_SIZE);
		if (size < expectedSize)
		{
			return false;
		}

		manifest.Entries.resize(manifest.TotalBlocks);
		for (uint64_t i = 0; i < manifest.TotalBlocks; i++)
		{
			if (!read(&manifest.Entries[i].BlockIndex, 8)) return false;
			if (!read(manifest.Entries[i].Hash, SHA256_HASH_SIZE)) return false;
		}

		return true;
	}

	// ============================================================
	// 哈希值转十六进制字符串
	// ============================================================
	std::wstring BlockHasher::HashToHex(const uint8_t hash[SHA256_HASH_SIZE])
	{
		wchar_t hex[65];
		for (int i = 0; i < 32; i++)
		{
			swprintf_s(hex + i * 2, 3, L"%02X", hash[i]);
		}
		hex[64] = L'\0';
		return std::wstring(hex);
	}

	// ============================================================
	// 从数据源按偏移读取数据
	// ============================================================
	bool BlockHasher::ReadAt(HANDLE hFile, uint64_t offset, void* buffer, uint32_t size)
	{
		LARGE_INTEGER pos;
		pos.QuadPart = offset;

		if (!SetFilePointerEx(hFile, pos, nullptr, FILE_BEGIN))
		{
			return false;
		}

		DWORD bytesRead = 0;
		if (!ReadFile(hFile, buffer, size, &bytesRead, nullptr))
		{
			return false;
		}

		return bytesRead == size;
	}

} // namespace BlockHash
