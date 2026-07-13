#include "MessageParser.h"

#include <cstring>

using namespace Protocol;

ParseResult MessageParser::TryParse(const RingBuffer& buf,
                                    ParsedMessage& parsed,
                                    ParseDiag& diag)
{
    diag = ParseDiag{};

    size_t readable = buf.ReadableBytes();
    if (readable < HEADER_SIZE)
        return ParseResult::kNeedMore;

    const uint8_t* p = buf.ReadPtr();

    MsgHeader hdr;
    std::memcpy(&hdr, p, sizeof(hdr));

    if (hdr.Magic != PROTOCOL_MAGIC)
    {
        diag.gotMagic = hdr.Magic;
        return ParseResult::kBadMagic;
    }

    uint32_t bodyLen = 0;
    switch (static_cast<MessageType>(hdr.Type))
    {
        case MessageType::HELLO:
            bodyLen = HELLO_BODY_SIZE;
            break;
        case MessageType::DATA_BLOCK:
        {
            if (readable < HEADER_SIZE + BLOCK_HEADER_SIZE)
                return ParseResult::kNeedMore;
            BlockHeader bh;
            std::memcpy(&bh, p + HEADER_SIZE, sizeof(bh));
            if (bh.CompressedSize > BLOCK_SIZE * 2)
            {
                diag.gotType       = hdr.Type;
                diag.claimedCompSz = bh.CompressedSize;
                diag.claimedBlkIdx = bh.BlockIndex;
                return ParseResult::kOversizeBlock;
            }
            bodyLen = BLOCK_HEADER_SIZE + bh.CompressedSize;
            break;
        }
        case MessageType::BYE:
            bodyLen = 0;
            break;
        case MessageType::ACK:
            bodyLen = ACK_BODY_SIZE;
            break;
        case MessageType::ERROR:
            bodyLen = 0;
            break;
        default:
            diag.gotType = hdr.Type;
            return ParseResult::kInvalidType;
    }

    if (readable < HEADER_SIZE + bodyLen)
        return ParseResult::kNeedMore;

    parsed.header = hdr;
    parsed.body.assign(p + HEADER_SIZE, p + HEADER_SIZE + bodyLen);
    return ParseResult::kOk;
}
