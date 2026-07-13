#pragma once

// 服务端协议定义，必须与 client/NetworkClient.h 保持一致
// 所有结构体使用 #pragma pack(push, 1) 保证跨平台二进制一致

#include <cstdint>

namespace Protocol
{

// "C2OG" 小端序
constexpr uint32_t PROTOCOL_MAGIC    = 0x474F3243;
constexpr uint32_t HEADER_SIZE       = 16;
constexpr uint32_t BLOCK_HEADER_SIZE = 52;
constexpr uint32_t ACK_BODY_SIZE     = 48;
// HelloBody 固定 88 字节（与 Windows 客户端 wchar_t[32] = 64 字节一致）
constexpr uint32_t HELLO_BODY_SIZE   = 88;
constexpr uint32_t BLOCK_SIZE        = 1024 * 1024; // 1MB

// 消息类型
enum class MessageType : uint32_t
{
    DATA_BLOCK = 0x01,
    ACK        = 0x02,
    HELLO      = 0x03,
    BYE        = 0x04,
    ERROR      = 0x05,
};

// ACK 状态码
enum class AckStatus : uint32_t
{
    OK               = 0,
    HASH_MISMATCH    = 1,
    DECOMPRESS_ERROR = 2,
    WRITE_ERROR      = 3,
    INVALID_DEV      = 4,
    OUT_OF_RANGE     = 5,
};

#pragma pack(push, 1)

// 通用消息头 (16 bytes)
struct MsgHeader
{
    uint32_t Magic;
    uint32_t Type;
    uint32_t DevNo;
    uint32_t Reserved;
};

// 数据块头 (52 bytes)
struct BlockHeader
{
    uint64_t BlockIndex;
    uint32_t DataSize;
    uint32_t CompressedSize;
    uint8_t  Hash[32];
    uint32_t VersionId;
};

// ACK 消息体 (48 bytes)
struct AckBody
{
    uint64_t BlockIndex;
    uint32_t Status;
    uint32_t DevNo;
    uint8_t  Hash[32];
};

// Hello 消息体（固定 80 字节，wchar_t 在 Linux 是 4 字节，但协议约定 UTF-16LE）
// 为跨平台一致，这里用 uint16_t 数组代替 wchar_t[32]，对应 64 字节 UTF-16LE
struct HelloBody
{
    uint32_t VersionId;
    uint32_t DevNo;
    uint64_t TotalBlocks;
    uint64_t TotalSize;
    uint16_t VersionType[32]; // UTF-16LE，32 个码元 = 64 字节
};

#pragma pack(pop)

static_assert(sizeof(MsgHeader)   == 16, "MsgHeader must be 16 bytes");
static_assert(sizeof(BlockHeader) == 52, "BlockHeader must be 52 bytes");
static_assert(sizeof(AckBody)     == 48, "AckBody must be 48 bytes");
static_assert(sizeof(HelloBody)   == 88, "HelloBody must be 88 bytes");

} // namespace Protocol
