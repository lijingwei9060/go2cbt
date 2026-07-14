#include "NetworkClient.h"
#include "Logger.h"
#include <string.h>


namespace Network
{

	NetworkClient::NetworkClient()
		: m_socket(INVALID_SOCKET)
		, m_connected(false)
		, m_timeoutSec(30)
		, m_port(0)
		, m_ackReceiverRunning(false)
	{
		InitializeWinsock();
	}

	NetworkClient::~NetworkClient()
	{
		StopAckReceiver();
		Disconnect();
	}

	// ============================================================
	// 初始化 Winsock
	// ============================================================
	bool NetworkClient::InitializeWinsock()
	{
		static bool winsockReady = false;
		if (winsockReady) return true;

		WSADATA wsaData;
		int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
		if (result != 0)
		{
			LOG_ERROR(L"[NetworkClient] WSAStartup failed");
			return false;
		}

		winsockReady = true;
		return true;
	}

	// ============================================================
	// 连接服务器
	// ============================================================
	bool NetworkClient::Connect(const std::string& serverIp, uint16_t port, int timeoutSec)
	{
		m_serverIp = serverIp;
		m_port = port;
		m_timeoutSec = timeoutSec;

		LOG_INFO(L"[NetworkClient] Connecting to server...");

		m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (m_socket == INVALID_SOCKET)
		{
			m_lastError = L"Failed to create socket";
			LOG_ERROR(m_lastError.c_str());
			return false;
		}

		// 设置超时
			// 启用 TCP Keep-Alive，防止长时间空闲（如 hash 计算）导致连接被防火墙/NAT 断开
			SetTcpKeepAlive(m_socket);
		SetSocketTimeout(m_socket, m_timeoutSec);
		SetSocketTimeout(m_socket, m_timeoutSec);

		sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);

		if (inet_pton(AF_INET, serverIp.c_str(), &addr.sin_addr) != 1)
		{
			m_lastError = L"Invalid server IP address";
			LOG_ERROR(m_lastError.c_str());
			closesocket(m_socket);
			m_socket = INVALID_SOCKET;
			return false;
		}

