#include "DiskScanner.h"

#include "Logger.h"

#include <algorithm>
#include <vector>


namespace BackupSystem
{


	DiskScanner::DiskScanner()
	{

	}


	DiskScanner::~DiskScanner()
	{

	}




	//
	// 打开 PhysicalDrive
	//
	bool DiskScanner::OpenDisk(int devno, HANDLE& handle)
	{

		wchar_t path[64] = { 0 };
		swprintf_s(path, L"\\\\.\\PhysicalDrive%d", devno);

		handle = CreateFileW(
			path,
			GENERIC_READ,
			FILE_SHARE_READ |
			FILE_SHARE_WRITE,
			nullptr,
			OPEN_EXISTING,
			0,
			nullptr
		);


		if (handle == INVALID_HANDLE_VALUE)
		{
			DWORD err = GetLastError();
			wchar_t buffer[256];
			swprintf_s(buffer, L"Open %s failed, error=%lu", path, err);
			LOG_ERROR(buffer);
			return false;
		}

		return true;
	}







	//
	// 查询全部磁盘
	//
	bool DiskScanner::EnumerateAll(std::vector<DiskInfo>& disks)
	{

		disks.clear();

		//
		// Windows支持磁盘数量通常很少
		//
		// 这里扫描0-127
		//
		for (int i = 0; i < 128; i++)
		{

			DiskInfo disk;


			if (Enumerate(i, disk))
			{
				disks.push_back(disk);
			}

		}


		return !disks.empty();

	}






	//
	// 查询指定磁盘
	//
	bool DiskScanner::Enumerate(int devno, DiskInfo& disk)
	{

		HANDLE hDisk;


		if (!OpenDisk(devno, hDisk))
		{
			return false;
		}

		disk.DevNo = devno;
		wchar_t path[64];

		swprintf_s(path, L"\\\\.\\PhysicalDrive%d", devno);

		disk.PhysicalPath = path;

		bool result = true;



		if (!QueryDiskSize(hDisk, disk.Size))
		{
			LOG_ERROR(L"QueryDiskSize %d failed", devno);
			result = false;
		}



		if (!QueryGeometry(hDisk, disk))
		{
			LOG_ERROR(L"QueryGeometry %d failed", devno);
			result = false;
		}


		if (!QueryStorageInfo(hDisk, disk))
		{

			LOG_WARNING(L"Storage descriptor unavailable");
		}



		if (!QueryLayout(hDisk, disk))
		{
			LOG_ERROR(L"QueryLayout %d failed", devno);
			result = false;
		}

		CalculateUnallocated(disk);

		CloseHandle(hDisk);

		wchar_t buffer[256];
		swprintf_s(buffer, L"Open %s result=%d", path, result);
		LOG_ERROR(buffer);
		return result;

	}








	//
	// 查询容量
	//
	bool DiskScanner::QueryDiskSize(HANDLE hDisk, uint64_t& size)
	{

		GET_LENGTH_INFORMATION info{};
		DWORD returned = 0;

		if (!DeviceIoControl(
			hDisk,
			IOCTL_DISK_GET_LENGTH_INFO,
			nullptr,
			0,
			&info,
			sizeof(info),
			&returned,
			nullptr))
		{
			return false;
		}



		size = info.Length.QuadPart;


		return true;

	}








	//
	// 查询磁盘几何信息
	//
	bool DiskScanner::QueryGeometry(HANDLE hDisk, DiskInfo& disk)
	{

		BYTE buffer[sizeof(DISK_GEOMETRY_EX) + 1024];
		DWORD returned = 0;



		if (!DeviceIoControl(
			hDisk,
			IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
			nullptr,
			0,
			buffer,
			sizeof(buffer),
			&returned,
			nullptr))
		{
			return false;
		}




		DISK_GEOMETRY_EX* geometry = reinterpret_cast<DISK_GEOMETRY_EX*>(buffer);




		disk.BytesPerSector = geometry->Geometry.BytesPerSector;



		return true;

	}









