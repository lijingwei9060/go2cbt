#pragma once

#include <cstddef>
#include <cstdint>
#include <sys/types.h>
#include <vector>

// 简单线性缓冲区（非循环），用 read_pos_/write_pos_ 维护有效数据
// 收到数据时追加到尾部，消费后定期 Compact 把未消费数据前移
// 单线程使用，无需加锁
class RingBuffer
{
public:
    explicit RingBuffer(size_t capacity);

    // 从 fd 调用 recv，返回接收到的字节数；0 表示对端关闭，-1 表示 EAGAIN
    ssize_t WriteFromFd(int fd);

    // 从内存追加
    void WriteFrom(const uint8_t* data, size_t len);

    const uint8_t* ReadPtr() const   { return buf_.data() + read_pos_; }
    size_t         ReadableBytes() const { return write_pos_ - read_pos_; }

    size_t WritableBytes() const     { return buf_.size() - write_pos_; }
    void   Consume(size_t bytes);
    void   Compact();

    void   Reset();

    // 把 data 写到 send 队列
    void   Append(const uint8_t* data, size_t len);

private:
    std::vector<uint8_t> buf_;
    size_t read_pos_  = 0;
    size_t write_pos_ = 0;
};
