#pragma once
#include <windows.h>
#include <winioctl.h>
#include <cstdint>
#include <string>
#include <vector>


namespace CbtDriver
{

// CBT 驱动定义（与 go2cbt.h 完全一致）
constexpr uint64_t CBT_BLOCK_SIZE = 1024 * 1024;  // 1MB

// IOCTL 定义（与 go2cbt.h 保持一致）
#define IOCTL_CBT_QUERY  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_CBT_RESET  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

#pragma pack(push, 8)

// 输入: 指定磁盘编号
struct CBT_IOCTL_INPUT
{
	ULONG DeviceNumber;
};

// Query 输出: 变长结构，BitmapData 后跟实际位图数据
struct CBT_QUERY_OUTPUT
{
	ULONGLONG TotalBits;       // 位图总 bit 数 = 总块数
	ULONGLONG TotalBytes;      // 位图字节大小
	UCHAR     BitmapData[1];    // 变长位图数据
};

#pragma pack(pop)

//
// CbtClient: CBT 驱动客户端
//
// 职责:
// 1. 打开 \\.\CbtMonitor 设备
// 2. 启动 go2cbt 服务
// 3. Query: 两步协议读取 CBT 位图
// 4. Reset: 清零位图
//
class CbtClient
{

public:

	CbtClient();
	~CbtClient();

	//
	// 连接到 CBT 驱动
	// 自动尝试启动 go2cbt 服务（如果未运行）
	//
	bool Connect();

	//
	// 查询 CBT 位图
	// devNo: 磁盘编号 (0=Harddisk0)
	// bitmap: [输出] 位图字节数组 (每个 bit 代表一个 1MB 块)
	// totalBits: [输出] 总 bit 数 = 总块数
	// 返回 true 表示查询成功
	//
	bool Query(ULONG devNo, std::vector<uint8_t>& bitmap, ULONGLONG& totalBits);

	//
	// 解析位图，返回标记为 changed 的块索引列表
	// bitmap: Query 返回的位图数据
	// totalBits: 总 bit 数
	// changedBlocks: [输出] 变更块索引
	//
	static void ParseChangedBlocks(const std::vector<uint8_t>& bitmap, ULONGLONG totalBits,
		std::vector<uint64_t>& changedBlocks);

	//
	// 重置 CBT 位图（开始新的跟踪周期）
	//
	bool Reset(ULONG devNo);

	//
	// 断开连接
	//
	void Disconnect();

	//
	// 检查是否已连接
	//
	bool IsConnected() const { return m_connected; }

private:

	//
	// 启动 go2cbt 服务 (sc start go2cbt)
	//
	bool StartDriverService();

	HANDLE m_hDevice;
	bool m_connected;
};

} // namespace CbtDriver
