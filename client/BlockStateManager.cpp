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

		// ============================================================
	// CRC32 查表计算（用于 Header 完整性校验）
	// ============================================================
	static uint32_t ComputeCRC32(const uint8_t* data, size_t size)
	{
		static const uint32_t table[256] = {
			0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3,
			0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988, 0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91,
			0x1DB71064, 0x6AB020F2, 0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
			0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5,
			0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172, 0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B,
			0x35B5A8FA, 0x42B2986C, 0xDBBBC9D6, 0xACBCF940, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
			0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423, 0xCFBA9599, 0xB8BDA50F,
			0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924, 0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D,
			0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
			0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D, 0x91646C97, 0xE6635C01,
			0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E, 0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457,
			0x65B0D9C6, 0x12B7E950, 0x8BBEB8EA, 0xFCB9887C, 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
			0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2, 0x4ADFA541, 0x3DD895D7, 0xA4D1C46D, 0xD3D6F4FB,
			0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0, 0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9,
			0x5005713C, 0x270241AA, 0xBE0B1010, 0xC90C2086, 0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
			0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81, 0xB7BD5C3B, 0xC0BA6CAD,
			0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A, 0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683,
			0xE3630B12, 0x94643B84, 0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
			0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D, 0x806567CB, 0x196C3671, 0x6E6B06E7,
			0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC, 0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5,
			0xD6D6A3E8, 0xA1D1937E, 0x38D8C2C4, 0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
			0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60, 0xDF60EFC3, 0xA867DF55, 0x316E8EEF, 0x4669BE79,
			0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236, 0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F,
			0xC5BA3BBE, 0xB2BD0B28, 0x2BB45A92, 0x5CB30A04, 0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
			0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F, 0x72676785, 0x05605713,
			0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38, 0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21,
			0x86D3D2D4, 0xF1D4E242, 0x68DDB3F8, 0x1FDA836E, 0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
			0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C, 0x8F659EFF, 0xF862AE69, 0x616BFFD3, 0x166CCF45,
			0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2, 0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB,
			0xAED16A4A, 0xD9D65ADC, 0x40BF0B66, 0x37B83BF0, 0xA9BCAE53, 0xDEBB9EC5, 0x47A2CF7F, 0x30A5FFE9,
			0xBDBDF21C, 0xCABAC28A, 0x539BD330, 0x249CF3A6, 0xBA2D6605, 0xCD2A7693, 0x54052729, 0x230236BF,
			0xB300352E, 0xC40713B8, 0x5D0E4202, 0x2A09F294, 0xB4CF6737, 0xC3C857A1, 0x5AEF061B, 0x2DE8F58D
		};

		uint32_t crc = 0xFFFFFFFF;
		for (size_t i = 0; i < size; i++)
		{
			crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xFF];
		}
		return crc ^ 0xFFFFFFFF;
	}

	bool BlockStateManager::Save()
	{
		if (!m_initialized)
		{
			return false;
		}

		std::lock_guard<std::mutex> lock(m_mutex);

		std::wstring filePath = GetStateFilePath();
		std::wstring tempPath = filePath + L".tmp";

		// ============================================================
		// Step 1: 写入临时文件（原子写入模式）
		// - FILE_FLAG_WRITE_THROUGH: 绕过磁盘缓存，直接写入介质
		// - 写入中断时原始文件不受影响
		// ============================================================
		HANDLE hFile = CreateFileW(
			tempPath.c_str(),
			GENERIC_WRITE,
			0,                           // 独占写入
			nullptr,
			CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
			nullptr
		);

		if (hFile == INVALID_HANDLE_VALUE)
		{
			DWORD err = GetLastError();
			wchar_t msg[256];
			swprintf_s(msg, L"[BlockState] Failed to create temp file: %s, error=%lu",
				tempPath.c_str(), err);
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
		uint32_t fileVer = STATE_FILE_VERSION;
		uint32_t devNo = (uint32_t)m_devNo;
		uint64_t blockSize = BLOCK_SIZE;
		uint64_t totalBlocks = m_totalBlocks;
		uint32_t versionCount = (uint32_t)m_versions.size();
		uint32_t blockRecordSize = (uint32_t)BLOCK_RECORD_SIZE;
		uint32_t crc32 = 0; // 占位，后续回填

		writeHdr(&magic, 4);
		writeHdr(&fileVer, 4);
		writeHdr(&devNo, 4);
		writeHdr(&blockRecordSize, 4);
		writeHdr(&blockSize, 8);
		writeHdr(&totalBlocks, 8);
		writeHdr(&versionCount, 4);
		writeHdr(&crc32, 4);    // CRC32 占位 (offset 36)
		// padding 24 bytes

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

		// ============================================================
		// Step 2: 计算并回写 CRC32（Header 完整性校验）
		// CRC32 覆盖 Header 中除 CRC32 字段本身之外的所有字节
		// ============================================================
		{
			uint32_t dataCRC = ComputeCRC32(header, 36);  // bytes 0-35
			memcpy(header + 36, &dataCRC, 4);
		}

		// ============================================================
		// Step 3: Flush + 回写带 CRC 的 Header
		// FlushFileBuffers 确保操作系统缓存刷入物理介质
		// ============================================================
		LARGE_INTEGER zero = {};
		SetFilePointerEx(hFile, zero, nullptr, FILE_BEGIN);
		WriteFile(hFile, header, HEADER_SIZE, &written, nullptr);

		// 强制刷盘
		if (!FlushFileBuffers(hFile))
		{
			DWORD err = GetLastError();
			wchar_t msg[256];
			swprintf_s(msg, L"[BlockState] FlushFileBuffers failed: error=%lu", err);
			LOG_WARNING(msg);
			// 继续——某些文件系统不支持 FlushFileBuffers
		}

		CloseHandle(hFile);

		// ============================================================
		// Step 4: 原子替换——用临时文件替换正式文件
		// MoveFileExW + MOVEFILE_REPLACE_EXISTING:
		//   - 系统崩溃时 .dat 保持旧版本，.dat.tmp 残留
		//   - MoveFileEx 是 NTFS 元数据操作，要么完成要么不做
		// ============================================================
		if (!MoveFileExW(tempPath.c_str(), filePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			DWORD err = GetLastError();
			wchar_t msg[256];
			swprintf_s(msg, L"[BlockState] MoveFileEx failed: %s -> %s, error=%lu",
				tempPath.c_str(), filePath.c_str(), err);
			LOG_ERROR(msg);
			DeleteFileW(tempPath.c_str());
			return false;
		}

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

		// 验证 CRC32（Header 完整性校验）
		uint32_t savedCRC = 0;
		memcpy(&savedCRC, header + 36, 4);
		// 临时清零 CRC32 字段，计算其余部分的 CRC
		memset(header + 36, 0, 4);
		uint32_t computedCRC = ComputeCRC32(header, HEADER_SIZE);
		memcpy(header + 36, &savedCRC, 4);  // 恢复

		if (savedCRC != computedCRC)
		{
			wchar_t msg[256];
			swprintf_s(msg, L"[BlockState] CRC32 mismatch: saved=0x%08X computed=0x%08X",
				savedCRC, computedCRC);
			LOG_ERROR(msg);
			CloseHandle(hFile);
			return false;
		}

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
