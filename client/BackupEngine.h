#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
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

	BackupConfig()
		: Port(0), DryRun(false), UseCbt(false),
		  RetryCount(3), TimeoutSec(30),
		  StateDir(L".\\backup_states\\")
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
	// 全量备份单个磁盘
	//
	bool FullBackup(const BackupConfig& config, int devNo,
		Disk::DiskLayout& layout, BackupStats& stats);

	//
	// 增量备份单个磁盘（使用 CBT）
	//
	bool IncrementalBackup(const BackupConfig& config, int devNo,
		Disk::DiskLayout& layout, BackupStats& stats);

	//
	// 传输块数据到服务器
	// hasher: 哈希模块
	// compressor: 压缩模块
	// client: 网络模块（dryrun 时可为模拟）
	// state: 块状态管理器
	// 返回: 成功传输的块数
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
};

} // namespace BackupEngine