		if (::connect(m_socket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
		{
			int err = WSAGetLastError();
			wchar_t msg[256];
			swprintf_s(msg, L"[NetworkClient] connect to %hs:%hu failed, error=%d",
				serverIp.c_str(), port, err);
			LOG_ERROR(msg);
			m_lastError = msg;

			closesocket(m_socket);
			m_socket = INVALID_SOCKET;
			return false;
		}

		m_connected = true;

		wchar_t msg[256];
		swprintf_s(msg, L"[NetworkClient] Connected to %hs:%hu", serverIp.c_str(), port);
		LOG_INFO(msg);

		return true;
	}

	// ============================================================
	// 发送 Hello
	// ============================================================
	bool NetworkClient::SendHello(uint32_t versionId, uint32_t devNo,
		uint64_t totalBlocks, uint64_t totalSize,
		const std::wstring& versionType)
	{
		if (!m_connected.load())
		{
			LOG_ERROR(L"[NetworkClient] SendHello called but not connected");
			return false;
		}

		LOG_INFO(L"[NetworkClient] Sending HELLO...");

		// ---- Header ----
		MsgHeader hdr;
		memset(&hdr, 0, sizeof(hdr));
		hdr.Magic = PROTOCOL_MAGIC;
		hdr.Type = (uint32_t)MessageType::HELLO;
		hdr.DevNo = devNo;

		// ---- Body ----
		HelloBody body;
		memset(&body, 0, sizeof(body));
		body.VersionId = versionId;
		body.DevNo = devNo;
		body.TotalBlocks = totalBlocks;
		body.TotalSize = totalSize;
		wcsncpy_s(body.VersionType, versionType.c_str(), 31);

		// 发送
		if (!SendAll((uint8_t*)&hdr, sizeof(hdr))) return false;
		if (!SendAll((uint8_t*)&body, sizeof(body))) return false;

		// 等待 ACK 确认
		bool ackOk = ReceiveAck(devNo, 0);
		if (ackOk)
		{
			wchar_t msg[256];
			swprintf_s(msg, L"[NetworkClient] HELLO ACK OK: dev=%u version=%u blocks=%llu size=%llu type=%s",
				devNo, versionId, totalBlocks, totalSize, versionType.c_str());
			LOG_INFO(msg);
		}
		else
		{
			LOG_ERROR(L"[NetworkClient] HELLO ACK FAILED - server rejected handshake");
		}
		return ackOk;
	}

	// ============================================================
	// 发送数据块 + 等待 ACK（同步模式，增量备份使用）
	// ============================================================
	bool NetworkClient::SendBlock(uint32_t devNo, uint64_t blockIndex,
		const uint8_t* rawData, uint32_t rawSize,
		const uint8_t* compressedData, uint32_t compressedSize,
		const uint8_t hash[32], uint32_t versionId)
	{
		if (!m_connected.load()) return false;

		// ---- Header ----
		MsgHeader hdr;
		memset(&hdr, 0, sizeof(hdr));
		hdr.Magic = PROTOCOL_MAGIC;
		hdr.Type = (uint32_t)MessageType::DATA_BLOCK;
		hdr.DevNo = devNo;

		// ---- Block Header ----
		BlockHeader blkHdr;
		memset(&blkHdr, 0, sizeof(blkHdr));
		blkHdr.BlockIndex = blockIndex;
		blkHdr.DataSize = rawSize;
		blkHdr.CompressedSize = compressedSize;
		memcpy(blkHdr.Hash, hash, 32);
		blkHdr.VersionId = versionId;

		// 发送
		if (!SendAll((uint8_t*)&hdr, sizeof(hdr))) return false;
		if (!SendAll((uint8_t*)&blkHdr, sizeof(blkHdr))) return false;
		if (!SendAll(compressedData, compressedSize)) return false;

		// 发送日志（DEBUG 级别）
		{
			wchar_t hexStr[65];
			for (int hi = 0; hi < 32; hi++)
				swprintf_s(hexStr + hi * 2, 3, L"%02X", hash[hi]);
			hexStr[64] = L'\0';

			wchar_t dbg[384];
			swprintf_s(dbg, L"[NetworkClient] SEND block=%llu dev=%u raw=%u compressed=%u hash=%s",
				blockIndex, devNo, rawSize, compressedSize, hexStr);
			LOG_DEBUG(dbg);
		}

		// 等待 ACK
		return ReceiveAck(devNo, blockIndex);
	}

	// ============================================================
	// 非阻塞发送数据块（流水线模式：不等待 ACK）
	// ============================================================
	bool NetworkClient::SendBlockNoWait(uint32_t devNo, uint64_t blockIndex,
		const uint8_t* rawData, uint32_t rawSize,
		const uint8_t* compressedData, uint32_t compressedSize,
		const uint8_t hash[32], uint32_t versionId)
	{
		if (!m_connected.load()) return false;

		// 加锁防止并发 SendAll（主线程独占发送）
		std::lock_guard<std::mutex> lock(m_sendMutex);

		// ---- Header ----
		MsgHeader hdr;
		memset(&hdr, 0, sizeof(hdr));
		hdr.Magic = PROTOCOL_MAGIC;
		hdr.Type = (uint32_t)MessageType::DATA_BLOCK;
		hdr.DevNo = devNo;

		// ---- Block Header ----
		BlockHeader blkHdr;
		memset(&blkHdr, 0, sizeof(blkHdr));
		blkHdr.BlockIndex = blockIndex;
		blkHdr.DataSize = rawSize;
		blkHdr.CompressedSize = compressedSize;
		memcpy(blkHdr.Hash, hash, 32);
		blkHdr.VersionId = versionId;

		// 发送（不等待 ACK）
		if (!SendAll((uint8_t*)&hdr, sizeof(hdr))) return false;
		if (!SendAll((uint8_t*)&blkHdr, sizeof(blkHdr))) return false;
		if (!SendAll(compressedData, compressedSize)) return false;

		// 发送日志（DEBUG 级别）
		{
			wchar_t hexStr[65];
			for (int hi = 0; hi < 32; hi++)
				swprintf_s(hexStr + hi * 2, 3, L"%02X", hash[hi]);
			hexStr[64] = L'\0';

			wchar_t dbg[384];
			swprintf_s(dbg, L"[NetworkClient] SEND (pipeline) block=%llu dev=%u raw=%u compressed=%u hash=%s",
				blockIndex, devNo, rawSize, compressedSize, hexStr);
			LOG_DEBUG(dbg);
		}

		return true;
	}

	// ============================================================
	// 启动 ACK 接收线程
	// ============================================================
	void NetworkClient::StartAckReceiver(AckCallback callback)
	{
		// 如果已有线程在运行，先停止
		StopAckReceiver();

		m_ackCallback = callback;
		m_ackReceiverRunning = true;
		m_ackThread = std::thread(&NetworkClient::AckReceiverLoop, this);

		LOG_INFO(L"[NetworkClient] ACK receiver thread started");
	}

	// ============================================================
	// 停止 ACK 接收线程并等待退出
	// ============================================================
	void NetworkClient::StopAckReceiver()
	{
		if (!m_ackReceiverRunning.load())
		{
			// 线程未运行，直接返回
			if (m_ackThread.joinable())
			{
				m_ackThread.join();
			}
			return;
		}

		m_ackReceiverRunning = false;

		// 等待线程退出（select 最多 1 秒超时，很快响应）
		if (m_ackThread.joinable())
		{
			m_ackThread.join();
		}

		LOG_INFO(L"[NetworkClient] ACK receiver thread stopped");
	}

	// ============================================================
	// ACK 接收线程主循环
	// 使用 select() 轮询 socket，每秒检查退出标志
	// ============================================================
	void NetworkClient::AckReceiverLoop()
	{
		LOG_DEBUG(L"[NetworkClient] AckReceiverLoop entered");

		while (m_ackReceiverRunning.load())
		{
			// 使用 select 检查是否有数据可读（1 秒超时）
			fd_set readFds;
			FD_ZERO(&readFds);
			FD_SET(m_socket, &readFds);

			struct timeval timeout;
			timeout.tv_sec = 1;
			timeout.tv_usec = 0;

			int selectResult = select(0, &readFds, nullptr, nullptr, &timeout);

			if (selectResult == SOCKET_ERROR)
			{
				int err = WSAGetLastError();
				wchar_t msg[256];
				swprintf_s(msg, L"[NetworkClient] ACK thread select() error=%d", err);
				LOG_ERROR(msg);

				// 通知上层连接出错
				if (m_ackCallback)
				{
					uint8_t zeroHash[32] = {};
					m_ackCallback(0, 0xFFFFFFFFFFFFFFFFULL, zeroHash, (uint32_t)-1);
				}
				break;
			}

			if (selectResult == 0)
			{
				// 超时，检查退出标志后继续
				continue;
			}

			// 有数据可读，接收 ACK
			MsgHeader hdr;
			memset(&hdr, 0, sizeof(hdr));
			if (!RecvAll((uint8_t*)&hdr, sizeof(hdr)))
			{
				LOG_ERROR(L"[NetworkClient] ACK thread RecvAll(header) failed - connection lost");
				if (m_ackCallback)
				{
					uint8_t zeroHash[32] = {};
					m_ackCallback(0, 0xFFFFFFFFFFFFFFFFULL, zeroHash, (uint32_t)-2);
				}
				break;
			}

			// 验证消息头
			if (hdr.Magic != PROTOCOL_MAGIC)
			{
				LOG_ERROR(L"[NetworkClient] ACK thread: magic mismatch");
				if (m_ackCallback)
				{
					uint8_t zeroHash[32] = {};
					m_ackCallback(0, 0xFFFFFFFFFFFFFFFFULL, zeroHash, (uint32_t)-3);
				}
				break;
			}

			if (hdr.Type != (uint32_t)MessageType::ACK)
			{
				wchar_t msg[256];
				swprintf_s(msg, L"[NetworkClient] ACK thread: expected ACK(0x02), got type=0x%X", hdr.Type);
				LOG_ERROR(msg);
				if (m_ackCallback)
				{
					uint8_t zeroHash[32] = {};
					m_ackCallback(0, 0xFFFFFFFFFFFFFFFFULL, zeroHash, (uint32_t)-4);
				}
				break;
			}

			// 接收 ACK Body
			AckBody ack;
			memset(&ack, 0, sizeof(ack));
			if (!RecvAll((uint8_t*)&ack, sizeof(ack)))
			{
				LOG_ERROR(L"[NetworkClient] ACK thread RecvAll(body) failed");
				if (m_ackCallback)
				{
					uint8_t zeroHash[32] = {};
					m_ackCallback(0, 0xFFFFFFFFFFFFFFFFULL, zeroHash, (uint32_t)-5);
				}
				break;
			}

			// ACK 成功日志（DEBUG 级别）
			{
				wchar_t hexStr[65];
				for (int hi = 0; hi < 32; hi++)
					swprintf_s(hexStr + hi * 2, 3, L"%02X", ack.Hash[hi]);
				hexStr[64] = L'\0';

				wchar_t dbg[256];
				swprintf_s(dbg, L"[NetworkClient] RECV ACK (pipeline) block=%llu dev=%u status=%u hash=%s",
					ack.BlockIndex, ack.DevNo, ack.Status, hexStr);
				LOG_DEBUG(dbg);
			}

			// 调用回调通知上层
			if (m_ackCallback)
			{
				m_ackCallback(ack.DevNo, ack.BlockIndex, ack.Hash, ack.Status);
			}
		}

		LOG_DEBUG(L"[NetworkClient] AckReceiverLoop exiting");
	}

	// ============================================================
	// 发送 BYE
	// ============================================================
	bool NetworkClient::SendBye(uint32_t devNo)
	{
		if (!m_connected.load()) return false;

		LOG_INFO(L"[NetworkClient] Sending BYE...");

		MsgHeader hdr;
		memset(&hdr, 0, sizeof(hdr));
		hdr.Magic = PROTOCOL_MAGIC;
		hdr.Type = (uint32_t)MessageType::BYE;
		hdr.DevNo = devNo;

		if (!SendAll((uint8_t*)&hdr, sizeof(hdr))) return false;

		// 等待最后的 ACK
		bool ok = ReceiveAck(devNo, 0xFFFFFFFFFFFFFFFFULL);
		if (ok)
		{
			LOG_INFO(L"[NetworkClient] BYE ACK OK");
		}
		return ok;
	}

	// ============================================================
	// 断开连接
	// ============================================================
	void NetworkClient::Disconnect()
	{
		if (m_socket != INVALID_SOCKET)
		{
			closesocket(m_socket);
			m_socket = INVALID_SOCKET;
		}
		m_connected = false;
	}

	// ============================================================
	// 发送全部数据
	// ============================================================
	bool NetworkClient::SendAll(const uint8_t* data, int size)
	{
		int totalSent = 0;
		while (totalSent < size)
		{
			int sent = send(m_socket, (const char*)(data + totalSent), size - totalSent, 0);
			if (sent == SOCKET_ERROR)
			{
				int err = WSAGetLastError();
				wchar_t msg[256];
				swprintf_s(msg, L"[NetworkClient] send failed, error=%d", err);
				LOG_ERROR(msg);
				m_lastError = msg;
				m_connected = false;
				return false;
			}
			totalSent += sent;
		}
		return true;
	}

	// ============================================================
	// 接收全部数据
	// ============================================================
	bool NetworkClient::RecvAll(uint8_t* buffer, int size)
	{
		int totalRecv = 0;
		while (totalRecv < size)
		{
			int recvd = recv(m_socket, (char*)(buffer + totalRecv), size - totalRecv, 0);
			if (recvd == SOCKET_ERROR)
			{
				int err = WSAGetLastError();
				wchar_t msg[256];
				swprintf_s(msg, L"[NetworkClient] recv failed, error=%d", err);
				LOG_ERROR(msg);
				m_lastError = msg;
				m_connected = false;
				return false;
			}
			if (recvd == 0)
			{
				// 连接关闭
				m_lastError = L"Connection closed by server";
				LOG_ERROR(m_lastError.c_str());
				m_connected = false;
				return false;
			}
			totalRecv += recvd;
		}
		return true;
	}

	// ============================================================
	// 接收并解析 ACK（同步模式）
	// ============================================================
	bool NetworkClient::ReceiveAck(uint32_t devNo, uint64_t expectedBlockIndex)
	{
		// 接收 Header
		MsgHeader hdr;
		memset(&hdr, 0, sizeof(hdr));
		if (!RecvAll((uint8_t*)&hdr, sizeof(hdr))) return false;

		// 验证
		if (hdr.Magic != PROTOCOL_MAGIC)
		{
			m_lastError = L"ACK magic mismatch";
			LOG_ERROR(m_lastError.c_str());
			return false;
		}

		if (hdr.Type != (uint32_t)MessageType::ACK)
		{
			m_lastError = L"Expected ACK, got different message type";
			LOG_ERROR(m_lastError.c_str());
			return false;
		}

		// 接收 ACK Body
		AckBody ack;
		memset(&ack, 0, sizeof(ack));
		if (!RecvAll((uint8_t*)&ack, sizeof(ack))) return false;

		if (ack.Status != 0)
		{
			wchar_t msg[256];
			swprintf_s(msg, L"[NetworkClient] Server error for block %llu: status=%u",
				ack.BlockIndex, ack.Status);
			LOG_ERROR(msg);
			m_lastError = msg;
			return false;
		}

		// ACK 成功日志（DEBUG 级别）
		{
			wchar_t hexStr[65];
			for (int hi = 0; hi < 32; hi++)
				swprintf_s(hexStr + hi * 2, 3, L"%02X", ack.Hash[hi]);
			hexStr[64] = L'\0';

			wchar_t dbg[256];
			swprintf_s(dbg, L"[NetworkClient] RECV ACK block=%llu dev=%u status=OK hash=%s",
				ack.BlockIndex, ack.DevNo, hexStr);
			LOG_DEBUG(dbg);
		}

		return true;
	}

	// ============================================================
	// 设置 socket 超时
	// ============================================================
	bool NetworkClient::SetSocketTimeout(SOCKET sock, int timeoutSec)
	{
		DWORD timeout = timeoutSec * 1000;
		if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) == SOCKET_ERROR)
		{
			return false;
		}
		if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout)) == SOCKET_ERROR)
		{
			return false;
		}
		return true;
	}

		// ============================================================
		// 设置 TCP Keep-Alive，防止空闲连接被断开
		// ============================================================
		bool NetworkClient::SetTcpKeepAlive(SOCKET sock)
		{
			// 启用 keepalive
			BOOL keepalive = TRUE;
			if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (const char*)&keepalive, sizeof(keepalive)) == SOCKET_ERROR)
				return false;

			// 空闲 30 秒后开始发送 keepalive 探测
			struct tcp_keepalive ka;
			ka.onoff = 1;
			ka.keepalivetime = 30 * 1000;  // 30 秒空闲后启动
			ka.keepaliveinterval = 5 * 1000; // 每 5 秒重试一次
			if (WSAIoctl(sock, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), nullptr, 0, nullptr, nullptr, nullptr) == SOCKET_ERROR)
				return false;

			LOG_DEBUG(L"[NetworkClient] TCP Keep-Alive enabled: idle=30s interval=5s");
			return true;
		}

} // namespace Network
