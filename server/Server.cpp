#include "Server.h"
#include "Logger.h"
#include "MessageParser.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

using namespace Protocol;

Server::Server(DiskMap& diskMap, const std::string& listenAddr)
    : diskMap_(diskMap), listenAddr_(listenAddr), processor_(diskMap)
{
}

Server::~Server()
{
    Stop();
}

bool Server::Start()
{
    if (!SetupListenSocket())
        return false;

    epollFd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epollFd_ < 0)
    {
        LOG_ERROR("[Server] epoll_create1 failed errno=%d", errno);
        return false;
    }

    EpollAdd(listenFd_, EPOLLIN);
    running_ = true;
    LOG_INFO("[Server] listening on %s", listenAddr_.c_str());
    return true;
}

void Server::Stop()
{
    if (!running_.exchange(false))
        return;
    LOG_INFO("[Server] stopping...");

    // 打印每个连接的运行统计摘要后再关闭
    for (auto& kv : conns_)
    {
        Conn& c = *kv.second;
        LOG_INFO("[Server] shutdown summary fd=%d devno=%u blocks=%lu bytes=%lu errors=%lu",
                 c.fd, c.devno, c.blocksReceived, c.bytesWritten, c.errors);
        if (c.fd >= 0)
            close(c.fd);
    }
    conns_.clear();

    if (listenFd_ >= 0) { close(listenFd_); listenFd_ = -1; }
    if (epollFd_  >= 0) { close(epollFd_);  epollFd_  = -1; }
}

void Server::Run()
{
    epoll_event events[64];
    while (running_)
    {
        int n = epoll_wait(epollFd_, events, 64, kEpollTimeoutMs);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            LOG_ERROR("[Server] epoll_wait errno=%d", errno);
            break;
        }
        for (int i = 0; i < n; ++i)
        {
            int fd = events[i].data.fd;
            if (fd == listenFd_)
            {
                HandleAccept();
            }
            else
            {
                auto it = conns_.find(fd);
                if (it == conns_.end())
                    continue;
                Conn& c = *it->second;
                if (events[i].events & (EPOLLERR | EPOLLHUP))
                {
                    HandleConnError(c);
                    continue;
                }
                if (events[i].events & EPOLLIN)
                    HandleConnReadable(c);
                // 上面可能已 close 连接
                if (conns_.find(fd) == conns_.end())
                    continue;
                if (events[i].events & EPOLLOUT)
                    HandleConnWritable(c);
            }
        }
    }
    LOG_INFO("[Server] event loop exited");
}

bool Server::ParseListenAddr(std::string& ip, uint16_t& port)
{
    auto pos = listenAddr_.rfind(':');
    if (pos == std::string::npos)
        return false;
    ip = listenAddr_.substr(0, pos);
    port = static_cast<uint16_t>(std::stoi(listenAddr_.substr(pos + 1)));
    return true;
}

bool Server::SetupListenSocket()
{
    std::string ip;
    uint16_t port = 0;
    if (!ParseListenAddr(ip, port))
    {
        LOG_ERROR("[Server] invalid --listen format: %s", listenAddr_.c_str());
        return false;
    }

    listenFd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listenFd_ < 0)
    {
        LOG_ERROR("[Server] socket failed errno=%d", errno);
        return false;
    }

    int yes = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (ip.empty() || ip == "0.0.0.0")
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    else
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    if (bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        LOG_ERROR("[Server] bind %s failed errno=%d", listenAddr_.c_str(), errno);
        return false;
    }
    if (listen(listenFd_, 16) < 0)
    {
        LOG_ERROR("[Server] listen failed errno=%d", errno);
        return false;
    }
    return true;
}

void Server::SetNonBlocking(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

void Server::EpollAdd(int fd, uint32_t events)
{
    epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events  = events | EPOLLRDHUP;
    ev.data.fd = fd;
    epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev);
}

void Server::EpollMod(int fd, uint32_t events)
{
    epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events  = events | EPOLLRDHUP;
    ev.data.fd = fd;
    epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &ev);
}

void Server::HandleAccept()
{
    while (true)
    {
        sockaddr_in peer;
        socklen_t len = sizeof(peer);
        int cfd = accept4(listenFd_, reinterpret_cast<sockaddr*>(&peer), &len,
                          SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            LOG_ERROR("[Server] accept4 errno=%d", errno);
            break;
        }

        // 关闭 Nagle，因为我们自己做流水线 + ACK
        int one = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        LOG_INFO("[Server] accept fd=%d peer=%s:%u", cfd, ip, ntohs(peer.sin_port));

        auto conn = std::make_unique<Conn>();
        conn->fd = cfd;
        int fd = cfd;
        conns_[fd] = std::move(conn);
        EpollAdd(fd, EPOLLIN);
    }
}

