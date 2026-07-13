#pragma once

#include <cstdint>
#include <map>
#include <string>

// devno → backing file fd + 大小 的映射
class DiskMap
{
public:
    struct BackingFile
    {
        int      fd         = -1;
        uint64_t size_bytes = 0;
        std::string path;
    };

    DiskMap() = default;
    ~DiskMap();

    DiskMap(const DiskMap&)            = delete;
    DiskMap& operator=(const DiskMap&) = delete;

    // 打开 backing file (O_WRONLY)，fstat 记录大小
    // 成功返回 true，失败返回 false（错误已 LOG_ERROR）
    bool Add(uint32_t devno, const std::string& path);

    bool     Contains(uint32_t devno) const;
    int      GetFd(uint32_t devno) const;
    uint64_t GetSize(uint32_t devno) const;
    const BackingFile* Get(uint32_t devno) const;

    void CloseAll();

private:
    std::map<uint32_t, BackingFile> map_;
};
