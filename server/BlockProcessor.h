#pragma once

#include "DiskMap.h"
#include "Protocol.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// 单块处理：inflate → SHA-256 校验 → pwrite → 返回 ACK 状态
// 失败时不写盘
class BlockProcessor
{
public:
    explicit BlockProcessor(DiskMap& diskMap);

    // 处理一个 DATA_BLOCK
    // 输入: devno + block_header + 压缩数据
    // 输出: ACK 状态码；actualStatus 与 ack_body 已填充
    Protocol::AckStatus Process(uint32_t devno,
                                const Protocol::BlockHeader& bh,
                                const uint8_t* compressedData,
                                Protocol::AckBody& ackOut);

private:
    DiskMap& diskMap_;
    std::vector<uint8_t> rawBuf_; // 复用 1MB+ 缓冲

    static void ComputeSha256(const uint8_t* data, size_t len, uint8_t out[32]);
};
