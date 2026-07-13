#pragma once

#include "BlockProcessor.h"
#include "DiskMap.h"
#include "RingBuffer.h"

#include <atomic>
#include <memory>
#include <unordered_map>

// 单线程 epoll 服务端
class Server
{
public:
    Server(DiskMap& diskMap, const std::string& listenAddr);
    ~Server();

    bool Start();
    void Stop();
    void Run(); // 阻塞运行直到 Stop

private:
    static constexpr size_t kRecvBufSize = 4 * 1024 * 1024; // 4MB 接收缓冲
    static constexpr size_t kSendBufSize = 256 * 1024;       // 256KB 发送缓冲
    static constexpr int    kEpollTimeoutMs = 1000;

    struct Conn
    {
        int       fd          = -1;
        uint32_t  devno       = 0;
        bool      helloDone   = false;
        RingBuffer recvBuf;
        RingBuffer sendBuf;
        uint64_t blocksReceived = 0;
        uint64_t bytesWritten   = 0;
        uint64_t errors         = 0;

        Conn() : recvBuf(kRecvBufSize), sendBuf(kSendBufSize) {}
    };

    DiskMap&         diskMap_;
    std::string      listenAddr_;
    int              listenFd_  = -1;
    int              epollFd_   = -1;
    std::atomic<bool> running_{false};

    std::unordered_map<int, std::unique_ptr<Conn>> conns_;
    BlockProcessor    processor_;

    bool ParseListenAddr(std::string& ip, uint16_t& port);
    bool SetupListenSocket();
    void SetNonBlocking(int fd);
    void EpollAdd(int fd, uint32_t events);
    void EpollMod(int fd, uint32_t events);

    void HandleAccept();
    void HandleConnReadable(Conn& c);
    void HandleConnWritable(Conn& c);
    void HandleConnError(Conn& c);

    void CloseConn(int fd);
    void ProcessMessages(Conn& c);

    void HandleHello(Conn& c, const Protocol::MsgHeader& hdr, const std::vector<uint8_t>& body);
    void HandleDataBlock(Conn& c, const Protocol::MsgHeader& hdr, const std::vector<uint8_t>& body);
    void HandleBye(Conn& c, const Protocol::MsgHeader& hdr);

    void EnqueueAck(Conn& c, const Protocol::AckBody& ack);
    void TryArmWrite(Conn& c);
};
