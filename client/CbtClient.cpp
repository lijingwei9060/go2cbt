#include "CbtClient.h"
#include "Logger.h"
#include <string.h>


namespace CbtDriver
{

	static const wchar_t* CBT_DEVICE_NAME = L"\\\\.\\CbtMonitor";

	CbtClient::CbtClient()
		: m_hDevice(INVALID_HANDLE_VALUE)
		, m_connected(false)
	{
	}

	CbtClient::~CbtClient()
	{
		Disconnect();
	}

	// ============================================================
	// 连接 CBT 驱动
	// ============================================================
	bool CbtClient::Connect()
	{
		if (m_connected)
		{
			return true;
		}

		m_hDevice = CreateFileW(
			CBT_DEVICE_NAME,
			GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			nullptr
		);

		if (m_hDevice == INVALID_HANDLE_VALUE)
		{
			DWORD err = GetLastError();
			if (err == ERROR_FILE_NOT_FOUND)
			{
				// 驱动未加载，尝试启动
				LOG_INFO(L"[CbtClient] Driver not loaded, attempting to start...");
				if (StartDriverService())
				{
					// 重试打开
					m_hDevice = CreateFileW(
						CBT_DEVICE_NAME,
						GENERIC_READ | GENERIC_WRITE,
						FILE_SHARE_READ | FILE_SHARE_WRITE,
						nullptr,
						OPEN_EXISTING,
						FILE_ATTRIBUTE_NORMAL,
						nullptr
					);
				}
			}

			if (m_hDevice == INVALID_HANDLE_VALUE)
			{
				err = GetLastError();
				wchar_t msg[256];
				swprintf_s(msg, L"[CbtClient] Cannot open %s, error=%lu", CBT_DEVICE_NAME, err);
				LOG_ERROR(msg);
				return false;
			}
		}

		m_connected = true;
		LOG_INFO(L"[CbtClient] Connected to CBT driver");
		return true;
	}

	// ============================================================
	// 两步查询 CBT 位图
	// ============================================================
	bool CbtClient::Query(ULONG devNo, std::vector<uint8_t>& bitmap, ULONGLONG& totalBits)
	{
		if (!m_connected || m_hDevice == INVALID_HANDLE_VALUE)
		{
			LOG_ERROR(L"[CbtClient] Not connected");
			return false;
		}

		bitmap.clear();
		totalBits = 0;

		CBT_IOCTL_INPUT input;
		memset(&input, 0, sizeof(input));
		input.DeviceNumber = devNo;

		// ==== 第一步: 小缓冲区获取 TotalBits / TotalBytes ====
		BYTE metaBuf[16] = {};  // sizeof(CBT_QUERY_OUTPUT header)
		DWORD bytesReturned = 0;

		BOOL ok = DeviceIoControl(
			m_hDevice,
			IOCTL_CBT_QUERY,
			&input, sizeof(input),
			metaBuf, sizeof(metaBuf),
			&bytesReturned,
			nullptr
		);

		// STATUS_BUFFER_OVERFLOW → ERROR_MORE_DATA（预期行为）
		if (!ok && GetLastError() != ERROR_MORE_DATA)
		{
			DWORD err = GetLastError();
			wchar_t msg[256];
			swprintf_s(msg, L"[CbtClient] IOCTL_CBT_QUERY step1 failed for Disk%lu, error=%lu",
				devNo, err);
			LOG_ERROR(msg);
			return false;
		}

		CBT_QUERY_OUTPUT* pMeta = reinterpret_cast<CBT_QUERY_OUTPUT*>(metaBuf);
		if (pMeta->TotalBytes == 0 || pMeta->TotalBits == 0)
		{
			wchar_t msg[256];
			swprintf_s(msg, L"[CbtClient] Disk%lu returned zero-sized bitmap", devNo);
			LOG_ERROR(msg);
			return false;
		}

		totalBits = pMeta->TotalBits;
		ULONGLONG totalBytes = pMeta->TotalBytes;

		// ==== 第二步: 分配精确大小的缓冲区，完整读取位图 ====
		DWORD requiredSize = (DWORD)(FIELD_OFFSET(CBT_QUERY_OUTPUT, BitmapData) + totalBytes);
		std::vector<BYTE> buf(requiredSize);
		memset(buf.data(), 0, requiredSize);

		bytesReturned = 0;
		ok = DeviceIoControl(
			m_hDevice,
			IOCTL_CBT_QUERY,
			&input, sizeof(input),
			buf.data(), requiredSize,
			&bytesReturned,
			nullptr
		);

		if (!ok)
		{
			DWORD err = GetLastError();
			wchar_t msg[256];
			swprintf_s(msg, L"[CbtClient] IOCTL_CBT_QUERY step2 failed for Disk%lu, error=%lu",
				devNo, err);
			LOG_ERROR(msg);
			return false;
		}

		CBT_QUERY_OUTPUT* output = reinterpret_cast<CBT_QUERY_OUTPUT*>(buf.data());
		bitmap.assign(output->BitmapData, output->BitmapData + totalBytes);

		wchar_t msg[256];
		swprintf_s(msg, L"[CbtClient] Disk%lu CBT Query: %llu blocks, %llu bytes",
			devNo, totalBits, totalBytes);
		LOG_INFO(msg);

		return true;
	}

