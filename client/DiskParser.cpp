#include <algorithm>
#include <string.h>
#include "DiskParser.h"
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

	bool DiskParser::ParseMBR(DiskLayout& layout)
	{
		BYTE buffer[512];
		if (!Read(0, buffer, 512)) // 读取第一个扇区（MBR所在位置）
		{
			return false;
		}

		if (buffer[510] != 0x55 || buffer[511] != 0xAA) // 检查MBR签名
		{
			return false;
		}

		auto entries = reinterpret_cast<MBRPartitionEntry*>	(buffer + 446);

		//
		// GPT Protective MBR
		//
		if (entries[0].Type == 0xEE)
		{
			layout.Disk.Style = PartitionStyle::GPT;
			return true;
		}
		layout.Disk.Style = PartitionStyle::MBR;
		for (int i = 0; i < 4; i++)
		{
			if (entries[i].Type == 0)
				continue;

			PartitionInfo p{};
			p.Index = i;
			p.MbrType = entries[i].Type;
			p.Offset = (uint64_t)entries[i].StartLBA * 512;
			p.Size = (uint64_t)entries[i].SectorCount * 512;
			layout.Partitions.push_back(p);
		}
		return true;
	}

	bool DiskParser::ParseGPT(DiskLayout& layout)
	{

		GPTHeader header{};

		if (!Read(512, &header, sizeof(header)))
		{
			return false;
		}



		if (memcmp(header.Signature, "EFI PART", 8) != 0)
		{
			return false;
		}

		layout.Disk.GPT.PrimaryHeaderOffset = 512;
		layout.Disk.GPT.BackupHeaderOffset = header.BackupLBA * 512;
		layout.Disk.GPT.PrimaryEntryOffset = header.PartitionEntryLBA * 512;
		layout.Disk.GPT.EntryCount = header.NumberOfPartitionEntries;
		layout.Disk.GPT.EntrySize = header.SizeOfPartitionEntry;
		layout.Disk.GPT.DiskGuid = header.DiskGuid;
		uint64_t size = (uint64_t)header.NumberOfPartitionEntries * header.SizeOfPartitionEntry;
		std::vector<BYTE> entries(size);
		if (!Read(layout.Disk.GPT.PrimaryEntryOffset, entries.data(), (DWORD)size))
		{
			return false;
		}

		for (uint32_t i = 0; i < header.NumberOfPartitionEntries; i++)
		{

			GPTEntry* e = reinterpret_cast<GPTEntry*>(entries.data() + i * header.SizeOfPartitionEntry);

			if (IsEqualGUID(e->TypeGuid, GUID{}))
			{
				continue;
			}
			PartitionInfo p{};
			p.Index = i;
			p.TypeGuid = e->TypeGuid;
			p.PartitionGuid = e->PartitionGuid;
			p.Offset = e->FirstLBA * 512;
			p.Size = (e->LastLBA - e->FirstLBA + 1) * 512;
			p.Attributes = e->Attributes;
			p.Name = e->Name;

			layout.Partitions.push_back(p);
		}

		CalculateFreeSpace(layout);
		return true;
	}

	void DiskParser::CalculateFreeSpace(DiskLayout& layout)
	{
		//
		// 后续完善:
		// 需要先获得磁盘容量
		//
	}

	bool DiskParser::Parse(DiskLayout& layout)
	{
		memset(&layout, 0, sizeof(layout.Disk));
		layout.Disk.DeviceNumber = m_DeviceNumber;

		//
		// 暂时默认512
		// 后续从IOCTL读取
		//
		layout.Disk.SectorSize = 512;

		if (!ParseMBR(layout))
		{
			return false;
		}

		if (layout.Disk.Style == PartitionStyle::GPT)
		{
			return ParseGPT(layout);
		}

		CalculateFreeSpace(layout);
		return true;
	}


}