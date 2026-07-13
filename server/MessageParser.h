#pragma once

#include "Protocol.h"
#include "RingBuffer.h"

#include <cstdint>
#include <optional>
#include <vector>

// 从 RingBuffer 字节流中切出完整消息
// 返回值:
//   kNeedMore  — 数据不完整，等更多数据
//   kOk        — 完整消息已解析，parsed 已填充，调用方负责 Consume
//   kBadMagic  — 当前 16B 头部 Magic 不匹配，上层应丢弃 1 字节重新扫描
enum class ParseResult
{
    kNeedMore,
    kOk,
    kBadMagic,
};

struct ParsedMessage
{
    Protocol::MsgHeader   header;
    std::vector<uint8_t>  body;   // 完整 body 字节
};

class MessageParser
{
public:
    // 尝试从 buf 当前可读数据中解析一条消息
    static ParseResult TryParse(const RingBuffer& buf, ParsedMessage& parsed);
};
