#include <algorithm>
#include <string.h>
#include "DiskParser.h"
#include <winioctl.h>
#include "Logger.h"

namespace Disk
{
	DiskParser::DiskParser()
	{
		m_hDisk = INVALID_HANDLE_VALUE;
		m_DeviceNumber = -1;
	}

	DiskParser::~DiskParser()
	{
		Close();
	}

	void DiskParser::Close()
	{
		if (m_hDisk != INVALID_HANDLE_VALUE)
		{
			CloseHandle(m_hDisk);
			m_hDisk = INVALID_HANDLE_VALUE;
		}
	}

	bool DiskParser::Open(int deviceNumber)
	{

		Close();
		wchar_t path[64];
		swprintf_s(path, L"\\\\.\\PhysicalDrive%d", deviceNumber);
		m_hDisk =
			CreateFileW(
				path,
				GENERIC_READ,
				FILE_SHARE_READ |
				FILE_SHARE_WRITE,
				nullptr,
				OPEN_EXISTING,
				0,
				nullptr
			);

		if (m_hDisk == INVALID_HANDLE_VALUE)
		{
			DWORD err = GetLastError();
			wchar_t buffer[256];
			swprintf_s(buffer, L"Open %s failed, error=%lu", path, err);
			LOG_ERROR(buffer);
			return false;
		}

		m_DeviceNumber = deviceNumber;
		return true;
	}

	bool DiskParser::Read(uint64_t offset, void* buffer, uint32_t size)
	{

		LARGE_INTEGER pos;

		pos.QuadPart = offset;

		// 将文件（磁盘）指针移动到目标偏移位置
		// FILE_BEGIN：从文件/设备开头开始计算偏移
		if (!SetFilePointerEx(m_hDisk, pos, nullptr, FILE_BEGIN))
		{
			DWORD err = GetLastError();  // 获取定位失败的具体错误码
			wchar_t buffer[256];
			swprintf_s(buffer, L"[DiskParser] SetFilePointerEx failed at offset %llu, error=%lu\n", offset, err);
			LOG_ERROR(buffer);
			return false;
		}

		// 从当前位置同步读取数据
		DWORD readSize = 0;
		if (!ReadFile(
			m_hDisk,       // 磁盘设备句柄
			buffer,        // 输出数据缓冲区
			size,          // 期望读取的字节长度
			&readSize,     // [输出] 实际读取的字节数
			nullptr))       // 同步模式，不使用 OVERLAPPED 结构
		{
			DWORD err = GetLastError();  // 获取读取失败的具体错误码
			wchar_t buffer[256];
			swprintf_s(buffer, L"[DiskParser] ReadFile failed at offset %llu, error=%lu\n", offset, err);
			LOG_ERROR(buffer);
			return false;
		}
		// 校验完整性：确保实际读取量等于请求量（防止读到 EOF 等部分读取场景）
		if (readSize != size)
		{
			wchar_t buffer[256];
			swprintf_s(buffer, L"[DiskParser] Partial read at offset %llu: requested=%u, actual=%lu\n", offset, size, readSize);
			LOG_ERROR(buffer);
			return false;
		}
		return true;
	}

	//
	// 查询磁盘总大小（使用 IOCTL_DISK_GET_LENGTH_INFO）
	//
	bool DiskParser::QueryDiskSize(DiskLayout& layout)
	{
		GET_LENGTH_INFORMATION lengthInfo = {};
		DWORD bytesReturned = 0;
		if (DeviceIoControl(
			m_hDisk,
			IOCTL_DISK_GET_LENGTH_INFO,
			nullptr, 0,
			&lengthInfo, sizeof(lengthInfo),
			&bytesReturned,
			nullptr))
		{
			layout.Disk.Size = lengthInfo.Length.QuadPart;
			wchar_t msg[128];
			swprintf_s(msg, L"[DiskParser] Disk size: %llu bytes", layout.Disk.Size);
			LOG_INFO(msg);
			return true;
		}
		else
		{
			DWORD err = GetLastError();
			wchar_t msg[256];
			swprintf_s(msg, L"[DiskParser] IOCTL_DISK_GET_LENGTH_INFO failed, error=%lu", err);
			LOG_WARNING(msg);
			layout.Disk.Size = 0;
			return false;
		}
	}

