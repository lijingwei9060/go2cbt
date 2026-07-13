#include "BlockProcessor.h"
#include "Logger.h"

#include <openssl/sha.h>
#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <sys/uio.h>

using namespace Protocol;

BlockProcessor::BlockProcessor(DiskMap& diskMap)
    : diskMap_(diskMap)
{
    rawBuf_.resize(BLOCK_SIZE + 64); // 1MB + 余量
}

void BlockProcessor::ComputeSha256(const uint8_t* data, size_t len, uint8_t out[32])
{
    SHA256(data, len, out);
}

AckStatus BlockProcessor::Process(uint32_t devno,
                                  const BlockHeader& bh,
                                  const uint8_t* compressedData,
                                  AckBody& ackOut)
{
    std::memset(&ackOut, 0, sizeof(ackOut));
    ackOut.BlockIndex = bh.BlockIndex;
    ackOut.DevNo      = devno;
    std::memcpy(ackOut.Hash, bh.Hash, 32);

    const std::string hashHex = Logger::HashToHex(bh.Hash);

    LOG_DEBUG("[Block] begin devno=%u blockIndex=%llu version=%u dataSize=%u compressedSize=%u hash=%s",
              devno, (unsigned long long)bh.BlockIndex, bh.VersionId,
              bh.DataSize, bh.CompressedSize, hashHex.c_str());

    // 1. 查找 backing file
    const DiskMap::BackingFile* bf = diskMap_.Get(devno);
    if (!bf)
    {
        LOG_WARNING("[Block] INVALID_DEV devno=%u blockIndex=%llu hash=%s",
                    devno, (unsigned long long)bh.BlockIndex, hashHex.c_str());
        ackOut.Status = static_cast<uint32_t>(AckStatus::INVALID_DEV);
        return AckStatus::INVALID_DEV;
    }

    // 2. 校验偏移范围
    if (bh.DataSize == 0 || bh.DataSize > BLOCK_SIZE)
    {
        LOG_WARNING("[Block] OUT_OF_RANGE devno=%u blockIndex=%llu dataSize=%u (invalid)",
                    devno, (unsigned long long)bh.BlockIndex, bh.DataSize);
        ackOut.Status = static_cast<uint32_t>(AckStatus::OUT_OF_RANGE);
        return AckStatus::OUT_OF_RANGE;
    }

    uint64_t offset = bh.BlockIndex * static_cast<uint64_t>(BLOCK_SIZE);
    if (offset + bh.DataSize > bf->size_bytes)
    {
        LOG_WARNING("[Block] OUT_OF_RANGE devno=%u blockIndex=%llu offset=%llu dataSize=%u fileSize=%llu",
                    devno, (unsigned long long)bh.BlockIndex, (unsigned long long)offset,
                    bh.DataSize, (unsigned long long)bf->size_bytes);
        ackOut.Status = static_cast<uint32_t>(AckStatus::OUT_OF_RANGE);
        return AckStatus::OUT_OF_RANGE;
    }

    // 3. inflate 解压
    uLongf rawLen = static_cast<uLongf>(rawBuf_.size());
    uLong srcLen  = static_cast<uLong>(bh.CompressedSize);
    int zret = uncompress2(rawBuf_.data(), &rawLen, compressedData, &srcLen);
    if (zret != Z_OK || rawLen != bh.DataSize)
    {
        LOG_ERROR("[Block] DECOMPRESS_ERROR devno=%u blockIndex=%llu zret=%d rawLen=%lu expected=%u hash=%s",
                  devno, (unsigned long long)bh.BlockIndex, zret, rawLen, bh.DataSize, hashHex.c_str());
        ackOut.Status = static_cast<uint32_t>(AckStatus::DECOMPRESS_ERROR);
        return AckStatus::DECOMPRESS_ERROR;
    }

    LOG_DEBUG("[Block] inflate ok devno=%u blockIndex=%llu rawLen=%lu",
              devno, (unsigned long long)bh.BlockIndex, rawLen);

    // 4. SHA-256 校验
    uint8_t actualHash[32];
    ComputeSha256(rawBuf_.data(), rawLen, actualHash);
    if (std::memcmp(actualHash, bh.Hash, 32) != 0)
    {
        std::string actualHex = Logger::HashToHex(actualHash);
        LOG_WARNING("[Block] HASH_MISMATCH devno=%u blockIndex=%llu expected=%s actual=%s",
                    devno, (unsigned long long)bh.BlockIndex, hashHex.c_str(), actualHex.c_str());
        ackOut.Status = static_cast<uint32_t>(AckStatus::HASH_MISMATCH);
        return AckStatus::HASH_MISMATCH;
    }

    LOG_DEBUG("[Block] sha256 verified devno=%u blockIndex=%llu hash=%s",
              devno, (unsigned long long)bh.BlockIndex, hashHex.c_str());

    // 5. pwrite 写入
    ssize_t w = pwrite(bf->fd, rawBuf_.data(), rawLen, static_cast<off_t>(offset));
    if (w < 0 || static_cast<size_t>(w) != rawLen)
    {
        LOG_ERROR("[Block] WRITE_ERROR devno=%u blockIndex=%llu offset=%llu w=%zd errno=%d hash=%s",
                  devno, (unsigned long long)bh.BlockIndex, (unsigned long long)offset, w, errno, hashHex.c_str());
        ackOut.Status = static_cast<uint32_t>(AckStatus::WRITE_ERROR);
        return AckStatus::WRITE_ERROR;
    }

    // 6. 成功
    ackOut.Status = static_cast<uint32_t>(AckStatus::OK);

    LOG_INFO("[Block] ACK devno=%u blockIndex=%llu version=%u offset=%llu size=%lu ack=OK hash=%s",
             devno, (unsigned long long)bh.BlockIndex, bh.VersionId,
             (unsigned long long)offset, rawLen, hashHex.c_str());

    LOG_DEBUG("[Block] done devno=%u blockIndex=%llu ack=0 status=OK",
              devno, (unsigned long long)bh.BlockIndex);

    return AckStatus::OK;
}
