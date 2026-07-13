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

const char* Server::MsgTypeName(uint32_t type)
{
    switch (static_cast<MessageType>(type))
    {
        case MessageType::DATA_BLOCK: return "DATA_BLOCK";
        case MessageType::ACK:        return "ACK";
        case MessageType::HELLO:      return "HELLO";
        case MessageType::BYE:        return "BYE";
        case MessageType::ERROR:      return "ERROR";
        default:                      return "UNKNOWN";
    }
}

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

        // 读取内核分配的收发缓冲区大小，用于诊断
        int rcvbuf = 0, sndbuf = 0;
        socklen_t optlen = sizeof(int);
        getsockopt(cfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, &optlen);
        optlen = sizeof(int);
        getsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, &optlen);

        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        LOG_INFO("[Server] accept fd=%d peer=%s:%u tcp_nodelay=1 rcvbuf=%d sndbuf=%d",
                 cfd, ip, ntohs(peer.sin_port), rcvbuf, sndbuf);

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
            LOG_DEBUG("[Server] recv fd=%d bytes=%zd recvBufReadable=%zu",
                      fd, n, c.recvBuf.ReadableBytes());
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
            CloseConn(fd, "peer closed");
            return;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            LOG_DEBUG("[Server] recv EAGAIN fd=%d, drain complete", fd);
            ProcessMessages(c);
            return;
        }
        LOG_ERROR("[Server] recv error fd=%d errno=%d (%s)",
                  fd, errno, strerror(errno));
        CloseConn(fd, "recv error");
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
            LOG_DEBUG("[Server] send fd=%d bytes=%zd sendBufRemaining=%zu",
                      c.fd, n, c.sendBuf.ReadableBytes());
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            LOG_DEBUG("[Server] send EAGAIN fd=%d sendBufRemaining=%zu", c.fd, c.sendBuf.ReadableBytes());
            return;
        }
        LOG_ERROR("[Server] send error fd=%d errno=%d (%s)", c.fd, errno, strerror(errno));
        CloseConn(c.fd, "send error");
        return;
    }
    c.sendBuf.Compact();
    if (c.sendBuf.ReadableBytes() == 0)
    {
        LOG_DEBUG("[Server] send buf drained fd=%d, disarming EPOLLOUT", c.fd);
        EpollMod(c.fd, EPOLLIN);
    }
}

void Server::HandleConnError(Conn& c)
{
    int sockErr = 0;
    socklen_t optlen = sizeof(sockErr);
    getsockopt(c.fd, SOL_SOCKET, SO_ERROR, &sockErr, &optlen);
    LOG_WARNING("[Server] conn error/hangup fd=%d devno=%u sockErr=%d (%s)",
                c.fd, c.devno, sockErr, sockErr ? strerror(sockErr) : "no error");
    CloseConn(c.fd, "epoll error/hangup");
}

void Server::CloseConn(int fd, const char* reason)
{
    auto it = conns_.find(fd);
    if (it == conns_.end())
        return;
    Conn& c = *it->second;
    LOG_INFO("[Server] close conn fd=%d devno=%u reason=%s blocks=%lu bytes=%lu errors=%lu",
             fd, c.devno, reason ? reason : "",
             c.blocksReceived, c.bytesWritten, c.errors);
    epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    conns_.erase(it);
}

