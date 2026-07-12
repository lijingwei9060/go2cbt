#include "VolumeMapper.h"
#include "Logger.h"


namespace VolumeMapping
{

	VolumeMapper::VolumeMapper()
	{
	}

	VolumeMapper::~VolumeMapper()
	{
	}

	// ============================================================
	// 枚举所有卷 GUID 路径
	// FindFirstVolumeW / FindNextVolumeW 返回格式: \\?\Volume{GUID}\
	// ============================================================
	std::vector<std::wstring> VolumeMapper::EnumerateVolumes()
	{
		std::vector<std::wstring> volumes;
		wchar_t volumeName[MAX_PATH] = {};

		HANDLE hFind = FindFirstVolumeW(volumeName, ARRAYSIZE(volumeName));
		if (hFind == INVALID_HANDLE_VALUE)
		{
			DWORD err = GetLastError();
			wchar_t msg[256];
			swprintf_s(msg, L"[VolumeMapper] FindFirstVolumeW failed, error=%lu", err);
			LOG_ERROR(msg);
			return volumes;
		}

		do
		{
			volumes.push_back(volumeName);
		} while (FindNextVolumeW(hFind, volumeName, ARRAYSIZE(volumeName)));

		FindVolumeClose(hFind);

		wchar_t msg[256];
		swprintf_s(msg, L"[VolumeMapper] Enumerated %zu volumes", volumes.size());
		LOG_INFO(msg);

		return volumes;
	}

	// ============================================================
	// 查询卷的物理磁盘扩展信息
	// volumeGuid: \\?\Volume{GUID}\  格式
	//
	// 注意：打开卷设备时需要去掉末尾的反斜杠
	//       \\?\Volume{GUID}\  →  \\?\Volume{GUID}
	// ============================================================
	bool VolumeMapper::GetVolumeDiskExtents(const std::wstring& volumeGuid, VolumeInfo& info)
	{
		info.VolumeGuid = volumeGuid;
		info.DriveLetter = GetDriveLetter(volumeGuid);
		info.DiskNumber = -1;
		info.DiskOffset = 0;
		info.Size = 0;

		// 去掉末尾反斜杠：打开卷设备需要 \\?\Volume{GUID} 格式
		std::wstring devicePath = volumeGuid;
		if (!devicePath.empty() && devicePath.back() == L'\\')
		{
			devicePath.pop_back();
		}

		// 打印进度：在打开卷之前输出，便于排查卡住位置
		{
			wchar_t dbg[512];
			swprintf_s(dbg, L"[VolumeMapper] Opening %s (drive: %s)...",
				devicePath.c_str(),
				info.DriveLetter.empty() ? L"none" : info.DriveLetter.c_str());
			LOG_INFO(dbg);
		}

		// 打开卷设备（仅查询属性，不需要读/写权限）
		// 使用 0 访问权限而非 GENERIC_READ，避免 CreateFileW 被磁盘
		// 上未完成的 I/O 串行化阻塞——在 IO 繁忙时可能无限期挂起。
		// IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS 是纯查询操作，
		// 不需要任何数据访问权限。
		HANDLE hVolume = CreateFileW(
			devicePath.c_str(),
			0,                           // 0 = 仅查询，无需读/写权限
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr,
			OPEN_EXISTING,
			0,
			nullptr
		);

		if (hVolume == INVALID_HANDLE_VALUE)
		{
			DWORD err = GetLastError();
			// ERROR_ACCESS_DENIED (5) 是常见情况：需要管理员权限
			if (err != ERROR_ACCESS_DENIED)
			{
				wchar_t msg[512];
				swprintf_s(msg, L"[VolumeMapper] Cannot open volume %s, error=%lu", devicePath.c_str(), err);
				LOG_WARNING(msg);
			}
			return false;
		}

		// 查询磁盘扩展信息
		// VOLUME_DISK_EXTENTS 包含可变长度的 DISK_EXTENT 数组
		BYTE buffer[sizeof(VOLUME_DISK_EXTENTS) + 8 * sizeof(DISK_EXTENT)] = {};
		DWORD bytesReturned = 0;

		BOOL result = DeviceIoControl(
			hVolume,
			IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
			nullptr, 0,
			buffer, sizeof(buffer),
			&bytesReturned,
			nullptr
		);

		CloseHandle(hVolume);

		if (!result)
		{
			DWORD err = GetLastError();
			wchar_t msg[512];
			swprintf_s(msg, L"[VolumeMapper] IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS failed for %s, error=%lu",
				devicePath.c_str(), err);
			LOG_WARNING(msg);
			return false;
		}

		PVOLUME_DISK_EXTENTS extents = reinterpret_cast<PVOLUME_DISK_EXTENTS>(buffer);
		if (extents->NumberOfDiskExtents == 0)
		{
			return false;
		}

		// 取第一个扩展（大多数卷只有一个扩展）
		// 跨区卷（spanned volume）可能有多个扩展，暂不处理
		PDISK_EXTENT extent = &extents->Extents[0];
		info.DiskNumber = extent->DiskNumber;
		info.DiskOffset = extent->StartingOffset.QuadPart;
		info.Size = extent->ExtentLength.QuadPart;

		wchar_t msg[512];
		swprintf_s(msg, L"[VolumeMapper] Volume %s → Disk%d Offset=0x%llx Size=%llu",
			info.DriveLetter.empty() ? L"(no letter)" : info.DriveLetter.c_str(),
			info.DiskNumber, info.DiskOffset, info.Size);
		LOG_INFO(msg);

		return true;
	}