	//
	// 查询扇区大小（使用 IOCTL_DISK_GET_DRIVE_GEOMETRY_EX）
	// 失败时回退到传统 512 字节扇区
	//
	bool DiskParser::QuerySectorSize(DiskLayout& layout)
	{
		// DISK_GEOMETRY_EX 是变长结构，分配足够的缓冲区
		BYTE buffer[sizeof(DISK_GEOMETRY_EX) + 256] = {};
		DWORD bytesReturned = 0;
		if (DeviceIoControl(
			m_hDisk,
			IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
			nullptr, 0,
			buffer, sizeof(buffer),
			&bytesReturned,
			nullptr))
		{
			DISK_GEOMETRY_EX* geometry = reinterpret_cast<DISK_GEOMETRY_EX*>(buffer);
			layout.Disk.SectorSize = geometry->Geometry.BytesPerSector;
			wchar_t msg[128];
			swprintf_s(msg, L"[DiskParser] Sector size: %u bytes", layout.Disk.SectorSize);
			LOG_INFO(msg);
			return true;
		}
		else
		{
			DWORD err = GetLastError();
			wchar_t msg[256];
			swprintf_s(msg, L"[DiskParser] IOCTL_DISK_GET_DRIVE_GEOMETRY_EX failed, error=%lu, fallback to 512", err);
			LOG_WARNING(msg);
			// 回退到传统 512 字节扇区
			layout.Disk.SectorSize = 512;
			return false;
		}
	}

	bool DiskParser::ParseMBR(DiskLayout& layout)
{
	uint32_t sectorSize = layout.Disk.SectorSize;

	// 读取第一个扇区（MBR 固定位于磁盘开头，以扇区大小为单位读取保证对齐）
	BYTE buffer[4096] = {};
	if (!Read(0, buffer, sectorSize))
	{
		LOG_ERROR(L"[DiskParser] 读取 MBR 扇区失败");
		return false;
	}

	// 检查 MBR 签名 0x55AA（位于扇区末尾两个字节，偏移与扇区大小无关）
	if (buffer[510] != 0x55 || buffer[511] != 0xAA)
	{
		LOG_ERROR(L"[DiskParser] MBR 签名无效");
		return false;
	}

	// MBR 分区表位于扇区偏移 446 处，共 4 个条目，每个 16 字节
	auto entries = reinterpret_cast<MBRPartitionEntry*>(buffer + 446);

	//
	// GPT Protective MBR: 第一个分区条目类型为 0xEE 表示此磁盘使用 GPT
	//
	if (entries[0].Type == 0xEE)
	{
		layout.Disk.Style = PartitionStyle::GPT;
		// 即使是 GPT 磁盘，MBR 扇区本身也是重要的磁盘元数据
		DiskRange meta;
		meta.Offset = 0;
		meta.Size = sectorSize;
		layout.MetadataRanges.push_back(meta);
		return true;
	}

	layout.Disk.Style = PartitionStyle::MBR;

	// 添加 MBR 扇区到磁盘元数据区域
	{
		DiskRange meta;
		meta.Offset = 0;
		meta.Size = sectorSize;
		layout.MetadataRanges.push_back(meta);
	}

	// 遍历 4 个主分区条目，将 LBA 转换为字节偏移
	for (int i = 0; i < 4; i++)
	{
		if (entries[i].Type == 0)
			continue;

		PartitionInfo p{};
		p.Index = i;
		p.MbrType = entries[i].Type;
		p.Offset = (uint64_t)entries[i].StartLBA * sectorSize;
		p.Size = (uint64_t)entries[i].SectorCount * sectorSize;
		layout.Partitions.push_back(p);
	}
	return true;
}

