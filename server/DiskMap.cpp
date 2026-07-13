#include "DiskMap.h"
#include "Logger.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

DiskMap::~DiskMap() { CloseAll(); }

bool DiskMap::Add(uint32_t devno, const std::string& path)
{
    int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0)
    {
        LOG_ERROR("[DiskMap] open failed devno=%u path=%s errno=%d", devno, path.c_str(), errno);
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) < 0)
    {
        LOG_ERROR("[DiskMap] fstat failed devno=%u path=%s errno=%d", devno, path.c_str(), errno);
        close(fd);
        return false;
    }

    if (!S_ISREG(st.st_mode))
    {
        LOG_ERROR("[DiskMap] not a regular file devno=%u path=%s", devno, path.c_str());
        close(fd);
        return false;
    }

    if (st.st_size <= 0)
    {
        LOG_ERROR("[DiskMap] file empty or size=0 devno=%u path=%s", devno, path.c_str());
        close(fd);
        return false;
    }

    BackingFile bf;
    bf.fd         = fd;
    bf.size_bytes = static_cast<uint64_t>(st.st_size);
    bf.path       = path;
    map_[devno]   = bf;

    LOG_INFO("[DiskMap] opened devno=%u path=%s size=%lu bytes", devno, path.c_str(), bf.size_bytes);
    return true;
}

bool DiskMap::Contains(uint32_t devno) const { return map_.count(devno) > 0; }

int DiskMap::GetFd(uint32_t devno) const
{
    auto it = map_.find(devno);
    return it == map_.end() ? -1 : it->second.fd;
}

uint64_t DiskMap::GetSize(uint32_t devno) const
{
    auto it = map_.find(devno);
    return it == map_.end() ? 0 : it->second.size_bytes;
}

const DiskMap::BackingFile* DiskMap::Get(uint32_t devno) const
{
    auto it = map_.find(devno);
    return it == map_.end() ? nullptr : &it->second;
}

void DiskMap::CloseAll()
{
    for (auto& kv : map_)
    {
        if (kv.second.fd >= 0)
        {
            fsync(kv.second.fd);
            close(kv.second.fd);
            kv.second.fd = -1;
        }
    }
    map_.clear();
}
