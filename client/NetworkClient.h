#pragma once
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>


#pragma comment(lib, "ws2_32.lib")

namespace Network
{

// 协议常量
constexpr uint32_t PROTOCOL_MAGIC  = 0x474F3243;  // "C2OG" LE
constexpr uint32_t HEADER_SIZE     = 16;           // 通用消息头
constexpr uint32_t BLOCK_HEADER_SIZE = 52;          // DATA_BLOCK 块头
constexpr uint32_t ACK_BODY_SIZE   = 48;           // ACK 消息体
constexpr uint32_t BLOCK_SIZE      = 1024 * 1024;   // 1MB

// 消息类型
enum class MessageType : uint32_t
{
	DATA_BLOCK = 0x01,   // C→S: 数据块
	ACK        = 0x02,   // S→C: 确认应答
	HELLO      = 0x03,   // C→S: 握手
	BYE        = 0x04,   // C→S: 结束
};

// 通用消息头 (16 bytes)
#pragma pack(push, 1)
struct MsgHeader
{
	uint32_t Magic;       // PROTOCOL_MAGIC
	uint32_t Type;        // MessageType
	uint32_t DevNo;       // 磁盘编号
	uint32_t Reserved;    // 保留
};

// 数据块头 (52 bytes)
struct BlockHeader
{
	uint64_t BlockIndex;      // 块编号
	uint32_t DataSize;        // 压缩前原始大小
	uint32_t CompressedSize;  // 压缩后大小
	uint8_t  Hash[32];        // SHA-256 哈希
	uint32_t VersionId;       // 版本 ID
};

// ACK 消息体 (48 bytes)
struct AckBody
{
	uint64_t BlockIndex;   // 确认的块编号
	uint32_t Status;       // 0=OK, 非0=服务端错误
	uint32_t DevNo;        // Echo 磁盘编号
	uint8_t  Hash[32];     // Echo 哈希值
};

// Hello 消息体
struct HelloBody
{
	uint32_t VersionId;
	uint32_t DevNo;
	uint64_t TotalBlocks;
	uint64_t TotalSize;
	wchar_t  VersionType[32];   // UTF-16LE "FULL" / "INCREMENTAL"
};
#pragma pack(pop)

//
// ACK 回调: 服务器返回 ACK 时调用
// devNo: 磁盘编号
// blockIndex: 块编号
// hash: 服务端回传的哈希（32 字节）
// status: 0=成功
//
using AckCallback = std::function<void(uint32_t devNo, uint64_t blockIndex,
	const uint8_t hash[32], uint32_t status)>;

//
// NetworkClient: TCP 网络传输客户端
//
class NetworkClient
{

public:

	NetworkClient();
	~NetworkClient();

	//
	// 连接到服务器
	// serverIp: IPv4 地址字符串
	// port: 端口号
	// timeoutSec: 超时秒数 (默认 30)
	//
	bool Connect(const std::string& serverIp, uint16_t port, int timeoutSec = 30);

	//
	// 发送 Hello 握手消息
	// versionId, devNo, totalBlocks, totalSize, versionType
	// 等待服务端 ACK 确认
	//
	bool SendHello(uint32_t versionId, uint32_t devNo,
		uint64_t totalBlocks, uint64_t totalSize,
		const std::wstring& versionType);

	//
	// 发送数据块 + 等待 ACK
	// devNo: 磁盘编号
	// blockIndex: 块编号
	// rawData: 原始数据（压缩前）
	// rawSize: 原始大小
	// compressedData: 压缩后的数据
	// compressedSize: 压缩后大小
	// hash: SHA-256 哈希 (32 bytes)
	// versionId: 版本 ID
	//
	// 返回: true=ACK 成功收到, false=超时或失败
	//
	bool SendBlock(uint32_t devNo, uint64_t blockIndex,
		const uint8_t* rawData, uint32_t rawSize,
		const uint8_t* compressedData, uint32_t compressedSize,
		const uint8_t hash[32], uint32_t versionId);

	//
	// 发送 BYE 消息 + 等待 ACK
	//
	bool SendBye(uint32_t devNo);

	//
	// 断开连接
	//
	void Disconnect();

	//
	// 是否已连接
	//
	bool IsConnected() const { return m_connected; }

	//
	// 设置 / 获取超时秒数
	//
	void SetTimeout(int seconds) { m_timeoutSec = seconds; }
	int  GetTimeout() const { return m_timeoutSec; }

	//
	// 获取最后一次错误
	//
	std::wstring GetLastError() const { return m_lastError; }

private:

	//
	// 初始化 Winsock (程序启动时调用一次)
	//
	static bool InitializeWinsock();

	//
	// 发送原始数据
	//
	bool SendAll(const uint8_t* data, int size);

	//
	// 接收精确大小的数据
	//
	bool RecvAll(uint8_t* buffer, int size);

	//
	// 设置 socket 超时
	//
	bool SetSocketTimeout(SOCKET sock, int timeoutSec);

	//
	// 接收并解析 ACK
	//
	bool ReceiveAck(uint32_t devNo, uint64_t expectedBlockIndex);

	SOCKET m_socket;
	bool m_connected;
	int m_timeoutSec;

	std::string m_serverIp;
	uint16_t m_port;

	std::wstring m_lastError;
};

} // namespace Network