	bool DiskParser::ParseGPT(DiskLayout& layout)
{
	uint32_t sectorSize = layout.Disk.SectorSize;

	// 读取 GPT Header（位于 LBA 1，即偏移 = 1 * 扇区大小）
	GPTHeader header{};
	if (!Read(sectorSize, &header, sizeof(header)))
	{
		LOG_ERROR(L"[DiskParser] 读取 GPT Header 失败");
		return false;
	}

	// 验证 GPT 签名 "EFI PART"
	if (memcmp(header.Signature, "EFI PART", 8) != 0)
	{
		LOG_ERROR(L"[DiskParser] GPT 签名无效");
		return false;
	}

	// 填充 GPT 布局信息（LBA -> 字节偏移转换）
	layout.Disk.GPT.PrimaryHeaderOffset = sectorSize;  // LBA 1
	layout.Disk.GPT.BackupHeaderOffset = header.BackupLBA * sectorSize;
	layout.Disk.GPT.PrimaryEntryOffset = header.PartitionEntryLBA * sectorSize;
	// 备份分区表条目数组位于备份 Header 之前
	layout.Disk.GPT.BackupEntryOffset = header.BackupLBA * sectorSize
		- (uint64_t)header.NumberOfPartitionEntries * header.SizeOfPartitionEntry;
	layout.Disk.GPT.EntryCount = header.NumberOfPartitionEntries;
	layout.Disk.GPT.EntrySize = header.SizeOfPartitionEntry;
	layout.Disk.GPT.DiskGuid = header.DiskGuid;

	// 读取主分区表条目数组
	uint64_t entryArrayBytes = (uint64_t)header.NumberOfPartitionEntries * header.SizeOfPartitionEntry;
	std::vector<BYTE> entries(entryArrayBytes);
	if (!Read(layout.Disk.GPT.PrimaryEntryOffset, entries.data(), (DWORD)entryArrayBytes))
	{
		LOG_ERROR(L"[DiskParser] 读取 GPT 分区表条目失败");
		return false;
	}

	// 遍历所有分区条目
	for (uint32_t i = 0; i < header.NumberOfPartitionEntries; i++)
	{
		GPTEntry* e = reinterpret_cast<GPTEntry*>(entries.data() + i * header.SizeOfPartitionEntry);

		// 空条目（TypeGuid 全为零）跳过
		if (IsEqualGUID(e->TypeGuid, GUID{}))
		{
			continue;
		}

		PartitionInfo p{};
		p.Index = i;
		p.TypeGuid = e->TypeGuid;
		p.PartitionGuid = e->PartitionGuid;
		// 将 LBA 转换为字节偏移
		p.Offset = e->FirstLBA * sectorSize;
		p.Size = (e->LastLBA - e->FirstLBA + 1) * sectorSize;
		p.Attributes = e->Attributes;
		p.Name = e->Name;

		layout.Partitions.push_back(p);
	}

	// ========================================
	// 填充 GPT 磁盘元数据区域（MetadataRanges）
	// 这些区域对灾难恢复至关重要，必须在备份分区数据之前备份
	// ========================================
	{
		// 1. Protective MBR (LBA 0)
		DiskRange protectiveMBR;
		protectiveMBR.Offset = 0;
		protectiveMBR.Size = sectorSize;
		layout.MetadataRanges.push_back(protectiveMBR);

		// 2. GPT Header (LBA 1)
		DiskRange gptHeaderMeta;
		gptHeaderMeta.Offset = sectorSize;
		gptHeaderMeta.Size = sectorSize;
		layout.MetadataRanges.push_back(gptHeaderMeta);

		// 3. GPT 主分区表条目数组 (LBA 2 ~ LBA 33)
		DiskRange gptEntriesMeta;
		gptEntriesMeta.Offset = header.PartitionEntryLBA * sectorSize;
		gptEntriesMeta.Size = entryArrayBytes;
		layout.MetadataRanges.push_back(gptEntriesMeta);

		// 4. GPT 备份分区表条目数组（位于磁盘末尾，备份 Header 之前）
		uint64_t backupEntriesOffset = header.BackupLBA * sectorSize - entryArrayBytes;
		DiskRange backupEntriesMeta;
		backupEntriesMeta.Offset = backupEntriesOffset;
		backupEntriesMeta.Size = entryArrayBytes;
		layout.MetadataRanges.push_back(backupEntriesMeta);

		// 5. GPT 备份 Header（位于磁盘最后一个 LBA）
		DiskRange backupHeaderMeta;
		backupHeaderMeta.Offset = header.BackupLBA * sectorSize;
		backupHeaderMeta.Size = sectorSize;
		layout.MetadataRanges.push_back(backupHeaderMeta);
	}

	CalculateFreeSpace(layout);
	return true;
}

