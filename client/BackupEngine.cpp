#include "BackupEngine.h"

#include "BlockHasher.h"

#include "BlockStateManager.h"

#include "CbtClient.h"

#include "DataCompressor.h"

#include "NetworkClient.h"

#include "VolumeMapper.h"

#include "VssManager.h"

#include "Logger.h"

#include <string.h>




namespace BackupEngine

{



		BackupEngine::BackupEngine()
			: m_windowSize(0)
			, m_window(nullptr)
			, m_inFlightCount(0)
			, m_ackedCount(0)
			, m_failedCount(0)
			, m_pipelineError(false)
		{

		}



		BackupEngine::~BackupEngine()
		{
			delete[] m_window;
			m_window = nullptr;
		}



		// ============================================================

		// 执行备份入口

		// ============================================================

	bool BackupEngine::Run(const BackupConfig& config,

		const std::vector<int>& devNos,

		std::vector<BackupStats>& stats)

	{

		LOG_INFO(L"[BackupEngine] Starting backup run");



		bool allOk = true;



		for (int devNo : devNos)

		{

			BackupStats diskStats;

			diskStats.DevNo = devNo;



			wchar_t msg[128];

			swprintf_s(msg, L"[BackupEngine] Processing Disk%d...", devNo);

			LOG_INFO(msg);



			if (BackupDisk(config, devNo, diskStats))

			{

				swprintf_s(msg, L"[BackupEngine] Disk%d complete: %llu blocks sent, %llu acked",
					devNo, diskStats.SentBlocks, diskStats.AckedBlocks);

				LOG_INFO(msg);

			}

			else

			{

				swprintf_s(msg, L"[BackupEngine] Disk%d FAILED", devNo);

				LOG_ERROR(msg);

				allOk = false;

			}



			stats.push_back(diskStats);

		}



		return allOk;

	}



		// ============================================================

		// 备份单个磁盘

		// ============================================================

	bool BackupEngine::BackupDisk(const BackupConfig& config, int devNo, BackupStats& stats)

	{

		// Step 1: 解析磁盘布局（含四分类）

		Disk::DiskParser parser;

		if (!parser.Open(devNo))

		{

			LOG_ERROR(L"[BackupEngine] Failed to open disk");

			return false;

		}



		Disk::DiskLayout layout;

		if (!parser.Parse(layout))

		{

			parser.Close();

			LOG_ERROR(L"[BackupEngine] Failed to parse disk layout");

			return false;

		}

		parser.Close();



		// Step 2: 根据备份类型分发

		if (config.UseCbt)

		{

			return IncrementalBackup(config, devNo, layout, stats);

		}

		else

		{

			return FullBackup(config, devNo, layout, stats);

		}

	}



		// ============================================================

		// 全量备份（流水线模式）
		// 流程：VSS快照 → 连接服务器+HELLO → 逐块(读→hash→压缩→发送) → 断开
		// 不再 BuildManifest 一次性算所有 hash，改为边算边传
		// ============================================================

	bool BackupEngine::FullBackup(const BackupConfig& config, int devNo,

		Disk::DiskLayout& layout, BackupStats& stats)

