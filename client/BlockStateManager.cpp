#include "BlockStateManager.h"
#include "BlockHasher.h"
#include "Logger.h"
#include <string.h>
#include <algorithm>


namespace BlockState
{

	// 状态文件魔数
	static const uint32_t STATE_FILE_MAGIC = 0x534B4C42;  // "BLKS" little-endian
	static const uint32_t STATE_FILE_VERSION = 1;
	static const size_t   BLOCK_RECORD_SIZE = 48;            // 每条块记录固定 48 字节
	static const size_t   HEADER_SIZE = 64;

	// ============================================================
	// 构造 / 析构
	// ============================================================
	BlockStateManager::BlockStateManager()
		: m_initialized(false)
		, m_dirty(false)
		, m_devNo(-1)
		, m_totalBlocks(0)
		, m_nextVersionId(0)
	{
	}

	BlockStateManager::~BlockStateManager()
	{
		if (m_dirty && m_initialized)
		{
			Save();
		}
	}

	// ============================================================
	// 初始化
	// ============================================================
	bool BlockStateManager::Initialize(const std::wstring& stateDir, int devNo)
	{
		std::lock_guard<std::mutex> lock(m_mutex);

		m_stateDir = stateDir;
		m_devNo = devNo;
		m_initialized = true;

		// 确保目录存在
		CreateDirectoryW(stateDir.c_str(), nullptr);

		// 尝试加载已有状态文件
		if (Load())
		{
			wchar_t msg[256];
			swprintf_s(msg, L"[BlockState] Loaded existing state for Disk%d: %zu blocks, %zu versions",
				devNo, m_blocks.size(), m_versions.size());
			LOG_INFO(msg);
		}
		else
		{
			wchar_t msg[256];
			swprintf_s(msg, L"[BlockState] New state file created for Disk%d", devNo);
			LOG_INFO(msg);
		}

		return true;
	}

	// ============================================================
	// 创建新版本
	// ============================================================
	VersionRecord BlockStateManager::CreateVersion(const std::wstring& type, FILETIME snapshotTime,
		uint64_t totalBlocks, uint64_t totalSize)
	{
		std::lock_guard<std::mutex> lock(m_mutex);

		VersionRecord ver;
		ver.VersionId = m_nextVersionId++;
		ver.DevNo = m_devNo;
		ver.VersionType = type;
		ver.SnapshotTime = snapshotTime;
		ver.TotalBlocks = totalBlocks;
		ver.TotalSize = totalSize;
		ver.ChangedBlocks = 0;
		ver.AckedBlocks = 0;

		FILETIME localFt;
		GetSystemTimeAsFileTime(&localFt);
		ver.CreateTimestamp = ((uint64_t)localFt.dwHighDateTime << 32) | localFt.dwLowDateTime;

		m_versions[ver.VersionId] = ver;
		m_dirty = true;

		wchar_t msg[256];
		swprintf_s(msg, L"[BlockState] Version %llu created: type=%ls, blocks=%llu",
			ver.VersionId, type.c_str(), totalBlocks);
		LOG_INFO(msg);

		return ver;
	}

	// ============================================================
	// 批量初始化全量备份块状态
	// ============================================================
	bool BlockStateManager::InitFullBlocks(uint64_t versionId, const BlockHash::HashManifest& manifest)
	{
		std::lock_guard<std::mutex> lock(m_mutex);

		auto it = m_versions.find(versionId);
		if (it == m_versions.end())
		{
			LOG_ERROR(L"[BlockState] InitFullBlocks: version not found");
			return false;
		}

		m_totalBlocks = manifest.TotalBlocks;
		m_blocks.clear();
		m_blocks.reserve(m_totalBlocks);

		for (uint64_t i = 0; i < m_totalBlocks; i++)
		{
			BlockState bs;
			bs.BlockIndex = i;
			bs.Offset = i * BLOCK_SIZE;
			memcpy(bs.Hash, manifest.Entries[i].Hash, SHA256_SIZE);
			bs.VersionId = versionId;
			bs.Ack = AckStatus::Pending;
			bs.Changed = true;

			m_blocks.push_back(bs);
		}

		it->second.ChangedBlocks = m_totalBlocks;
		m_dirty = true;

		wchar_t msg[256];
		swprintf_s(msg, L"[BlockState] Full blocks initialized: %llu blocks for version %llu",
			m_totalBlocks, versionId);
		LOG_INFO(msg);

		return true;
	}