	void DiskParser::CalculateFreeSpace(DiskLayout& layout)
{
	layout.FreeRanges.clear();

	// 如果没有分区，整个磁盘都是未分配空间
	if (layout.Partitions.empty())
	{
		if (layout.Disk.Size > 0)
		{
			DiskRange range;
			range.Offset = 0;
			range.Size = layout.Disk.Size;
			layout.FreeRanges.push_back(range);
		}
		return;
	}

	// 按分区起始偏移量排序（使用指针避免拷贝）
	std::vector<PartitionInfo*> sorted;
	for (auto& p : layout.Partitions)
	{
		sorted.push_back(&p);
	}
	std::sort(sorted.begin(), sorted.end(),
		[](const PartitionInfo* a, const PartitionInfo* b)
		{
			return a->Offset < b->Offset;
		});

	uint64_t current = 0;  // 当前扫描位置（已分配区域的末尾）

	for (auto* p : sorted)
	{
		// 如果当前分区起始偏移大于已扫描位置，中间存在未分配空隙
		if (p->Offset > current)
		{
			DiskRange range;
			range.Offset = current;
			range.Size = p->Offset - current;
			layout.FreeRanges.push_back(range);
		}

		// 移动扫描位置到当前分区末尾
		uint64_t partitionEnd = p->Offset + p->Size;
		if (partitionEnd > current)
		{
			current = partitionEnd;
		}
	}

	// 最后一个分区之后到磁盘末尾的未分配空间
	if (layout.Disk.Size > 0 && current < layout.Disk.Size)
	{
		DiskRange range;
		range.Offset = current;
		range.Size = layout.Disk.Size - current;
		layout.FreeRanges.push_back(range);
	}
}

	bool DiskParser::Parse(DiskLayout& layout)
{
	// 初始化 Disk 结构体（零值初始化）
	memset(&layout, 0, sizeof(layout.Disk));
	layout.Disk.DeviceNumber = m_DeviceNumber;

	// 设置默认值，后续查询会覆盖
	layout.Disk.SectorSize = 512;
	layout.Disk.Size = 0;

	// 查询磁盘总大小（失败不致命，Size 保持 0）
	if (!QueryDiskSize(layout))
	{
		LOG_WARNING(L"[DiskParser] QueryDiskSize failed, FreeSpace tail gap will be unavailable");
	}

	// 查询扇区大小（失败时自动回退到默认 512）
	if (!QuerySectorSize(layout))
	{
		LOG_WARNING(L"[DiskParser] QuerySectorSize failed, using default 512");
	}

	// 解析 MBR（或识别 GPT Protective MBR）
	if (!ParseMBR(layout))
	{
		LOG_ERROR(L"[DiskParser] MBR 解析失败");
		return false;
	}

	// GPT 磁盘：进一步解析 GPT Header 和分区表
	if (layout.Disk.Style == PartitionStyle::GPT)
	{
		if (!ParseGPT(layout))
		{
			LOG_ERROR(L"[DiskParser] GPT 解析失败");
			return false;
		}
		return true;
	}

	// MBR 磁盘：计算未分配空间
	CalculateFreeSpace(layout);
	return true;
}


}