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
	{
		InitializeWinsock();
	}

	NetworkClient::~NetworkClient()
	{
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

		m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (m_socket == INVALID_SOCKET)
		{
			m_lastError = L"Failed to create socket";
			LOG_ERROR(m_lastError.c_str());
			return false;
		}

		// 设置超时
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
		if (!m_connected) return false;

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
		return ReceiveAck(devNo, 0);
	}

	// ============================================================
	// 发送数据块 + 等待 ACK
	// ============================================================
	bool NetworkClient::SendBlock(uint32_t devNo, uint64_t blockIndex,
		const uint8_t* rawData, uint32_t rawSize,
		const uint8_t* compressedData, uint32_t compressedSize,
		const uint8_t hash[32], uint32_t versionId)
	{
		if (!m_connected) return false;

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
	// 发送 BYE
	// ============================================================
	bool NetworkClient::SendBye(uint32_t devNo)
	{
		if (!m_connected) return false;

		MsgHeader hdr;
		memset(&hdr, 0, sizeof(hdr));
		hdr.Magic = PROTOCOL_MAGIC;
		hdr.Type = (uint32_t)MessageType::BYE;
		hdr.DevNo = devNo;

		if (!SendAll((uint8_t*)&hdr, sizeof(hdr))) return false;

		// 等待最后的 ACK
		return ReceiveAck(devNo, 0xFFFFFFFFFFFFFFFFULL);
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
				m_connected = false;
				return false;
			}
			totalRecv += recvd;
		}
		return true;
	}

	// ============================================================
	// 接收并解析 ACK
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

		// 校验 Hash 回传（服务端确认）
		// Hash 验证由调用方处理

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

} // namespace Network
