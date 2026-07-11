#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <cstdint>
#include <string>
#include <vector>


namespace BlockHash
{

// 块大小：1MB（与 CBT_BLOCK_SIZE 对齐）
constexpr uint64_t BLOCK_SIZE = 1024 * 1024;
constexpr size_t   SHA256_HASH_SIZE = 32;   // SHA-256 输出 32 字节

//
// 单个块的哈希记录
//
struct BlockHashEntry
{
	uint64_t BlockIndex;          // 块序号（从 0 开始）
	uint64_t Offset;              // 块起始字节偏移
	uint8_t  Hash[SHA256_HASH_SIZE];  // SHA-256 哈希值（32 字节）
};

//
// 哈希清单（一次备份的所有块的哈希汇总）
// 用于：
// 1. 增量对比：与上次备份的清单对比，找出变化块
// 2. 完整性校验：服务器端可验证块完整性
// 3. 重复块检测：相同哈希 = 相同数据 = 跳过传输
//
struct HashManifest
{
	uint64_t BlockSize;           // 块大小（固定 1MB）
	uint64_t TotalBlocks;         // 总块数
	uint64_t TotalSize;           // 总数据大小（字节）
	std::vector<BlockHashEntry> Entries;  // 所有块的哈希
	uint64_t Timestamp;           // 备份时间戳（FILETIME）
	std::wstring VolumePath;      // 数据来源路径
};

//
// BlockHasher: 块哈希计算模块
//
// 功能：
// 1. SHA-256 单块哈希计算（BCrypt API）
// 2. 从数据源句柄批量构建哈希清单
// 3. 两份清单对比（增量变更检测）
// 4. 清单序列化 / 反序列化（二进制格式）
//
class BlockHasher
{

public:

	BlockHasher();
	~BlockHasher();

	//
	// 初始化 BCrypt SHA-256 算法 Provider
	// 复用单个 ALG_HANDLE 避免重复创建
	//
	bool Initialize();

	//
	// 计算单个数据块的 SHA-256 哈希
	// data: 数据缓冲区指针
	// size: 数据大小（通常为 BLOCK_SIZE，末尾块可能不足）
	// hashOut: [输出] 32 字节哈希值
	//
	bool ComputeBlockHash(const uint8_t* data, uint32_t size, uint8_t hashOut[SHA256_HASH_SIZE]);

	//
	// 从数据源句柄批量构建哈希清单
	// hSource: 打开的数据源句柄（快照卷或物理磁盘）
	// offset: 读取起始偏移（字节）
	// totalSize: 总数据大小（字节）
	// volumePath: 数据来源描述（用于填充 manifest）
	//
	// manifest: [输出] 完整哈希清单
	//
	bool BuildManifest(HANDLE hSource, uint64_t offset, uint64_t totalSize,
		const std::wstring& volumePath, HashManifest& manifest);

	//
	// 仅读取指定块的哈希（适用于增量备份时验证变化块）
	// hSource: 数据源句柄
	// blockIndexes: 需要验证的块索引列表
	// manifest: 对应的清单（用于查找 offset）
	//
	bool ComputeBlockHashes(HANDLE hSource, const HashManifest& manifest,
		const std::vector<uint64_t>& blockIndexes,
		std::vector<BlockHashEntry>& hashes);

	//
	// 比较两份哈希清单，返回变更的块索引列表
	// old: 上次备份的清单
	// current: 当前备份的清单
	// changedBlocks: [输出] 哈希不同的块索引
	//
	static void CompareManifests(const HashManifest& old, const HashManifest& current,
		std::vector<uint64_t>& changedBlocks);

	//
	// 序列化清单为二进制格式
	// 格式: [Header 32B][Entry1 40B][Entry2 40B]...
	//
	static std::vector<uint8_t> SerializeManifest(const HashManifest& manifest);

	//
	// 从二进制反序列化清单
	//
	static bool DeserializeManifest(const uint8_t* data, size_t size, HashManifest& manifest);

	//
	// 哈希值转十六进制字符串（用于日志/调试）
	//
	static std::wstring HashToHex(const uint8_t hash[SHA256_HASH_SIZE]);

	//
	// 获取最后一次错误消息
	//
	std::wstring GetLastError() const { return m_lastError; }

private:

	//
	// 按偏移读取指定范围的数据（内部使用 SetFilePointerEx + ReadFile）
	//
	bool ReadAt(HANDLE hFile, uint64_t offset, void* buffer, uint32_t size);

	bool m_initialized;
	BCRYPT_ALG_HANDLE m_hAlg;
	std::wstring m_lastError;

	// 数据缓冲区（读数据 + 计算哈希共用）
	std::vector<uint8_t> m_buffer;
};

} // namespace BlockHash