	//
	// 查询分区布局
	//
	bool DiskScanner::QueryLayout(HANDLE hDisk, DiskInfo& disk)
	{

		LOG_INFO(L"QueryLayout start");


		DWORD initialSize = 4096;
		std::vector<BYTE> buffer(initialSize);              // ← 用真实的小缓冲区代替 NULL/0
		DWORD bytesReturned = 0;
		//
		// 第一次调用获取需要的buffer大小
		// IOCTL_DISK_GET_DRIVE_LAYOUT_EX 在 NULL 缓冲区探测时的行为缺陷：
		// 
		// 当 OutputBuffer = NULL, OutputBufferLength = 0 时，虽然 DeviceIoControl 返回 FALSE + GetLastError() = ERROR_INSUFFICIENT_BUFFER，但 BytesReturned（即这里的 requiredSize）可能不会被回填为正确的值。
		// 
		//	原因：IOCTL_DISK_GET_DRIVE_LAYOUT_EX 的底层磁盘类驱动(disk.sys) 在输出缓冲区为 NULL 时，有时不会计算并回填所需大小，而是直接让 requiredSize 保持调用前的值（0）。
		//

		while (true)
		{
			DWORD bufferSize = static_cast<DWORD>(buffer.size());
			bytesReturned = 0;
			BOOL result =
				DeviceIoControl(
					hDisk,
					IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
					nullptr,
					0,
					buffer.data(),
					buffer.size(),
					&bytesReturned,
					nullptr
				);

			if (result)
			{
				LOG_INFO(L"IOCTL_DISK_GET_DRIVE_LAYOUT_EX success");
				break;
			}

			DWORD error = GetLastError();
			wchar_t msg[256];
			swprintf_s(msg, L"GET_DRIVE_LAYOUT_EX failed, error=%lu buffer=%lu returned=%lu", error, bufferSize, bytesReturned);
			LOG_WARNING(msg);

			//
			// buffer不足
			//
			// 扩大后重新尝试
			//
			if (error == ERROR_INSUFFICIENT_BUFFER)
			{
				bufferSize *= 2;

				//
				// 防止异常设备返回无限扩大
				//
				if (bufferSize > 1024 * 1024)
				{
					LOG_ERROR(L"Layout buffer exceed 1MB");
					return false;
				}

				buffer.resize(bufferSize);
				continue;
			}

			return false;
		}

		if (bytesReturned < sizeof(DRIVE_LAYOUT_INFORMATION_EX))
		{
			LOG_ERROR(L"Invalid layout returned size");
			return false;
		}

		DRIVE_LAYOUT_INFORMATION_EX* layout = reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(buffer.data());

		//
		// 输出PartitionStyle
		//
		switch (layout->PartitionStyle)
		{

		case PARTITION_STYLE_MBR:

			LOG_INFO(L"PartitionStyle=MBR");
			break;


		case PARTITION_STYLE_GPT:

			LOG_INFO(L"PartitionStyle=GPT");
			break;


		case PARTITION_STYLE_RAW:

			LOG_INFO(L"PartitionStyle=RAW");
			break;


		default:

			LOG_WARNING(L"PartitionStyle=UNKNOWN");
			break;

		}

		wchar_t countMsg[128];
		swprintf_s(countMsg, L"PartitionCount=%lu", layout->PartitionCount);
		LOG_INFO(countMsg);


		if (layout->PartitionStyle == PARTITION_STYLE_GPT)
		{
			disk.IsGPT = true;
			LOG_INFO(L"GPT disk detected");

			//
			// 这里先使用默认值
			// 后面由GPT Header覆盖
			//
			disk.GPT.PrimaryHeaderOffset = disk.BytesPerSector;
			disk.GPT.BackupHeaderOffset = disk.Size - disk.BytesPerSector;
			disk.GPT.PartitionEntryOffset = disk.BytesPerSector * 2;
			disk.GPT.PartitionEntryCount = layout->Gpt.MaxPartitionCount;
			disk.GPT.PartitionEntrySize = 128;
		}
		else if (layout->PartitionStyle == PARTITION_STYLE_MBR)
		{

			disk.IsMBR = true;
			disk.MBR.Signature = layout->Mbr.Signature;
			LOG_INFO(L"MBR disk detected");

		}

		for (DWORD i = 0; i < layout->PartitionCount; i++)
		{

			PARTITION_INFORMATION_EX& p = layout->PartitionEntry[i];
			wchar_t partMsg[256];
			swprintf_s(
				partMsg,
				L"Partition[%lu] Style=%d Offset=%lld Length=%lld",
				i,
				p.PartitionStyle,
				p.StartingOffset.QuadPart,
				p.PartitionLength.QuadPart
			);
			LOG_INFO(partMsg);

			if (p.PartitionLength.QuadPart <= 0)
			{
				continue;
			}




			PartitionInfo info;


			info.Index = i;
			info.Offset = p.StartingOffset.QuadPart;
			info.Size = p.PartitionLength.QuadPart;
			disk.Partitions.push_back(info);
		}

		LOG_INFO(L"QueryLayout success");
		return true;
	}