	// ============================================================
	// 更新增量备份块状态
	// ============================================================
	bool BlockStateManager::UpdateIncrementalBlocks(uint64_t versionId,
		const std::vector<uint64_t>& changedIndexes,
		const std::vector<BlockHash::BlockHashEntry>& hashes)
	{
		std::lock_guard<std::mutex> lock(m_mutex);

		if (changedIndexes.size() != hashes.size())
		{
			LOG_ERROR(L"[BlockState] UpdateIncrementalBlocks: index/hash count mismatch");
			return false;
		}

		if (m_blocks.empty())
		{
			LOG_ERROR(L"[BlockState] UpdateIncrementalBlocks: no existing blocks");
			return false;
		}

		auto it = m_versions.find(versionId);
		if (it == m_versions.end())
		{
			LOG_ERROR(L"[BlockState] UpdateIncrementalBlocks: version not found");
			return false;
		}

		// 先将所有块标记为未变化
		for (auto& bs : m_blocks)
		{
			bs.Changed = false;
			bs.Ack = AckStatus::Skipped;
		}

		uint64_t changedCount = 0;
		for (size_t i = 0; i < changedIndexes.size(); i++)
		{
			uint64_t idx = changedIndexes[i];
			if (idx >= m_blocks.size())
			{
				continue;
			}

			m_blocks[idx].VersionId = versionId;
			memcpy(m_blocks[idx].Hash, hashes[i].Hash, SHA256_SIZE);
			m_blocks[idx].Ack = AckStatus::Pending;
			m_blocks[idx].Changed = true;
			changedCount++;
		}

		it->second.ChangedBlocks = changedCount;
		it->second.AckedBlocks = 0;
		m_dirty = true;

		wchar_t msg[256];
		swprintf_s(msg, L"[BlockState] Incremental blocks updated: %llu/%zu changed for version %llu",
			changedCount, changedIndexes.size(), versionId);
		LOG_INFO(msg);

		return true;
	}

	// ============================================================
	// ACK 管理
	// ============================================================

	bool BlockStateManager::SetBlockAck(uint64_t blockIndex, AckStatus status)
	{
		std::lock_guard<std::mutex> lock(m_mutex);

		if (blockIndex >= m_blocks.size())
		{
			return false;
		}

		AckStatus oldStatus = m_blocks[blockIndex].Ack;
		m_blocks[blockIndex].Ack = status;
		m_dirty = true;

		// 更新版本统计
		if (oldStatus != AckStatus::Acknowledged && status == AckStatus::Acknowledged)
		{
			uint64_t vid = m_blocks[blockIndex].VersionId;
			auto it = m_versions.find(vid);
			if (it != m_versions.end())
			{
				it->second.AckedBlocks++;
			}
		}

		return true;
	}

	bool BlockStateManager::SetBlockAckRange(uint64_t startBlock, uint64_t count, AckStatus status)
	{
		std::lock_guard<std::mutex> lock(m_mutex);

		uint64_t end = (std::min)(startBlock + count, (uint64_t)m_blocks.size());
		for (uint64_t i = startBlock; i < end; i++)
		{
			AckStatus oldStatus = m_blocks[i].Ack;
			m_blocks[i].Ack = status;

			if (oldStatus != AckStatus::Acknowledged && status == AckStatus::Acknowledged)
			{
				uint64_t vid = m_blocks[i].VersionId;
				auto it = m_versions.find(vid);
				if (it != m_versions.end())
				{
					it->second.AckedBlocks++;
				}
			}
		}

		m_dirty = true;
		return true;
	}

	bool BlockStateManager::SetBlockAckList(const std::vector<uint64_t>& blockIndexes, AckStatus status)
	{
		for (uint64_t idx : blockIndexes)
		{
			SetBlockAck(idx, status);
		}
		return true;
	}

	// ============================================================
	// 查询
	// ============================================================

