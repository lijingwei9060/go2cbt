#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include "DiskParser.h"

// 前向声明
namespace BlockHash    { struct HashManifest; class BlockHasher; }
namespace BlockState   { class BlockStateManager; }
namespace CbtDriver    { class CbtClient; }
namespace DataCompress { class DataCompressor; }
namespace Network      { class NetworkClient; }
namespace VssSnapshot  { class VssManager; }


namespace BackupEngine
{

// 流水线窗口最大槽位数（环形窗口并发度）
static constexpr int DEFAULT_PIPELINE_WINDOW_SIZE = 6;

// 备份配置
struct BackupConfig
{
	std::string ServerIp;         // 服务器 IP
	uint16_t    Port;             // 端口
	bool        DryRun;           // 模拟模式
	bool        UseCbt;           // 使用 CBT 增量
	int         RetryCount;       // 重试次数（默认 3）
	int         TimeoutSec;       // 超时秒（默认 30）
	std::wstring StateDir;        // 状态文件目录
	int         PipelineWindowSize; // 流水线窗口大小（默认 6）

	BackupConfig()
		: Port(0), DryRun(false), UseCbt(false),
		  RetryCount(3), TimeoutSec(30),
		  StateDir(L".\\backup_states\\"),
		  PipelineWindowSize(DEFAULT_PIPELINE_WINDOW_SIZE)
	{}
};

// 备份统计
struct BackupStats
{
	int      DevNo;
	uint64_t TotalBlocks;
	uint64_t ChangedBlocks;
	uint64_t SentBlocks;
	uint64_t AckedBlocks;
	uint64_t FailedBlocks;
	uint64_t SkippedBlocks;
	uint64_t TotalBytes;
	uint64_t CompressedBytes;
	bool     VssUsed;

	BackupStats() { memset(this, 0, sizeof(*this)); }
};

//
// 流水线窗口槽位：表示一个正在传输中的块
//
struct PipelineSlot
{
	bool         InUse;              // 是否正在使用
	uint64_t     BlockIndex;         // 块编号
	std::vector<uint8_t> BlockData;  // 原始块数据（1MB）
	std::vector<uint8_t> Compressed; // 压缩后数据
	uint32_t     RawSize;            // 原始大小
	uint8_t      Hash[32];           // SHA-256 哈希
	bool         SendFailed;         // 发送是否失败

	PipelineSlot()
		: InUse(false), BlockIndex(0), RawSize(0), SendFailed(false)
	{
		memset(Hash, 0, sizeof(Hash));
	}
};

//
// BackupEngine: 备份引擎编排层
//
class BackupEngine
{

public:

	BackupEngine();
	~BackupEngine();

	//
	// 执行备份
	// config: 备份配置
	// devNos: 目标磁盘编号列表
	// stats: [输出] 每个磁盘的备份统计
	// 返回: true=全部成功
	//
	bool Run(const BackupConfig& config,
		const std::vector<int>& devNos,
		std::vector<BackupStats>& stats);

private:

	//
	// 备份单个磁盘
	//
	bool BackupDisk(const BackupConfig& config, int devNo, BackupStats& stats);

	//
	// 全量备份单个磁盘（流水线模式：边算 hash 边传输）
	//
	bool FullBackup(const BackupConfig& config, int devNo,
		Disk::DiskLayout& layout, BackupStats& stats);

	//
	// 增量备份单个磁盘（使用 CBT）
	//
	bool IncrementalBackup(const BackupConfig& config, int devNo,
		Disk::DiskLayout& layout, BackupStats& stats);

	//
	// 流水线传输数据块（全量备份专用）
	// 逐块处理：读 → hash → 压缩 → 非阻塞发送
	// 环形窗口：不等 ACK 就发下一块，ACK 回来后释放窗口槽位
	//
	uint64_t PipelineTransferBlocks(const BackupConfig& config, int devNo,
		BlockHash::BlockHasher& hasher,
		DataCompress::DataCompressor& compressor,
		Network::NetworkClient& client,
		BlockState::BlockStateManager& state,
		uint32_t versionId, uint64_t totalBlocks, uint64_t totalSize,
		const std::wstring& dataSourcePath,
		BackupStats& stats);

	//
	// 同步传输块数据（增量备份使用，保留原有逻辑）
	//
	uint64_t TransferBlocks(const BackupConfig& config, int devNo,
		BlockHash::BlockHasher& hasher,
		DataCompress::DataCompressor& compressor,
		Network::NetworkClient& client,
		BlockState::BlockStateManager& state,
		uint32_t versionId, uint64_t totalBlocks,
		const std::wstring& dataSourcePath,
		BackupStats& stats);

	//
	// 模拟 ACK（dryrun 模式）
	//
	bool SimulatedAck(uint32_t devNo, uint64_t blockIndex,
		const uint8_t hash[32], BlockState::BlockStateManager& state);

	//
	// 发送 BYE + 断开连接
	//
	void CloseConnection(Network::NetworkClient& client, uint32_t devNo);

	// ---- 流水线窗口管理 ----

	//
	// 获取空闲槽位（窗口满时阻塞等待，返回槽位索引，-1=超时/异常）
	//
	int AcquireSlot(int timeoutMs = 60000);

	//
	// 释放指定块的槽位（ACK 回调中调用）
	//
	void ReleaseSlot(uint64_t blockIndex);

	//
	// 等待所有在途块的 ACK（窗口清空）
	// 返回: true=所有 ACK 收到, false=超时
	//
	bool WaitForAllAcks(int timeoutMs = 300000);

	//
	// 重置流水线窗口状态
	//
	void ResetPipelineWindow();

	// ---- 流水线窗口数据 ----

	int                           m_windowSize;         // 实际窗口大小
	PipelineSlot*                 m_window;             // 窗口槽位数组
	std::mutex                    m_windowMutex;        // 窗口互斥锁
	std::condition_variable       m_windowCv;           // 窗口条件变量
	std::atomic<int>              m_inFlightCount;      // 在途块数（已发未 ACK）
	std::atomic<uint64_t>         m_ackedCount;         // 已 ACK 块数
	std::atomic<uint64_t>         m_failedCount;        // 失败块数
	std::atomic<bool>             m_pipelineError;      // 流水线错误标志
};

} // namespace BackupEngine
