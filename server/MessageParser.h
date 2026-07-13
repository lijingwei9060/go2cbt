#pragma once

#include "Protocol.h"
#include "RingBuffer.h"

#include <cstdint>
#include <optional>
#include <vector>

// 从 RingBuffer 字节流中切出完整消息
// 返回值:
//   kNeedMore      — 数据不完整，等更多数据
//   kOk            — 完整消息已解析，parsed 已填充
//   kBadMagic      — 当前 16B 头部 Magic 不匹配
//   kInvalidType   — Magic OK 但 Type 不在已知枚举内
//   kOversizeBlock — DATA_BLOCK 的 CompressedSize 异常大（> 2*BLOCK_SIZE）
enum class ParseResult
{
    kNeedMore,
    kOk,
    kBadMagic,
    kInvalidType,
    kOversizeBlock,
};

struct ParsedMessage
{
    Protocol::MsgHeader   header;
    std::vector<uint8_t>  body;
};

// 解析失败时的诊断信息（仅在结果 != kOk/kNeedMore 时有意义）
struct ParseDiag
{
    uint32_t gotMagic      = 0;  // 实际收到的 Magic（kBadMagic 时填充）
    uint32_t gotType       = 0;  // 实际收到的 Type（kInvalidType 时填充）
    uint32_t claimedCompSz = 0;  // 声明的 CompressedSize（kOversizeBlock 时填充）
    uint64_t claimedBlkIdx = 0;  // 声明的 BlockIndex（kOversizeBlock 时填充）
};

class MessageParser
{
public:
    static ParseResult TryParse(const RingBuffer& buf,
                                ParsedMessage& parsed,
                                ParseDiag& diag);
};