	BlockState BlockStateManager::GetBlockState(uint64_t blockIndex) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);

		if (blockIndex < m_blocks.size())
		{
			return m_blocks[blockIndex];
		}

		BlockState empty;
		memset(&empty, 0, sizeof(empty));
		return empty;
	}

	std::vector<BlockState> BlockStateManager::GetBlocksByAck(AckStatus status) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		std::vector<BlockState> result;

		for (const auto& bs : m_blocks)
		{
			if (bs.Ack == status)
			{
				result.push_back(bs);
			}
		}
		return result;
	}

	std::vector<BlockState> BlockStateManager::GetPendingBlocks() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		std::vector<BlockState> result;

		for (const auto& bs : m_blocks)
		{
			if (bs.NeedsUpload())
			{
				result.push_back(bs);
			}
		}
		return result;
	}

	uint64_t BlockStateManager::GetLastAcknowledgedBlock() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);

		// 从头扫描，找到最后一个连续 ACK 块
		uint64_t lastAcked = 0;
		for (uint64_t i = 0; i < m_blocks.size(); i++)
		{
			if (m_blocks[i].Ack == AckStatus::Acknowledged || m_blocks[i].Ack == AckStatus::Skipped)
			{
				lastAcked = i + 1;
			}
			else
			{
				break;
			}
		}
		return lastAcked;
	}

	std::vector<VersionRecord> BlockStateManager::GetVersionHistory()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		std::vector<VersionRecord> result;

		for (const auto& pair : m_versions)
		{
			result.push_back(pair.second);
		}

		// 按版本 ID 升序排列
		std::sort(result.begin(), result.end(),
			[](const VersionRecord& a, const VersionRecord& b) {
				return a.VersionId < b.VersionId;
			});

		return result;
	}

	VersionRecord BlockStateManager::GetVersion(uint64_t versionId) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);

		auto it = m_versions.find(versionId);
		if (it != m_versions.end())
		{
			return it->second;
		}

		VersionRecord empty;
		memset(&empty, 0, sizeof(empty));
		return empty;
	}

	std::vector<BlockState> BlockStateManager::GetBlockHistory(uint64_t blockIndex) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		std::vector<BlockState> result;

		if (blockIndex >= m_blocks.size())
		{
			return result;
		}

		// 当前实现返回单个块的历史（未来可扩展为多版本快照链）
		result.push_back(m_blocks[blockIndex]);

		return result;
	}

	// ============================================================
	// 持久化
	// ============================================================

	std::wstring BlockStateManager::GetStateFilePath() const
	{
		wchar_t path[MAX_PATH];
		swprintf_s(path, L"%s\\block_state_%d.dat", m_stateDir.c_str(), m_devNo);
		return std::wstring(path);
	}

	bool BlockStateManager::Save()
	{
		if (!m_initialized)
		{
			return false;
		}

		std::lock_guard<std::mutex> lock(m_mutex);

		std::wstring filePath = GetStateFilePath();

		HANDLE hFile = CreateFileW(
			filePath.c_str(),
			GENERIC_WRITE,
			0,
			nullptr,
			CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			nullptr
		);

		if (hFile == INVALID_HANDLE_VALUE)
		{
			DWORD err = GetLastError();
			wchar_t msg[256];
			swprintf_s(msg, L"[BlockState] Failed to create state file: %s, error=%lu",
				filePath.c_str(), err);
			LOG_ERROR(msg);
			return false;
		}

		// ---- Header: 64 bytes ----
		uint8_t header[HEADER_SIZE];
		memset(header, 0, HEADER_SIZE);

		size_t pos = 0;
		auto writeHdr = [&](const void* data, size_t size) {
			if (pos + size <= HEADER_SIZE) {
				memcpy(header + pos, data, size);
				pos += size;
			}
		};

		uint32_t magic = STATE_FILE_MAGIC;
		uint32_t version = STATE_FILE_VERSION;
		uint32_t devNo = (uint32_t)m_devNo;
		uint64_t blockSize = BLOCK_SIZE;
		uint64_t totalBlocks = m_totalBlocks;
		uint32_t versionCount = (uint32_t)m_versions.size();
		uint32_t blockRecordSize = (uint32_t)BLOCK_RECORD_SIZE;

		writeHdr(&magic, 4);
		writeHdr(&version, 4);
		writeHdr(&devNo, 4);
		writeHdr(&blockRecordSize, 4);
		writeHdr(&blockSize, 8);
		writeHdr(&totalBlocks, 8);
		writeHdr(&versionCount, 4);
		// padding 28 bytes (already zeroed)

		DWORD written = 0;
		WriteFile(hFile, header, HEADER_SIZE, &written, nullptr);

		// ---- Block Records ----
		std::vector<uint8_t> recordBuf(BLOCK_RECORD_SIZE);
		for (const auto& bs : m_blocks)
		{
			SerializeBlock(bs, recordBuf.data());
			WriteFile(hFile, recordBuf.data(), BLOCK_RECORD_SIZE, &written, nullptr);
		}

		// ---- Version Table ----
		// 每条 VersionRecord 序列化后写入
		for (const auto& pair : m_versions)
		{
			const auto& ver = pair.second;

			uint64_t verId = ver.VersionId;
			uint32_t dev = (uint32_t)ver.DevNo;
			uint64_t tBlocks = ver.TotalBlocks;
			uint64_t cBlocks = ver.ChangedBlocks;
			uint64_t tSize = ver.TotalSize;
			uint64_t aBlocks = ver.AckedBlocks;
			uint64_t snapTime = ((uint64_t)ver.SnapshotTime.dwHighDateTime << 32) | ver.SnapshotTime.dwLowDateTime;
			uint64_t createTime = ver.CreateTimestamp;

			WriteFile(hFile, &verId, 8, &written, nullptr);
			WriteFile(hFile, &dev, 4, &written, nullptr);

			// 版本类型字符串（UTF-16LE，定长 32 字节 = 16 个 wchar_t）
			wchar_t typeBuf[16] = {};
			wcsncpy_s(typeBuf, ver.VersionType.c_str(), 15);
			WriteFile(hFile, typeBuf, sizeof(typeBuf), &written, nullptr);

			WriteFile(hFile, &snapTime, 8, &written, nullptr);
			WriteFile(hFile, &tBlocks, 8, &written, nullptr);
			WriteFile(hFile, &cBlocks, 8, &written, nullptr);
			WriteFile(hFile, &tSize, 8, &written, nullptr);
			WriteFile(hFile, &aBlocks, 8, &written, nullptr);
			WriteFile(hFile, &createTime, 8, &written, nullptr);
		}

		CloseHandle(hFile);

		m_dirty = false;

		wchar_t msg[256];
		swprintf_s(msg, L"[BlockState] State saved: %ls (%zu blocks, %zu versions)",
			filePath.c_str(), m_blocks.size(), m_versions.size());
		LOG_INFO(msg);

		return true;
	}

	bool BlockStateManager::Load()
	{
		std::wstring filePath = GetStateFilePath();

		HANDLE hFile = CreateFileW(
			filePath.c_str(),
			GENERIC_READ,
			FILE_SHARE_READ,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			nullptr
		);

		if (hFile == INVALID_HANDLE_VALUE)
		{
			return false;  // 文件不存在，首次初始化
		}

		// ---- Header ----
		uint8_t header[HEADER_SIZE];
		DWORD read = 0;
		if (!ReadFile(hFile, header, HEADER_SIZE, &read, nullptr) || read < HEADER_SIZE)
		{
			CloseHandle(hFile);
			return false;
		}

		size_t pos = 0;
		auto readHdr = [&](void* data, size_t size) {
			if (pos + size <= HEADER_SIZE) {
				memcpy(data, header + pos, size);
				pos += size;
			}
		};

		uint32_t magic = 0, version = 0, devNo = 0, blockRecordSize = 0;
		uint64_t blockSize = 0;
		readHdr(&magic, 4);
		readHdr(&version, 4);
		readHdr(&devNo, 4);
		readHdr(&blockRecordSize, 4);
		readHdr(&blockSize, 8);
		readHdr(&m_totalBlocks, 8);
		readHdr(&version, 4);  // VersionCount (reuse variable)

		if (magic != STATE_FILE_MAGIC || blockRecordSize != BLOCK_RECORD_SIZE)
		{
			wchar_t msg[256];
			swprintf_s(msg, L"[BlockState] Invalid state file magic or record size: %s", filePath.c_str());
			LOG_ERROR(msg);
			CloseHandle(hFile);
			return false;
		}

		m_devNo = (int)devNo;

		uint32_t versionCount = version;

		// ---- Block Records ----
		m_blocks.clear();
		m_blocks.reserve(m_totalBlocks);

		std::vector<uint8_t> recordBuf(BLOCK_RECORD_SIZE);
		for (uint64_t i = 0; i < m_totalBlocks; i++)
		{
			if (!ReadFile(hFile, recordBuf.data(), BLOCK_RECORD_SIZE, &read, nullptr) || read < BLOCK_RECORD_SIZE)
			{
				break;
			}

			BlockState bs;
			DeserializeBlock(recordBuf.data(), bs);
			m_blocks.push_back(bs);
		}

		// ---- Version Table ----
		m_versions.clear();
		m_nextVersionId = 0;

		for (uint32_t i = 0; i < versionCount; i++)
		{
			uint64_t verId, tBlocks, cBlocks, tSize, aBlocks, snapTime, createTime;
			uint32_t dev;

			if (!ReadFile(hFile, &verId, 8, &read, nullptr) || read < 8) break;
			if (!ReadFile(hFile, &dev, 4, &read, nullptr) || read < 4) break;

			wchar_t typeBuf[16] = {};
			if (!ReadFile(hFile, typeBuf, sizeof(typeBuf), &read, nullptr) || read < sizeof(typeBuf)) break;

			if (!ReadFile(hFile, &snapTime, 8, &read, nullptr) || read < 8) break;
			if (!ReadFile(hFile, &tBlocks, 8, &read, nullptr) || read < 8) break;
			if (!ReadFile(hFile, &cBlocks, 8, &read, nullptr) || read < 8) break;
			if (!ReadFile(hFile, &tSize, 8, &read, nullptr) || read < 8) break;
			if (!ReadFile(hFile, &aBlocks, 8, &read, nullptr) || read < 8) break;
			if (!ReadFile(hFile, &createTime, 8, &read, nullptr) || read < 8) break;

			VersionRecord ver;
			ver.VersionId = verId;
			ver.DevNo = (int)dev;
			ver.VersionType = typeBuf;
			ver.SnapshotTime.dwHighDateTime = (DWORD)(snapTime >> 32);
			ver.SnapshotTime.dwLowDateTime = (DWORD)(snapTime & 0xFFFFFFFF);
			ver.TotalBlocks = tBlocks;
			ver.ChangedBlocks = cBlocks;
			ver.TotalSize = tSize;
			ver.AckedBlocks = aBlocks;
			ver.CreateTimestamp = createTime;

			m_versions[verId] = ver;

			if (verId >= m_nextVersionId)
			{
				m_nextVersionId = verId + 1;
			}
		}

		CloseHandle(hFile);

		wchar_t msg[256];
		swprintf_s(msg, L"[BlockState] State loaded: %ls (%zu blocks, %zu versions)",
			filePath.c_str(), m_blocks.size(), m_versions.size());
		LOG_INFO(msg);

		return true;
	}

	// ============================================================
	// 二进制序列化: BlockState ↔ 48 字节
	//
	// 格式:
	//   Flags:      4 bytes  [bit0=Changed, bit1-3=AckStatus, rest=reserved]
	//   VersionId:  8 bytes
	//   Hash:      32 bytes
	//   Reserved:   4 bytes
	// ============================================================
	void BlockStateManager::SerializeBlock(const BlockState& state, uint8_t* out)
	{
		memset(out, 0, BLOCK_RECORD_SIZE);

		uint32_t flags = 0;
		if (state.Changed) flags |= 0x01;
		flags |= ((uint32_t)state.Ack & 0x07) << 1;

		memcpy(out, &flags, 4);
		memcpy(out + 4, &state.BlockIndex, 8);
		memcpy(out + 12, &state.VersionId, 8);
		memcpy(out + 20, state.Hash, SHA256_SIZE);
		// bytes 52-55: reserved
	}

	void BlockStateManager::DeserializeBlock(const uint8_t* data, BlockState& state)
	{
		memset(&state, 0, sizeof(state));

		uint32_t flags = 0;
		memcpy(&flags, data, 4);

		state.Changed = (flags & 0x01) != 0;
		state.Ack = (AckStatus)((flags >> 1) & 0x07);

		memcpy(&state.BlockIndex, data + 4, 8);
		memcpy(&state.VersionId, data + 12, 8);
		memcpy(state.Hash, data + 20, SHA256_SIZE);

		state.Offset = state.BlockIndex * BLOCK_SIZE;
	}

} // namespace BlockState