	{

		stats.VssUsed = true;



		// ---- VSS 快照 ----

		VolumeMapping::VolumeMapper mapper;

		mapper.Map(layout);



		VssSnapshot::VssManager vss;

		if (!vss.Initialize())

		{

			LOG_ERROR(L"[BackupEngine] VSS initialization failed");

			return false;

		}

		// 启动 VSS 快照集（必须在 AddVolumeToSnapshotSet 之前调用）

		if (!vss.StartSnapshotSet())

		{

			LOG_ERROR(L"[BackupEngine] VSS StartSnapshotSet failed");

			vss.Cleanup();

			return false;

		}



		// 卷映射

		// 添加文件系统卷到快照集

		LOG_DEBUG(L"[BackupEngine] Adding filesystem volumes to VSS snapshot set...");

		bool hasFilesystem = false;

		int fsAdded = 0, fsSkipped = 0;

		for (const auto& mp : mapper.GetMappedPartitions())

		{

			auto content = mp.Partition.Content;

			const wchar_t* fsLabel = L"?";

			bool isFS = false;

			if (content == Disk::PartitionContent::FilesystemNTFS)

				{ fsLabel = L"NTFS"; isFS = true; }

			else if (content == Disk::PartitionContent::FilesystemFAT32)

				{ fsLabel = L"FAT32"; isFS = true; }

			else if (content == Disk::PartitionContent::FilesystemExFAT)

				{ fsLabel = L"exFAT"; isFS = true; }

			else if (content == Disk::PartitionContent::FilesystemReFS)

				{ fsLabel = L"ReFS"; isFS = true; }



			if (isFS)

			{

				VSS_ID setId;

				{

					wchar_t dbg[512];

					swprintf_s(dbg, L"[BackupEngine]   Adding to snapshot set: %s (drive=%s offset=0x%llx size=%llu)",
						fsLabel,
						mp.DriveLetter.empty() ? L"none" : mp.DriveLetter.c_str(),
						mp.Partition.Offset, mp.Partition.Size);

					LOG_DEBUG(dbg);

				}

				if (vss.AddVolumeToSnapshotSet(mp.VolumeGuid, setId))

				{

					hasFilesystem = true;

					fsAdded++;

				}

				else

				{

					fsSkipped++;



					wchar_t dbg[256];

					swprintf_s(dbg, L"[BackupEngine]   FAILED to add %s to snapshot set", fsLabel);

					LOG_WARNING(dbg);

				}

			}

		}

		{

			wchar_t dbg[128];

			swprintf_s(dbg, L"[BackupEngine] Snapshot set summary: %d added, %d failed (%d non-FS skipped)",
				fsAdded, fsSkipped, (int)mapper.GetMappedPartitions().size() - fsAdded - fsSkipped);

			LOG_DEBUG(dbg);

		}

		// VSS 快照失败则中止备份

		if (fsSkipped > 0 && fsAdded == 0)

		{

			LOG_ERROR(L"[BackupEngine] VSS snapshot set failed - all volumes rejected, aborting backup");

			vss.Cleanup();

			return false;

		}



		if (hasFilesystem)

		{

			if (!vss.DoSnapshotSet())

			{

				LOG_ERROR(L"[BackupEngine] VSS snapshot creation failed");

				vss.Cleanup();

				return false;

			}

			LOG_DEBUG(L"[BackupEngine] DoSnapshotSet completed successfully");

		}

		else

		{

			stats.VssUsed = false;

		}



		// ---- 计算基本信息 ----

		uint64_t totalSize = layout.Disk.Size;

		uint64_t totalBlocks = (totalSize + BlockHash::BLOCK_SIZE - 1) / BlockHash::BLOCK_SIZE;

		// 数据源：有文件系统分区时优先 VSS 快照，否则回退物理盘

		std::wstring dataSource;

		for (const auto& mp : mapper.GetMappedPartitions())

		{

			std::wstring snapPath;

			if (vss.GetSnapshotDevicePath(mp.VolumeGuid, snapPath) && !snapPath.empty())

			{

				dataSource = snapPath;

				LOG_INFO(L"[BackupEngine] Using VSS snapshot as data source");

				break;

			}

		}

		if (dataSource.empty())

		{

			wchar_t diskBuf[64];

			swprintf_s(diskBuf, L"\\\\.\\PhysicalDrive%d", devNo);

			dataSource = diskBuf;

			LOG_INFO(L"[BackupEngine] Using physical disk as data source");

		}



		// ---- 创建版本 ----

		BlockState::BlockStateManager state;

		state.Initialize(config.StateDir, devNo);



		FILETIME snapshotTime;

		GetSystemTimeAsFileTime(&snapshotTime);



		auto ver = state.CreateVersion(L"FULL", snapshotTime, totalBlocks, totalSize);



		// ---- 预分配块状态（流水线模式：先分配，hash 后填充） ----

		state.InitFullBlocksEmpty((uint64_t)ver.VersionId, totalBlocks);



		// ---- 连接服务器+发送 HELLO ----

		DataCompress::DataCompressor compressor;

		compressor.Initialize();



		Network::NetworkClient client;



		if (!config.DryRun)

		{

			LOG_INFO(L"[BackupEngine] Connecting to server...");

			if (!client.Connect(config.ServerIp, config.Port, config.TimeoutSec))

			{

				LOG_ERROR(L"[BackupEngine] Connect to server failed");

				vss.Cleanup();

				return false;

			}



			if (!client.SendHello((uint32_t)ver.VersionId, devNo, totalBlocks, totalSize, L"FULL"))

			{

				LOG_ERROR(L"[BackupEngine] SendHello failed - server rejected handshake");

				client.Disconnect();

				vss.Cleanup();

				state.Save();

				return false;

			}

			LOG_INFO(L"[BackupEngine] Server connection established, starting pipeline transfer...");

		}



		// ---- 初始化 hash 模块 ----

		BlockHash::BlockHasher hasher;

		hasher.Initialize();



		stats.TotalBlocks = totalBlocks;

		stats.ChangedBlocks = totalBlocks;

		stats.TotalBytes = totalSize;



		// ---- 流水线传输（核心改动：不再 BuildManifest，边算 hash 边传输） ----

		uint64_t sent = PipelineTransferBlocks(config, devNo, hasher, compressor, client, state,
			(uint32_t)ver.VersionId, totalBlocks, totalSize, dataSource, stats);



		stats.SentBlocks = sent;

		CloseConnection(client, devNo);



		vss.Cleanup();

		state.Save();



		return true;

	}