	// ============================================================
	// 解析位图 → 变更块索引
	// ============================================================
	void CbtClient::ParseChangedBlocks(const std::vector<uint8_t>& bitmap, ULONGLONG totalBits,
		std::vector<uint64_t>& changedBlocks)
	{
		changedBlocks.clear();
		ULONGLONG totalBytes = bitmap.size();

		for (ULONGLONG i = 0; i < totalBits; i++)
		{
			ULONGLONG byteIndex = i / 8;
			int bitInByte = 7 - (int)(i % 8);  // MSB first

			if (byteIndex >= totalBytes)
			{
				break;
			}

			if ((bitmap[(size_t)byteIndex] >> bitInByte) & 1)
			{
				changedBlocks.push_back(i);
			}
		}

		wchar_t msg[256];
		swprintf_s(msg, L"[CbtClient] %zu changed blocks out of %llu total",
			changedBlocks.size(), totalBits);
		LOG_INFO(msg);
	}

	// ============================================================
	// 重置 CBT 位图
	// ============================================================
	bool CbtClient::Reset(ULONG devNo)
	{
		if (!m_connected || m_hDevice == INVALID_HANDLE_VALUE)
		{
			LOG_ERROR(L"[CbtClient] Not connected");
			return false;
		}

		CBT_IOCTL_INPUT input;
		memset(&input, 0, sizeof(input));
		input.DeviceNumber = devNo;

		DWORD bytesReturned = 0;
		BOOL ok = DeviceIoControl(
			m_hDevice,
			IOCTL_CBT_RESET,
			&input, sizeof(input),
			nullptr, 0,
			&bytesReturned,
			nullptr
		);

		if (!ok)
		{
			DWORD err = GetLastError();
			wchar_t msg[256];
			swprintf_s(msg, L"[CbtClient] IOCTL_CBT_RESET failed for Disk%lu, error=%lu",
				devNo, err);
			LOG_ERROR(msg);
			return false;
		}

		wchar_t msg[128];
		swprintf_s(msg, L"[CbtClient] Disk%lu CBT bitmap reset", devNo);
		LOG_INFO(msg);

		return true;
	}

	// ============================================================
	// 断开连接
	// ============================================================
	void CbtClient::Disconnect()
	{
		if (m_hDevice != INVALID_HANDLE_VALUE)
		{
			CloseHandle(m_hDevice);
			m_hDevice = INVALID_HANDLE_VALUE;
		}
		m_connected = false;
	}

	// ============================================================
	// 启动 go2cbt 驱动服务
	// ============================================================
	bool CbtClient::StartDriverService()
	{
		// 打开 SCM
		SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
		if (!hSCM)
		{
			DWORD err = GetLastError();
			wchar_t msg[256];
			swprintf_s(msg, L"[CbtClient] OpenSCManager failed, error=%lu", err);
			LOG_ERROR(msg);
			return false;
		}

		// 打开 go2cbt 服务
		SC_HANDLE hService = OpenServiceW(hSCM, L"go2cbt", SERVICE_START | SERVICE_QUERY_STATUS);
		if (!hService)
		{
			DWORD err = GetLastError();
			wchar_t msg[256];
			swprintf_s(msg, L"[CbtClient] go2cbt service not found (error=%lu). "
				L"Please install: sc create go2cbt type=kernel binpath=<path>\\go2cbt.sys start=demand",
				err);
			LOG_ERROR(msg);
			CloseServiceHandle(hSCM);
			return false;
		}

		// 查询服务状态
		SERVICE_STATUS status;
		if (QueryServiceStatus(hService, &status))
		{
			if (status.dwCurrentState == SERVICE_RUNNING)
			{
				LOG_INFO(L"[CbtClient] go2cbt service already running");
				CloseServiceHandle(hService);
				CloseServiceHandle(hSCM);
				return true;
			}
		}

		// 启动服务
		if (!StartServiceW(hService, 0, nullptr))
		{
			DWORD err = GetLastError();
			if (err != ERROR_SERVICE_ALREADY_RUNNING)
			{
				wchar_t msg[256];
				swprintf_s(msg, L"[CbtClient] StartService go2cbt failed, error=%lu", err);
				LOG_ERROR(msg);
				CloseServiceHandle(hService);
				CloseServiceHandle(hSCM);
				return false;
			}
		}

		// 等待服务启动完成
		for (int i = 0; i < 10; i++)
		{
			Sleep(500);
			if (QueryServiceStatus(hService, &status))
			{
				if (status.dwCurrentState == SERVICE_RUNNING)
				{
					LOG_INFO(L"[CbtClient] go2cbt service started successfully");
					CloseServiceHandle(hService);
					CloseServiceHandle(hSCM);
					return true;
				}
			}
		}

		wchar_t msg[256];
		swprintf_s(msg, L"[CbtClient] go2cbt service start timeout (state=%lu)",
			status.dwCurrentState);
		LOG_WARNING(msg);

		CloseServiceHandle(hService);
		CloseServiceHandle(hSCM);
		return false;
	}

} // namespace CbtDriver