void Server::HandleConnReadable(Conn& c)
{
    int fd = c.fd;
    while (true)
    {
        ssize_t n = c.recvBuf.WriteFromFd(fd);
        if (n > 0)
        {
            ProcessMessages(c);
            // ProcessMessages 可能在 BYE 后销毁连接
            if (conns_.find(fd) == conns_.end())
                return;
            continue;
        }
        if (n == 0)
        {
            // 对端关闭
            LOG_INFO("[Server] peer closed fd=%d devno=%u blocks=%lu bytes=%lu errors=%lu",
                     fd, c.devno, c.blocksReceived, c.bytesWritten, c.errors);
            CloseConn(fd);
            return;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            ProcessMessages(c);
            return;
        }
        LOG_ERROR("[Server] recv error fd=%d errno=%d", fd, errno);
        CloseConn(fd);
        return;
    }
}

void Server::HandleConnWritable(Conn& c)
{
    while (c.sendBuf.ReadableBytes() > 0)
    {
        ssize_t n = send(c.fd, c.sendBuf.ReadPtr(), c.sendBuf.ReadableBytes(), MSG_NOSIGNAL);
        if (n > 0)
        {
            c.sendBuf.Consume(static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        LOG_ERROR("[Server] send error fd=%d errno=%d", c.fd, errno);
        CloseConn(c.fd);
        return;
    }
    c.sendBuf.Compact();
    if (c.sendBuf.ReadableBytes() == 0)
        EpollMod(c.fd, EPOLLIN);
}

void Server::HandleConnError(Conn& c)
{
    LOG_WARNING("[Server] conn error/hangup fd=%d devno=%u", c.fd, c.devno);
    CloseConn(c.fd);
}

void Server::CloseConn(int fd)
{
    auto it = conns_.find(fd);
    if (it == conns_.end())
        return;
    epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    conns_.erase(it);
}

void Server::ProcessMessages(Conn& c)
{
    while (true)
    {
        ParsedMessage msg;
        ParseResult r = MessageParser::TryParse(c.recvBuf, msg);
        if (r == ParseResult::kNeedMore)
            break;
        if (r == ParseResult::kBadMagic)
        {
            LOG_WARNING("[Server] bad magic fd=%d, skip 1 byte to resync", c.fd);
            c.recvBuf.Consume(1);
            c.recvBuf.Compact();
            continue;
        }

        size_t total = Protocol::HEADER_SIZE + msg.body.size();
        switch (static_cast<Protocol::MessageType>(msg.header.Type))
        {
            case Protocol::MessageType::HELLO:
                HandleHello(c, msg.header, msg.body);
                break;
            case Protocol::MessageType::DATA_BLOCK:
                HandleDataBlock(c, msg.header, msg.body);
                break;
            case Protocol::MessageType::BYE:
                HandleBye(c, msg.header);
                break;
            default:
                LOG_WARNING("[Server] unknown msg type=%u fd=%d", msg.header.Type, c.fd);
                break;
        }
        c.recvBuf.Consume(total);
        c.recvBuf.Compact();
    }
}

void Server::HandleHello(Conn& c, const MsgHeader& hdr, const std::vector<uint8_t>& body)
{
    if (body.size() < HELLO_BODY_SIZE)
    {
        LOG_ERROR("[Server] HELLO body too small fd=%d size=%zu", c.fd, body.size());
        AckBody ack{};
        ack.BlockIndex = 0;
        ack.DevNo      = hdr.DevNo;
        ack.Status     = static_cast<uint32_t>(AckStatus::INVALID_DEV);
        EnqueueAck(c, ack);
        CloseConn(c.fd);
        return;
    }

    HelloBody hb;
    std::memcpy(&hb, body.data(), sizeof(hb));

    std::string vtype = Logger::Utf16leToUtf8(hb.VersionType, 32);
    LOG_INFO("[Server] HELLO fd=%d devno=%u versionId=%u totalBlocks=%llu totalSize=%llu versionType=%s",
             c.fd, hb.DevNo, hb.VersionId, (unsigned long long)hb.TotalBlocks,
             (unsigned long long)hb.TotalSize, vtype.c_str());

    const DiskMap::BackingFile* bf = diskMap_.Get(hb.DevNo);
    AckBody ack{};
    ack.BlockIndex = 0;
    ack.DevNo      = hb.DevNo;
    if (!bf)
    {
        LOG_ERROR("[Server] HELLO INVALID_DEV devno=%u not registered", hb.DevNo);
        ack.Status = static_cast<uint32_t>(AckStatus::INVALID_DEV);
        EnqueueAck(c, ack);
        CloseConn(c.fd);
        return;
    }
    if (bf->size_bytes < hb.TotalSize)
    {
        LOG_ERROR("[Server] HELLO OUT_OF_RANGE backingFile=%llu < totalSize=%llu devno=%u",
                  (unsigned long long)bf->size_bytes, (unsigned long long)hb.TotalSize, hb.DevNo);
        ack.Status = static_cast<uint32_t>(AckStatus::OUT_OF_RANGE);
        EnqueueAck(c, ack);
        CloseConn(c.fd);
        return;
    }

    c.devno      = hb.DevNo;
    c.helloDone  = true;
    ack.Status   = static_cast<uint32_t>(AckStatus::OK);
    EnqueueAck(c, ack);
    LOG_INFO("[Server] HELLO ACK OK fd=%d devno=%u", c.fd, c.devno);
}

void Server::HandleDataBlock(Conn& c, const MsgHeader& hdr, const std::vector<uint8_t>& body)
{
    if (!c.helloDone)
    {
        LOG_WARNING("[Server] DATA_BLOCK before HELLO fd=%d, drop", c.fd);
        CloseConn(c.fd);
        return;
    }
    if (body.size() < BLOCK_HEADER_SIZE)
    {
        LOG_WARNING("[Server] DATA_BLOCK body too small fd=%d size=%zu", c.fd, body.size());
        return;
    }

    BlockHeader bh;
    std::memcpy(&bh, body.data(), sizeof(bh));
    if (body.size() < BLOCK_HEADER_SIZE + bh.CompressedSize)
    {
        LOG_WARNING("[Server] DATA_BLOCK truncated fd=%d size=%zu expected=%u",
                    c.fd, body.size(), BLOCK_HEADER_SIZE + bh.CompressedSize);
        return;
    }

    const uint8_t* compressed = body.data() + BLOCK_HEADER_SIZE;

    AckBody ack;
    AckStatus st = processor_.Process(hdr.DevNo, bh, compressed, ack);
    c.blocksReceived++;
    if (st == AckStatus::OK)
        c.bytesWritten += bh.DataSize;
    else
        c.errors++;

    // 每 100 块输出一次进度
    if (c.blocksReceived % 100 == 0)
    {
        LOG_INFO("[Server] progress fd=%d devno=%u blocks=%lu bytes=%lu errors=%lu",
                 c.fd, c.devno, c.blocksReceived, c.bytesWritten, c.errors);
    }

    EnqueueAck(c, ack);
}

void Server::HandleBye(Conn& c, const MsgHeader& hdr)
{
    LOG_INFO("[Server] BYE fd=%d devno=%u blocks=%lu bytes=%lu errors=%lu",
             c.fd, c.devno, c.blocksReceived, c.bytesWritten, c.errors);

    AckBody ack{};
    ack.BlockIndex = 0;
    ack.DevNo      = hdr.DevNo;
    ack.Status     = static_cast<uint32_t>(AckStatus::OK);
    EnqueueAck(c, ack);
    CloseConn(c.fd);
}

void Server::EnqueueAck(Conn& c, const AckBody& ack)
{
    MsgHeader hdr;
    hdr.Magic    = PROTOCOL_MAGIC;
    hdr.Type     = static_cast<uint32_t>(MessageType::ACK);
    hdr.DevNo    = ack.DevNo;
    hdr.Reserved = 0;

    c.sendBuf.Append(reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr));
    c.sendBuf.Append(reinterpret_cast<const uint8_t*>(&ack), sizeof(ack));
    TryArmWrite(c);
}

void Server::TryArmWrite(Conn& c)
{
    if (c.sendBuf.ReadableBytes() > 0)
    {
        EpollMod(c.fd, EPOLLIN | EPOLLOUT);
        // 立即尝试一次非阻塞 send，避免等下次 epoll 唤醒
        HandleConnWritable(c);
    }
}