		// ============================================================

		// 增量备份
		// 流程：CBT查询 → VSS快照 → 连接服务器+HELLO → 计算Hash → 传输数据 → 断开
		// ============================================================

	bool BackupEngine::IncrementalBackup(const BackupConfig& config, int devNo,

		Disk::DiskLayout& layout, BackupStats& stats)

	{

		stats.VssUsed = true;



		// ---- CBT 查询变更块 ----

		CbtDriver::CbtClient cbt;

		if (!cbt.Connect())

		{

			LOG_ERROR(L"[BackupEngine] CBT driver connection failed");

			return false;

		}



		std::vector<uint8_t> bitmap;

		ULONGLONG totalBits = 0;

		if (!cbt.Query(devNo, bitmap, totalBits))

		{

			LOG_ERROR(L"[BackupEngine] CBT query failed");

			return false;

		}



		std::vector<uint64_t> cbtChanged;

		CbtDriver::CbtClient::ParseChangedBlocks(bitmap, totalBits, cbtChanged);



		if (cbtChanged.empty())

		{

			LOG_INFO(L"[BackupEngine] No changed blocks detected by CBT");

			cbt.Disconnect();

			return true;

		}



		// ---- VSS 快照 ----

		VssSnapshot::VssManager vss;

		if (!vss.Initialize())

		{

			cbt.Disconnect();

			return false;

		}

		// 启动 VSS 快照集（必须在 AddVolumeToSnapshotSet 之前调用）

		if (!vss.StartSnapshotSet())

		{

			LOG_ERROR(L"[BackupEngine] VSS StartSnapshotSet failed for incremental");

			vss.Cleanup();

			cbt.Disconnect();

			return false;

		}



		VolumeMapping::VolumeMapper mapper;

		mapper.Map(layout);



		bool hasFilesystem = false;

		int fsAdded = 0, fsSkipped = 0;

		for (const auto& mp : mapper.GetMappedPartitions())

		{

			auto content = mp.Partition.Content;

			if (content == Disk::PartitionContent::FilesystemNTFS ||
				content == Disk::PartitionContent::FilesystemFAT32 ||
				content == Disk::PartitionContent::FilesystemExFAT ||
				content == Disk::PartitionContent::FilesystemReFS)

			{

				VSS_ID setId;

				if (vss.AddVolumeToSnapshotSet(mp.VolumeGuid, setId))

				{

					hasFilesystem = true;

					fsAdded++;

				}

				else

				{

					fsSkipped++;

				}

			}

		}

		// VSS 快照失败则中止备份

		if (fsSkipped > 0 && fsAdded == 0)

		{

			LOG_ERROR(L"[BackupEngine] VSS snapshot set failed - all volumes rejected, aborting incremental backup");

			vss.Cleanup();

			cbt.Disconnect();

			return false;

		}



		if (hasFilesystem)

		{

			if (!vss.DoSnapshotSet())

			{

				LOG_ERROR(L"[BackupEngine] VSS DoSnapshotSet failed for incremental");

				vss.Cleanup();

				cbt.Disconnect();

				return false;

			}

		}

		else

		{

			stats.VssUsed = false;

		}



		// ---- 创建增量版本 ----

		BlockState::BlockStateManager state;

		state.Initialize(config.StateDir, devNo);



		BlockHash::BlockHasher hasher;

		hasher.Initialize();



		uint64_t totalBlocks = state.GetTotalBlocks();

		uint64_t totalSize = totalBlocks * BlockHash::BLOCK_SIZE;



		FILETIME snapshotTime;

		GetSystemTimeAsFileTime(&snapshotTime);



		uint64_t versionId = 0;

		auto history = state.GetVersionHistory();

		if (!history.empty())

		{

			versionId = history.back().VersionId + 1;

		}



		auto ver = state.CreateVersion(L"INCREMENTAL", snapshotTime, totalBlocks, totalSize);



		// ---- ★ 先连接服务器+发送 HELLO，验证连接后再做耗时的 hash 计算 ----

		DataCompress::DataCompressor compressor;

		compressor.Initialize();



		Network::NetworkClient client;



		if (!config.DryRun)

		{

			LOG_INFO(L"[BackupEngine] Connecting to server BEFORE hash computation...");

			if (!client.Connect(config.ServerIp, config.Port, config.TimeoutSec))

			{

				LOG_ERROR(L"[BackupEngine] Connect to server failed - aborting before hash computation");

				vss.Cleanup();

				cbt.Disconnect();

				state.Save();

				return false;

			}



			if (!client.SendHello((uint32_t)ver.VersionId, devNo, totalBlocks, totalSize, L"INCREMENTAL"))

			{

				LOG_ERROR(L"[BackupEngine] SendHello failed - server rejected handshake");

				client.Disconnect();

				vss.Cleanup();

				cbt.Disconnect();

				state.Save();

				return false;

			}

			LOG_INFO(L"[BackupEngine] Server connection established, starting hash computation...");

		}



		// ---- 计算变化块哈希（耗时操作，连接已建立） ----

		wchar_t readPath[64];

		swprintf_s(readPath, L"\\\\.\\PhysicalDrive%d", devNo);

		HANDLE hRead = CreateFileW(

			readPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,

			nullptr, OPEN_EXISTING, 0, nullptr);



		std::vector<BlockHash::BlockHashEntry> changedHashes;

		if (hRead != INVALID_HANDLE_VALUE)

		{

			BlockHash::HashManifest dummyManifest;

			dummyManifest.TotalBlocks = totalBlocks;

			dummyManifest.BlockSize = BlockHash::BLOCK_SIZE;

			dummyManifest.TotalSize = totalSize;

			dummyManifest.Entries.resize(totalBlocks);



			hasher.ComputeBlockHashes(hRead, dummyManifest, cbtChanged, changedHashes);

			CloseHandle(hRead);

		}



		state.UpdateIncrementalBlocks(ver.VersionId, cbtChanged, changedHashes);



		stats.TotalBlocks = totalBlocks;

		stats.ChangedBlocks = cbtChanged.size();

		stats.TotalBytes = totalSize;



		// ---- 数据源确定 ----

		std::wstring dataSource;

		// 优先使用 VSS 快照路径

		for (const auto& mp : mapper.GetMappedPartitions())

		{

			std::wstring snapPath;

			if (vss.GetSnapshotDevicePath(mp.VolumeGuid, snapPath) && !snapPath.empty())

			{

				dataSource = snapPath;

				break;

			}

		}

		if (dataSource.empty())

		{

			wchar_t diskBuf[64];

			swprintf_s(diskBuf, L"\\\\.\\PhysicalDrive%d", devNo);

			dataSource = diskBuf;

		}



		// ---- 传输数据块 ----

		uint64_t sent = TransferBlocks(config, devNo, hasher, compressor, client, state,
			(uint32_t)ver.VersionId, totalBlocks, dataSource, stats);



		stats.SentBlocks = sent;

		CloseConnection(client, devNo);



		// 清零 CBT 位图

		cbt.Reset(devNo);

		cbt.Disconnect();



		vss.Cleanup();

		state.Save();



		return true;

	}