	// ============================================================
	// 查询卷的盘符
	// volumeGuid: \\?\Volume{GUID}\  格式
	// 返回 "C:" 格式的盘符，无盘符时返回空字符串
	// ============================================================
	std::wstring VolumeMapper::GetDriveLetter(const std::wstring& volumeGuid)
	{
		// GetVolumePathNamesForVolumeNameW 需要 volumeGuid 作为输入
		// 返回与此卷关联的所有挂载点路径（盘符和 NTFS 挂载点）
		wchar_t pathNames[MAX_PATH * 4] = {};
		DWORD pathLen = 0;

		// 第一次调用获取所需缓冲区大小
		// 传入 NULL 和 0 会失败并返回 ERROR_MORE_DATA
		BOOL result = GetVolumePathNamesForVolumeNameW(
			volumeGuid.c_str(),
			pathNames,
			ARRAYSIZE(pathNames),
			&pathLen
		);

		if (!result)
		{
			// 无盘符是常见情况（ESP、恢复分区等），不是错误
			return L"";
		}

		// 返回的第一个路径（通常是盘符 "C:\"）
		// 格式: "C:\\0D:\0\0"（双空字符终止的多字符串列表）
		std::wstring firstPath(pathNames);

		// 去掉末尾反斜杠: "C:\" → "C:"
		if (!firstPath.empty() && firstPath.back() == L'\\')
		{
			firstPath.pop_back();
		}

		return firstPath;
	}

	// ============================================================
	// 执行卷映射
	// 遍历所有卷，查询磁盘扩展信息，与 DiskLayout 分区按偏移匹配
	// ============================================================
	bool VolumeMapper::Map(const Disk::DiskLayout& layout)
	{
		m_volumes.clear();
		m_mapped.clear();

		// Step 1: 枚举所有卷 GUID
		std::vector<std::wstring> volumeGuids = EnumerateVolumes();
		if (volumeGuids.empty())
		{
			LOG_WARNING(L"[VolumeMapper] No volumes found");
			return false;
		}

		// Step 2: 查询每个卷的磁盘扩展信息
		for (const auto& guid : volumeGuids)
		{
			VolumeInfo info;
			if (GetVolumeDiskExtents(guid, info))
			{
				m_volumes.push_back(info);
			}
		}

		wchar_t msg[256];
		swprintf_s(msg, L"[VolumeMapper] %zu volumes with disk extents", m_volumes.size());
		LOG_INFO(msg);

		// Step 3: 将卷的物理偏移与 DiskLayout 分区列表匹配
		for (const auto& vol : m_volumes)
		{
			// 只匹配当前解析的磁盘
			if (vol.DiskNumber != layout.Disk.DeviceNumber)
			{
				continue;
			}

			// 按偏移精确匹配分区
			bool matched = false;
			for (const auto& part : layout.Partitions)
			{
				if (vol.DiskOffset == part.Offset)
				{
					MappedPartition mp;
					mp.Partition = part;
					mp.VolumeGuid = vol.VolumeGuid;
					mp.DriveLetter = vol.DriveLetter;
					m_mapped.push_back(mp);
					matched = true;

					wchar_t matchMsg[512];
					swprintf_s(matchMsg, L"[VolumeMapper] Matched partition[%u] @0x%llx → %s %s",
						part.Index, part.Offset,
						vol.DriveLetter.empty() ? L"(no letter)" : vol.DriveLetter.c_str(),
						vol.VolumeGuid.c_str());
					LOG_INFO(matchMsg);

					break;
				}
			}

			if (!matched)
			{
				wchar_t unmatchMsg[512];
				swprintf_s(unmatchMsg, L"[VolumeMapper] Unmatched volume: Disk%d Offset=0x%llx %s",
					vol.DiskNumber, vol.DiskOffset,
					vol.DriveLetter.empty() ? L"" : vol.DriveLetter.c_str());
				LOG_INFO(unmatchMsg);
			}
		}

		swprintf_s(msg, L"[VolumeMapper] Matched %zu partitions of %zu volumes",
			m_mapped.size(), m_volumes.size());
		LOG_INFO(msg);

		return !m_mapped.empty();
	}

	// ============================================================
	// 按偏移查找卷 GUID
	// ============================================================
	std::wstring VolumeMapper::FindVolumeGuid(uint64_t offset) const
	{
		for (const auto& mp : m_mapped)
		{
			if (mp.Partition.Offset == offset)
			{
				return mp.VolumeGuid;
			}
		}
		return L"";
	}

} // namespace VolumeMapping