void Server::ProcessMessages(Conn& c)
{
    while (true)
    {
        ParsedMessage msg;
        ParseDiag diag;
        ParseResult r = MessageParser::TryParse(c.recvBuf, msg, diag);
        if (r == ParseResult::kNeedMore)
        {
            LOG_DEBUG("[Server] need more data fd=%d recvBufReadable=%zu",
                      c.fd, c.recvBuf.ReadableBytes());
            break;
        }
        if (r == ParseResult::kBadMagic)
        {
            LOG_WARNING("[Server] bad magic fd=%d got=0x%08x expected=0x%08x, skip 1 byte to resync",
                        c.fd, diag.gotMagic, PROTOCOL_MAGIC);
            c.recvBuf.Consume(1);
            c.recvBuf.Compact();
            continue;
        }
        if (r == ParseResult::kInvalidType)
        {
            LOG_WARNING("[Server] invalid msg type fd=%d type=%u magic=0x%08x, skip 1 byte to resync",
                        c.fd, diag.gotType, PROTOCOL_MAGIC);
            c.recvBuf.Consume(1);
            c.recvBuf.Compact();
            continue;
        }
        if (r == ParseResult::kOversizeBlock)
        {
            LOG_ERROR("[Server] oversize compressed block fd=%d devno=%u blockIndex=%llu claimedCompressedSize=%u limit=%u, close conn",
                      c.fd, msg.header.DevNo, (unsigned long long)diag.claimedBlkIdx,
                      diag.claimedCompSz, BLOCK_SIZE * 2);
            CloseConn(c.fd, "oversize compressed block");
            return;
        }

        LOG_DEBUG("[Server] parsed msg fd=%d type=%s(%u) devno=%u bodySize=%zu",
                  c.fd, MsgTypeName(msg.header.Type), msg.header.Type,
                  msg.header.DevNo, msg.body.size());

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
            case Protocol::MessageType::ACK:
                LOG_WARNING("[Server] unexpected ACK from client fd=%d devno=%u, ignore",
                            c.fd, msg.header.DevNo);
                break;
            case Protocol::MessageType::ERROR:
                LOG_WARNING("[Server] ERROR msg from client fd=%d devno=%u, ignore",
                            c.fd, msg.header.DevNo);
                break;
            default:
                LOG_WARNING("[Server] unhandled msg type=%u fd=%d", msg.header.Type, c.fd);
                break;
        }
        if (conns_.find(c.fd) == conns_.end())
            return; // 上层 handler 已关闭连接
        c.recvBuf.Consume(total);
        c.recvBuf.Compact();
    }
}

void Server::HandleHello(Conn& c, const MsgHeader& hdr, const std::vector<uint8_t>& body)
{
    if (c.helloDone)
    {
        LOG_WARNING("[Server] duplicate HELLO fd=%d devno=%u alreadyHandshaked, ignore",
                    c.fd, c.devno);
        return;
    }

    LOG_DEBUG("[Server] HELLO body size=%zu expected=%u fd=%d",
              body.size(), HELLO_BODY_SIZE, c.fd);

    if (body.size() < HELLO_BODY_SIZE)
    {
        LOG_ERROR("[Server] HELLO body too small fd=%d got=%zu expected=%u",
                  c.fd, body.size(), HELLO_BODY_SIZE);
        AckBody ack{};
        ack.BlockIndex = 0;
        ack.DevNo      = hdr.DevNo;
        ack.Status     = static_cast<uint32_t>(AckStatus::INVALID_DEV);
        EnqueueAck(c, ack);
        CloseConn(c.fd, "hello body too small");
        return;
    }

    HelloBody hb;
    std::memcpy(&hb, body.data(), sizeof(hb));

    std::string vtype = Logger::Utf16leToUtf8(hb.VersionType, 32);
    LOG_INFO("[Server] HELLO fd=%d devno=%u versionId=%u totalBlocks=%llu totalSize=%llu versionType=%s",
             c.fd, hb.DevNo, hb.VersionId, (unsigned long long)hb.TotalBlocks,
             (unsigned long long)hb.TotalSize, vtype.c_str());

    LOG_DEBUG("[Server] HELLO validating devno=%u in DiskMap fd=%d", hb.DevNo, c.fd);
    const DiskMap::BackingFile* bf = diskMap_.Get(hb.DevNo);
    AckBody ack{};
    ack.BlockIndex = 0;
    ack.DevNo      = hb.DevNo;
    if (!bf)
    {
        LOG_ERROR("[Server] HELLO INVALID_DEV devno=%u not registered", hb.DevNo);
        ack.Status = static_cast<uint32_t>(AckStatus::INVALID_DEV);
        EnqueueAck(c, ack);
        CloseConn(c.fd, "hello invalid dev");
        return;
    }

    LOG_DEBUG("[Server] HELLO validating backingFileSize=%llu >= totalSize=%llu devno=%u",
              (unsigned long long)bf->size_bytes, (unsigned long long)hb.TotalSize, hb.DevNo);
    if (bf->size_bytes < hb.TotalSize)
    {
        LOG_ERROR("[Server] HELLO OUT_OF_RANGE backingFile=%llu < totalSize=%llu devno=%u path=%s",
                  (unsigned long long)bf->size_bytes, (unsigned long long)hb.TotalSize,
                  hb.DevNo, bf->path.c_str());
        ack.Status = static_cast<uint32_t>(AckStatus::OUT_OF_RANGE);
        EnqueueAck(c, ack);
        CloseConn(c.fd, "hello out of range");
        return;
    }

    c.devno      = hb.DevNo;
    c.helloDone  = true;
    ack.Status   = static_cast<uint32_t>(AckStatus::OK);
    EnqueueAck(c, ack);
    LOG_INFO("[Server] HELLO ACK OK fd=%d devno=%u backingFile=%s",
             c.fd, c.devno, bf->path.c_str());
}