		// ============================================================

		// 流水线传输（全量备份专用）
		//
		// 核心思路：
		// - 逐块处理：读 → 算 hash → 压缩 → 非阻塞发送
		// - 环形窗口：窗口 N 个槽位，不等 ACK 就发下一块
		// - ACK 线程：独立线程 recv ACK，回调释放窗口 + 更新状态
		// - hash 清单和 block state 在传输过程中逐步构建
		//
		// 对比旧流程：
		// - 旧：BuildManifest（遍历磁盘算 hash）→ TransferBlocks（重新遍历磁盘发送）
		// - 新：一遍完成，读一次磁盘，边算 hash 边传输，省一半磁盘 I/O
		// ============================================================

	uint64_t BackupEngine::PipelineTransferBlocks(const BackupConfig& config, int devNo,

		BlockHash::BlockHasher& hasher,

		DataCompress::DataCompressor& compressor,

		Network::NetworkClient& client,

		BlockState::BlockStateManager& state,

		uint32_t versionId, uint64_t totalBlocks, uint64_t totalSize,

		const std::wstring& dataSourcePath,

		BackupStats& stats)

	{

		// ---- 初始化流水线窗口 ----

		m_windowSize = config.PipelineWindowSize;

		ResetPipelineWindow();



		wchar_t startMsg[256];

		swprintf_s(startMsg, L"[BackupEngine] Pipeline transfer: %llu blocks, window=%d",
			totalBlocks, m_windowSize);

		LOG_INFO(startMsg);



		// ---- 打开数据源 ----

		HANDLE hSource = CreateFileW(

			dataSourcePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,

			nullptr, OPEN_EXISTING, 0, nullptr);



		if (hSource == INVALID_HANDLE_VALUE)

		{

			LOG_ERROR(L"[BackupEngine] Failed to open data source for pipeline transfer");

			return 0;

		}



		// ---- 设置 ACK 回调 ----

		auto ackCallback = [this, &state](uint32_t cbDevNo, uint64_t blockIndex,
			const uint8_t hash[32], uint32_t status)

		{

			if (blockIndex == 0xFFFFFFFFFFFFFFFFULL)

			{

				// 特殊标记：ACK 线程遇到连接错误
				m_pipelineError = true;
				m_windowCv.notify_all();

				LOG_ERROR(L"[BackupEngine] ACK thread reported connection error");

				return;

			}



			if (status == 0)

			{

				// ACK 成功

				state.SetBlockAck(blockIndex, BlockState::AckStatus::Acknowledged);

				m_ackedCount++;

			}

			else

			{

				// 服务端返回错误

				state.SetBlockAck(blockIndex, BlockState::AckStatus::Failed);

				m_failedCount++;



				wchar_t msg[256];

				swprintf_s(msg, L"[BackupEngine] ACK error for block %llu: status=%u",
					blockIndex, status);

				LOG_WARNING(msg);

			}



			// 释放窗口槽位

			ReleaseSlot(blockIndex);

			m_inFlightCount--;

			m_windowCv.notify_all();

		};



		// ---- 启动 ACK 接收线程 ----

		if (!config.DryRun)

		{

			client.StartAckReceiver(ackCallback);

		}



		// ---- 流水线主循环 ----

		uint64_t sentCount = 0;

		uint64_t totalCompressed = 0;

		uint64_t skippedCount = 0;



		for (uint64_t i = 0; i < totalBlocks; i++)

		{

			// 检查流水线错误（ACK 线程检测到连接断开）

			if (m_pipelineError.load())

			{

				LOG_ERROR(L"[BackupEngine] Pipeline error detected, aborting transfer");

				break;

			}



			// 计算块大小（最后一个块可能不足 1MB）

			uint32_t blockSize = BlockHash::BLOCK_SIZE;

			if (i == totalBlocks - 1)

			{

				uint64_t remain = totalSize - i * BlockHash::BLOCK_SIZE;

				if (remain < BlockHash::BLOCK_SIZE && remain > 0)

				{

					blockSize = (uint32_t)remain;

				}

			}



			if (config.DryRun)

			{

				// ---- 模拟模式：不需要网络，直接标记成功 ----



				// 读取块数据

				std::vector<uint8_t> blockData(blockSize);

				bool readOk = false;

				LARGE_INTEGER pos;

				pos.QuadPart = i * BlockHash::BLOCK_SIZE;

				if (SetFilePointerEx(hSource, pos, nullptr, FILE_BEGIN))

				{

					DWORD read = 0;

					if (ReadFile(hSource, blockData.data(), blockSize, &read, nullptr) && read == blockSize)

					{

						readOk = true;

					}

				}



				if (!readOk)

				{

					stats.FailedBlocks++;

					continue;

				}



				// 算 hash

				uint8_t hash[32];

				if (!hasher.ComputeBlockHash(blockData.data(), blockSize, hash))

				{

					stats.FailedBlocks++;

					continue;

				}



				// 更新状态

				state.SetBlockHash(i, hash);

				state.SetBlockAck(i, BlockState::AckStatus::Acknowledged);

				sentCount++;



				// 进度日志

				if (sentCount % 100 == 0 && sentCount > 0)

				{

					double pct = (double)sentCount / (double)totalBlocks * 100.0;

					wchar_t progressMsg[256];

					swprintf_s(progressMsg, L"[BackupEngine] Pipeline progress (dryrun): %llu/%llu blocks (%.1f%%)",
						sentCount, totalBlocks, pct);

					LOG_INFO(progressMsg);

					state.Save();

				}



				continue;

			}



			// ---- 真实模式：获取窗口槽位（窗口满时阻塞等待） ----

			int slotIdx = AcquireSlot();

			if (slotIdx < 0)

			{

				// 超时或异常

				LOG_ERROR(L"[BackupEngine] AcquireSlot timed out or pipeline error");

				break;

			}



			auto& slot = m_window[slotIdx];

			slot.BlockIndex = i;

			slot.BlockData.resize(blockSize);

			slot.RawSize = blockSize;

			slot.SendFailed = false;



			// ---- 读取块数据 ----

			bool readOk = false;

			LARGE_INTEGER pos;

			pos.QuadPart = i * BlockHash::BLOCK_SIZE;

			if (SetFilePointerEx(hSource, pos, nullptr, FILE_BEGIN))

			{

				DWORD read = 0;

				if (ReadFile(hSource, slot.BlockData.data(), blockSize, &read, nullptr) && read == blockSize)

				{

					readOk = true;

				}

			}



			if (!readOk)

			{

				wchar_t failMsg[256];

				swprintf_s(failMsg, L"[BackupEngine] Block %llu read failed, skipping", i);

				LOG_WARNING(failMsg);



				// 释放槽位，跳过此块

				slot.InUse = false;

				slot.BlockData.clear();

				m_windowCv.notify_one();



				stats.FailedBlocks++;

				skippedCount++;

				continue;

			}



			// ---- 计算 SHA-256 哈希 ----

			if (!hasher.ComputeBlockHash(slot.BlockData.data(), blockSize, slot.Hash))

			{

				wchar_t failMsg[256];

				swprintf_s(failMsg, L"[BackupEngine] Block %llu hash failed, skipping", i);

				LOG_WARNING(failMsg);



				slot.InUse = false;

				slot.BlockData.clear();

				m_windowCv.notify_one();



				stats.FailedBlocks++;

				skippedCount++;

				continue;

			}



			// 更新块状态中的哈希（流水线逐步构建）

			state.SetBlockHash(i, slot.Hash);



			// ---- 压缩 ----

			if (!compressor.Compress(slot.BlockData.data(), blockSize, slot.Compressed))

			{

				wchar_t failMsg[256];

				swprintf_s(failMsg, L"[BackupEngine] Block %llu compress failed, skipping", i);

				LOG_WARNING(failMsg);



				slot.InUse = false;

				slot.BlockData.clear();

				m_windowCv.notify_one();



				stats.FailedBlocks++;

				skippedCount++;

				continue;

			}



			totalCompressed += slot.Compressed.size();



			// ---- 非阻塞发送（不等待 ACK，ACK 由后台线程接收） ----

			bool sent = client.SendBlockNoWait(

				devNo, i,

				slot.BlockData.data(), blockSize,

				slot.Compressed.data(), (uint32_t)slot.Compressed.size(),

				slot.Hash, versionId);



			if (!sent)

			{

				// 发送失败 = 网络断开，中止流水线

				wchar_t failMsg[256];

				swprintf_s(failMsg, L"[BackupEngine] Block %llu send failed - network error, aborting pipeline", i);

				LOG_ERROR(failMsg);



				slot.InUse = false;

				slot.BlockData.clear();

				m_windowCv.notify_one();



				m_pipelineError = true;

				break;

			}



			sentCount++;

			m_inFlightCount++;



			// 释放原始块数据内存（压缩后不再需要原始数据，节约内存）

			slot.BlockData.clear();

			slot.BlockData.shrink_to_fit();



			// 进度日志

			if (sentCount % 100 == 0 && sentCount > 0)

			{

				double pct = (double)sentCount / (double)totalBlocks * 100.0;

				wchar_t progressMsg[256];

				swprintf_s(progressMsg,
					L"[BackupEngine] Pipeline progress: %llu/%llu sent (%.1f%%), %llu acked, %d in-flight, compressed %llu bytes",
					sentCount, totalBlocks, pct,
					m_ackedCount.load(), m_inFlightCount.load(), totalCompressed);

				LOG_INFO(progressMsg);

				state.Save();

			}

		}



		// ---- 等待所有在途块的 ACK ----

		if (m_inFlightCount.load() > 0 && !m_pipelineError.load())

		{

			wchar_t waitMsg[256];

			swprintf_s(waitMsg, L"[BackupEngine] Waiting for %d in-flight ACKs...",
				m_inFlightCount.load());

			LOG_INFO(waitMsg);



			bool allAcked = WaitForAllAcks();

			if (!allAcked)

			{

				LOG_WARNING(L"[BackupEngine] Timeout waiting for in-flight ACKs");

			}

		}



		// ---- 停止 ACK 接收线程 ----

		if (!config.DryRun)

		{

			client.StopAckReceiver();

		}



		// ---- 关闭数据源 ----

		CloseHandle(hSource);



		// ---- 汇总统计 ----

		stats.SentBlocks = sentCount;

		stats.AckedBlocks = m_ackedCount.load();

		stats.FailedBlocks += m_failedCount.load();

		stats.SkippedBlocks = skippedCount;

		stats.CompressedBytes = totalCompressed;



		wchar_t msg[256];

		swprintf_s(msg, L"[BackupEngine] Pipeline complete: %llu sent, %llu acked, %llu failed, %llu skipped, compressed %llu bytes",
			sentCount, stats.AckedBlocks, stats.FailedBlocks, skippedCount, totalCompressed);

		LOG_INFO(msg);



		return sentCount;

	}



