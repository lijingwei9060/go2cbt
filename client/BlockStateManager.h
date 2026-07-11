#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include "BlockHasher.h"


namespace BlockState
{

// 块大小（与 CBT_BLOCK_SIZE 对齐）
constexpr uint64_t BLOCK_SIZE = 1024 * 1024;
constexpr size_t   SHA256_SIZE = 32;

//
// ACK 状态：记录数据块在传输流程中的位置
//
enum class AckStatus : uint32_t
{
	Pending = 0,       // 待发送
	Sent = 1,          // 已发送给服务器，等待确认
	Acknowledged = 2,  // 服务器已确认收到
	Failed = 3,        // 发送失败（可重试）
	Skipped = 4        // 跳过（哈希未变 / 去重）
};

//
// 单个块的状态快照
//
struct BlockState
{
	uint64_t BlockIndex;              // 块编号（从 0 开始）
	uint64_t Offset;                  // 字节偏移 = BlockIndex × BLOCK_SIZE
	uint8_t  Hash[SHA256_SIZE];       // SHA-256 哈希值
	uint64_t VersionId;               // 所属版本 ID
	AckStatus Ack;                    // ACK 状态
	bool     Changed;                 // 本版本中是否变化

	// 简便判断
	bool NeedsUpload() const { return Changed && Ack != AckStatus::Acknowledged; }
};

//
// 版本记录：对应一次备份快照
//
struct VersionRecord
{
	uint64_t VersionId;               // 版本 ID（自增，从 0 开始）
	int      DevNo;                   // 磁盘编号
	std::wstring VersionType;         // "FULL" / "INCREMENTAL"
	FILETIME SnapshotTime;            // VSS 快照时间戳
	uint64_t TotalBlocks;             // 总块数
	uint64_t ChangedBlocks;           // 本版本变化块数
	uint64_t TotalSize;               // 数据总大小（字节）
	uint64_t AckedBlocks;             // 已确认块数
	uint64_t CreateTimestamp;         // 版本创建时间（本地时间）

	// 进度百分比
	double Progress() const {
		return TotalBlocks > 0 ? (double)AckedBlocks / (double)TotalBlocks * 100.0 : 0.0;
	}
};

//
// BlockStateManager: 块状态管理模块
//
// 职责：
// 1. 管理每个数据块的状态（哈希、版本、ACK）
// 2. 维护备份版本历史表
// 3. 支持断点续传（查询未确认块）
// 4. 持久化到本地文件（二进制格式）
//
// 文件格式（block_state_{devno}.dat）:
//   [Header 64B] [BlockRecord * TotalBlocks] [VersionTable]
//
// 每个 BlockRecord: 48 字节（固定大小，便于随机访问）
//   Flags:      4 bytes (bit0=Changed, bit1-3=AckStatus)
//   VersionId:  8 bytes
//   Hash:      32 bytes
//   Reserved:   4 bytes
//
class BlockStateManager
{

public:

	BlockStateManager();
	~BlockStateManager();

	//
	// 初始化：指定状态文件目录和磁盘编号
	// stateDir: 状态文件存储目录
	// devNo: 磁盘编号
	// 返回 true 如果成功加载或创建新状态文件
	//
	bool Initialize(const std::wstring& stateDir, int devNo);

	//
	// 创建新版本记录
	// type: "FULL" 或 "INCREMENTAL"
	// snapshotTime: VSS 快照时间
	// totalBlocks: 总块数
	// totalSize: 数据总大小
	// 返回新版本 ID（自增）
	//
	VersionRecord CreateVersion(const std::wstring& type, FILETIME snapshotTime,
		uint64_t totalBlocks, uint64_t totalSize);

	//
	// 批量初始化全量备份的块状态（所有块标记为 Changed=true, Ack=Pending）
	// versionId: 版本 ID
	// manifest: 哈希清单（来自 BlockHasher）
	//
	bool InitFullBlocks(uint64_t versionId, const BlockHash::HashManifest& manifest);

	//
	// 更新增量备份中变化块的哈希
	// versionId: 版本 ID
	// changedIndexes: 变化的块索引列表
	// hashes: 对应块的哈希值
	// 未在列表中的块保持 Changed=false, Ack=Skipped
	//
	bool UpdateIncrementalBlocks(uint64_t versionId,
		const std::vector<uint64_t>& changedIndexes,
		const std::vector<BlockHash::BlockHashEntry>& hashes);

	// ============================================================
	// ACK 管理
	// ============================================================

	//
	// 设置单个块的 ACK 状态
	//
	bool SetBlockAck(uint64_t blockIndex, AckStatus status);

	//
	// 批量设置连续块的 ACK 状态
	//
	bool SetBlockAckRange(uint64_t startBlock, uint64_t count, AckStatus status);

	//
	// 批量设置指定块的 ACK 状态（非连续列表）
	//
	bool SetBlockAckList(const std::vector<uint64_t>& blockIndexes, AckStatus status);

	// ============================================================
	// 查询
	// ============================================================

	//
	// 获取单个块的状态
	//
	BlockState GetBlockState(uint64_t blockIndex) const;

	//
	// 按 ACK 状态过滤块
	//
	std::vector<BlockState> GetBlocksByAck(AckStatus status) const;

	//
	// 获取所有待处理的块（Changed && Ack != Acknowledged）
	//
	std::vector<BlockState> GetPendingBlocks() const;

	//
	// 获取最后一个已确认块之前的连续确认数（断点续传起点）
	//
	uint64_t GetLastAcknowledgedBlock() const;

	//
	// 获取本磁盘的完整版本历史表
	//
	std::vector<VersionRecord> GetVersionHistory();

	//
	// 获取指定版本记录
	//
	VersionRecord GetVersion(uint64_t versionId) const;

	//
	// 获取指定块在所有已知版本中的历史（同一 BlockIndex 在不同版本中的状态）
	//
	std::vector<BlockState> GetBlockHistory(uint64_t blockIndex) const;

	//
	// 获取当前版本的块总数
	//
	uint64_t GetTotalBlocks() const { return m_totalBlocks; }

	//
	// 获取当前磁盘编号
	//
	int GetDevNo() const { return m_devNo; }

	//
	// 持久化到文件
	//
	bool Save();

	//
	// 是否已初始化
	//
	bool IsInitialized() const { return m_initialized; }

private:

	//
	// 加载状态文件和版本表
	//
	bool Load();

	//
	// 获取状态文件路径
	//
	std::wstring GetStateFilePath() const;

	//
	// 二进制序列化: BlockState → 48 字节
	//
	static void SerializeBlock(const BlockState& state, uint8_t* out);
	static void DeserializeBlock(const uint8_t* data, BlockState& state);

	bool m_initialized;
	bool m_dirty;                    // 有未保存的修改

	int m_devNo;
	uint64_t m_totalBlocks;

	std::wstring m_stateDir;
	mutable std::mutex m_mutex;

	// 块状态数组（索引 = BlockIndex）
	std::vector<BlockState> m_blocks;

	// 版本历史表（VersionId → VersionRecord）
	std::map<uint64_t, VersionRecord> m_versions;
	uint64_t m_nextVersionId;
};

} // namespace BlockState