	//
	// 查询存储设备信息
	//
	bool DiskScanner::QueryStorageInfo(HANDLE hDisk, DiskInfo& disk)
	{

		STORAGE_PROPERTY_QUERY query{};


		query.PropertyId = StorageDeviceProperty;
		query.QueryType = PropertyStandardQuery;

		BYTE buffer[4096] = { 0 };
		DWORD returned = 0;

		if (!DeviceIoControl(
			hDisk,
			IOCTL_STORAGE_QUERY_PROPERTY,
			&query,
			sizeof(query),
			buffer,
			sizeof(buffer),
			&returned,
			nullptr))
		{
			return false;
		}

		STORAGE_DEVICE_DESCRIPTOR* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buffer);

		auto readString = [&](DWORD offset)->std::wstring
			{

				if (offset == 0)
					return L"";


				char* p = reinterpret_cast<char*>(buffer + offset);


				int len =
					MultiByteToWideChar(
						CP_ACP,
						0,
						p,
						-1,
						nullptr,
						0
					);


				if (len <= 0)
					return L"";


				std::wstring result(len, 0);


				MultiByteToWideChar(
					CP_ACP,
					0,
					p,
					-1,
					result.data(),
					len
				);


				result.resize(wcslen(result.c_str()));


				return result;
			};

		disk.Vendor = readString(desc->VendorIdOffset);
		disk.Model = readString(desc->ProductIdOffset);
		disk.SerialNumber = readString(desc->SerialNumberOffset);
		return true;

	}








	//
	// 盘符查询
	//
	// 当前留空
	//
	// 下一步实现
	//
	void DiskScanner::QueryDriveLetter(int devno, PartitionInfo& partition)
	{

	}







	//
	// 计算未分配空间
	//
	void DiskScanner::CalculateUnallocated(DiskInfo& disk)
	{

		disk.Unallocated.clear();
		std::sort(
			disk.Partitions.begin(),
			disk.Partitions.end(),
			[](const PartitionInfo& a,
				const PartitionInfo& b)
			{
				return a.Offset < b.Offset;
			}
		);



		uint64_t current = 0;



		for (auto& p : disk.Partitions)
		{

			if (p.Offset > current)
			{
				UnallocatedRange range;
				range.Offset = current;
				range.Size = p.Offset - current;
				disk.Unallocated.push_back(range);

			}
			current = p.Offset + p.Size;

		}



		if (current < disk.Size)
		{
			UnallocatedRange range;
			range.Offset = current;
			range.Size = disk.Size - current;
			disk.Unallocated.push_back(range);

		}

	}


	bool DiskScanner::ReadGPTHeader(HANDLE hDisk, DiskInfo& disk)
	{

		if (!disk.IsGPT)
		{
			return false;
		}

		if (disk.BytesPerSector == 0)
		{
			return false;
		}

		std::vector<BYTE> buffer(disk.BytesPerSector);

		LARGE_INTEGER offset;


		//
		// GPT Header:
		//
		// LBA1
		//
		offset.QuadPart = disk.BytesPerSector;



		if (!SetFilePointerEx(
			hDisk,
			offset,
			nullptr,
			FILE_BEGIN))
		{
			return false;
		}


		DWORD readSize = 0;
		if (!ReadFile(
			hDisk,
			buffer.data(),
			disk.BytesPerSector,
			&readSize,
			nullptr))
		{
			return false;
		}



		if (readSize < sizeof(GPT_HEADER))
		{
			return false;
		}




		GPT_HEADER* header = reinterpret_cast<GPT_HEADER*>(buffer.data());



		//
		// 校验签名
		//
		if (memcmp(header->Signature, "EFI PART", 8) != 0)
		{
			LOG_ERROR(L"Invalid GPT signature");
			return false;
		}



		//
		// 保存GPT布局
		//

		disk.GPT.PrimaryHeaderOffset = header->CurrentLBA * disk.BytesPerSector;
		disk.GPT.BackupHeaderOffset = header->BackupLBA * disk.BytesPerSector;
		disk.GPT.PartitionEntryOffset = header->PartitionEntryLBA * disk.BytesPerSector;
		disk.GPT.PartitionEntryCount = header->NumberOfPartitionEntries;
		disk.GPT.PartitionEntrySize = header->SizeOfPartitionEntry;
		return true;
	}


}