		// ============================================================

		// 同步传输块数据（增量备份使用，保留原有逻辑）
		// ============================================================

	uint64_t BackupEngine::TransferBlocks(const BackupConfig& config, int devNo,

		BlockHash::BlockHasher& hasher,

		DataCompress::DataCompressor& compressor,

		Network::NetworkClient& client,

		BlockState::BlockStateManager& state,

		uint32_t versionId, uint64_t totalBlocks,

		const std::wstring& dataSourcePath,

		BackupStats& stats)

	{

		uint64_t sentCount = 0;

		uint64_t totalCompressed = 0;



		// 读取源数据句柄（简化：从物理磁盘读取）

		wchar_t path[64];

		swprintf_s(path, L"\\\\.\\PhysicalDrive%d", devNo);



		HANDLE hSource = CreateFileW(

			dataSourcePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,

			nullptr, OPEN_EXISTING, 0, nullptr);



		// 遍历所有待处理块

		auto pending = state.GetPendingBlocks();

		uint64_t pendingCount = pending.size();

		if (pendingCount == 0)

		{

			LOG_WARNING(L"[BackupEngine] No pending blocks to transfer - check GetPendingBlocks/InitFullBlocks");

		}



		wchar_t startMsg[256];

		swprintf_s(startMsg, L"[BackupEngine] Starting transfer: %llu blocks pending (%llu total)",
			pendingCount, totalBlocks);

		LOG_INFO(startMsg);



		uint64_t blockIdx = 0;

		for (const auto& block : pending)

		{

			blockIdx++;

			uint32_t blockSize = BlockHash::BLOCK_SIZE;

			if (block.BlockIndex == totalBlocks - 1)

			{

				uint64_t remain = (uint64_t)totalBlocks * BlockHash::BLOCK_SIZE - block.Offset;

				if (remain < BlockHash::BLOCK_SIZE)

				{

					blockSize = (uint32_t)remain;

				}

			}



			// 读取块数据

			std::vector<uint8_t> blockData(blockSize);

			bool readOk = false;



			if (hSource != INVALID_HANDLE_VALUE)

			{

				LARGE_INTEGER pos;

				pos.QuadPart = block.Offset;

				if (SetFilePointerEx(hSource, pos, nullptr, FILE_BEGIN))

				{

					DWORD read = 0;

					if (ReadFile(hSource, blockData.data(), blockSize, &read, nullptr) && read == blockSize)

					{

						readOk = true;

					}

				}

			}



			if (!readOk && !config.DryRun)

			{

				wchar_t failMsg[256];

				swprintf_s(failMsg, L"[BackupEngine] Block %llu read failed, skipping", block.BlockIndex);

				LOG_WARNING(failMsg);

				continue;

			}



			// 压缩

			std::vector<uint8_t> compressed;

			if (!compressor.Compress(blockData.data(), blockSize, compressed))

			{

				wchar_t failMsg[256];

				swprintf_s(failMsg, L"[BackupEngine] Block %llu compress failed, skipping", block.BlockIndex);

				LOG_WARNING(failMsg);

				continue;

			}



			totalCompressed += compressed.size();



			// 发送（带重试）

			bool acked = false;

			for (int retry = 0; retry <= config.RetryCount; retry++)

			{

				if (config.DryRun)

				{

					acked = SimulatedAck(devNo, block.BlockIndex, block.Hash, state);

				}

				else

				{

					acked = client.SendBlock(devNo, block.BlockIndex,
						blockData.data(), blockSize,
						compressed.data(), (uint32_t)compressed.size(),
						block.Hash, versionId);

				}



				if (acked)

				{

					state.SetBlockAck(block.BlockIndex, BlockState::AckStatus::Acknowledged);

					sentCount++;

					stats.AckedBlocks++;



					break;

				}

				else

				{

					state.SetBlockAck(block.BlockIndex, BlockState::AckStatus::Failed);



					if (retry < config.RetryCount)

					{

						wchar_t msg[256];

						swprintf_s(msg, L"[BackupEngine] Block %llu failed, retrying (%d/%d)...",
							block.BlockIndex, retry + 1, config.RetryCount);

						LOG_WARNING(msg);

					}

				}

			}



			// 每 100 块保存一次状态 + 进度日志

			if (sentCount % 100 == 0 && sentCount > 0)

			{

				double pct = (double)sentCount / (double)pendingCount * 100.0;

				wchar_t progressMsg[256];

				swprintf_s(progressMsg, L"[BackupEngine] Transfer progress: %llu/%llu blocks (%.1f%%), %llu bytes compressed",
					sentCount, pendingCount, pct, totalCompressed);

				LOG_INFO(progressMsg);

				state.Save();

			}

		}



		if (hSource != INVALID_HANDLE_VALUE)

		{

			CloseHandle(hSource);

		}



		stats.CompressedBytes = totalCompressed;



		wchar_t msg[256];

		swprintf_s(msg, L"[BackupEngine] Transfer complete: %llu blocks sent, %llu acked, compressed %llu bytes",
			sentCount, stats.AckedBlocks, totalCompressed);

		LOG_INFO(msg);



		return sentCount;

	}