void Server::HandleDataBlock(Conn& c, const MsgHeader& hdr, const std::vector<uint8_t>& body)
{
    if (!c.helloDone)
    {
        LOG_WARNING("[Server] DATA_BLOCK before HELLO fd=%d devno=%u, drop conn",
                    c.fd, hdr.DevNo);
        CloseConn(c.fd, "data_block before hello");
        return;
    }
    if (body.size() < BLOCK_HEADER_SIZE)
    {
        LOG_WARNING("[Server] DATA_BLOCK body too small fd=%d devno=%u got=%zu expected>=%u",
                    c.fd, hdr.DevNo, body.size(), BLOCK_HEADER_SIZE);
        return;
    }

    BlockHeader bh;
    std::memcpy(&bh, body.data(), sizeof(bh));
    if (body.size() < BLOCK_HEADER_SIZE + bh.CompressedSize)
    {
        LOG_WARNING("[Server] DATA_BLOCK truncated fd=%d devno=%u bodySize=%zu expected=%u (blockHeader=%u + compressedSize=%u)",
                    c.fd, hdr.DevNo, body.size(),
                    BLOCK_HEADER_SIZE + bh.CompressedSize,
                    BLOCK_HEADER_SIZE, bh.CompressedSize);
        return;
    }

    LOG_DEBUG("[Server] DATA_BLOCK dispatch fd=%d devno=%u blockIndex=%llu dataSize=%u compressedSize=%u version=%u",
              c.fd, hdr.DevNo, (unsigned long long)bh.BlockIndex,
              bh.DataSize, bh.CompressedSize, bh.VersionId);

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
    CloseConn(c.fd, "bye");
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

    const char* stName = "UNKNOWN";
    switch (static_cast<AckStatus>(ack.Status))
    {
        case AckStatus::OK:               stName = "OK"; break;
        case AckStatus::HASH_MISMATCH:    stName = "HASH_MISMATCH"; break;
        case AckStatus::DECOMPRESS_ERROR: stName = "DECOMPRESS_ERROR"; break;
        case AckStatus::WRITE_ERROR:      stName = "WRITE_ERROR"; break;
        case AckStatus::INVALID_DEV:      stName = "INVALID_DEV"; break;
        case AckStatus::OUT_OF_RANGE:     stName = "OUT_OF_RANGE"; break;
    }

    LOG_DEBUG("[Server] enqueue ACK fd=%d devno=%u blockIndex=%llu status=%s(%u) sendBufQueued=%zu",
              c.fd, ack.DevNo, (unsigned long long)ack.BlockIndex,
              stName, ack.Status, c.sendBuf.ReadableBytes());

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
