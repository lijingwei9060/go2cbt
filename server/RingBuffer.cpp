#include "RingBuffer.h"

#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>

RingBuffer::RingBuffer(size_t capacity)
    : buf_(capacity)
{
}

ssize_t RingBuffer::WriteFromFd(int fd)
{
    if (write_pos_ == buf_.size())
        Compact();
    if (write_pos_ == buf_.size())
    {
        // 缓冲区已满，无法再接收
        errno = ENOBUFS;
        return -1;
    }

    ssize_t n = recv(fd, buf_.data() + write_pos_, buf_.size() - write_pos_, 0);
    if (n > 0)
        write_pos_ += static_cast<size_t>(n);
    return n;
}

void RingBuffer::WriteFrom(const uint8_t* data, size_t len)
{
    if (write_pos_ + len > buf_.size())
        buf_.resize(write_pos_ + len);
    std::copy(data, data + len, buf_.begin() + write_pos_);
    write_pos_ += len;
}

void RingBuffer::Consume(size_t bytes)
{
    if (bytes > ReadableBytes())
        bytes = ReadableBytes();
    read_pos_ += bytes;
}

void RingBuffer::Compact()
{
    if (read_pos_ == 0)
        return;
    size_t keep = ReadableBytes();
    if (keep > 0)
        std::move(buf_.begin() + read_pos_, buf_.begin() + write_pos_, buf_.begin());
    read_pos_  = 0;
    write_pos_ = keep;
}

void RingBuffer::Reset()
{
    read_pos_  = 0;
    write_pos_ = 0;
}

void RingBuffer::Append(const uint8_t* data, size_t len)
{
    WriteFrom(data, len);
}