		// ============================================================

		// 流水线窗口管理

		// ============================================================

	void BackupEngine::ResetPipelineWindow()

	{

		std::lock_guard<std::mutex> lock(m_windowMutex);



		delete[] m_window;

		m_window = new PipelineSlot[m_windowSize];



		m_inFlightCount = 0;

		m_ackedCount = 0;

		m_failedCount = 0;

		m_pipelineError = false;

	}



	int BackupEngine::AcquireSlot(int timeoutMs)

	{

		std::unique_lock<std::mutex> lock(m_windowMutex);



		bool ok = m_windowCv.wait_for(lock,

			std::chrono::milliseconds(timeoutMs),

			[this] {

				if (m_pipelineError.load()) return true;

				for (int i = 0; i < m_windowSize; i++)

					if (!m_window[i].InUse) return true;

				return false;

			});



		if (!ok) return -1;  // 超时



		if (m_pipelineError.load()) return -1;  // 连接错误



		for (int i = 0; i < m_windowSize; i++)

		{

			if (!m_window[i].InUse)

			{

				m_window[i].InUse = true;

				return i;

			}

		}



		return -1;  // 不应到达

	}



	void BackupEngine::ReleaseSlot(uint64_t blockIndex)

	{

		std::lock_guard<std::mutex> lock(m_windowMutex);



		for (int i = 0; i < m_windowSize; i++)

		{

			if (m_window[i].InUse && m_window[i].BlockIndex == blockIndex)

			{

				m_window[i].InUse = false;

				m_window[i].BlockData.clear();

				m_window[i].BlockData.shrink_to_fit();

				m_window[i].Compressed.clear();

				m_window[i].Compressed.shrink_to_fit();

				break;

			}

		}

		// 不在这里 notify，由调用者负责

	}



	bool BackupEngine::WaitForAllAcks(int timeoutMs)

	{

		std::unique_lock<std::mutex> lock(m_windowMutex);

		return m_windowCv.wait_for(lock,

			std::chrono::milliseconds(timeoutMs),

			[this] { return m_inFlightCount.load() == 0 || m_pipelineError.load(); });

	}



		// ============================================================

		// 模拟 ACK（dryrun 模式）

		// ============================================================

	bool BackupEngine::SimulatedAck(uint32_t devNo, uint64_t blockIndex,

		const uint8_t hash[32], BlockState::BlockStateManager& state)

	{

		// 立即返回成功，模拟服务器行为

		state.SetBlockAck(blockIndex, BlockState::AckStatus::Acknowledged);



		return true;

	}



		// ============================================================

		// 关闭连接

		// ============================================================

	void BackupEngine::CloseConnection(Network::NetworkClient& client, uint32_t devNo)

	{

		if (client.IsConnected())

		{

			client.SendBye(devNo);

			client.Disconnect();

		}

	}



} // namespace BackupEngine
