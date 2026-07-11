#include <windows.h>

#include "Logger.h"
#include "PrivilegeManager.h"
#include "DiskScanner.h"
using namespace BackupCommon;
using namespace BackupSecurity;
using namespace BackupSystem;



//
// 打印帮助
//
void PrintHelp()
{

	wprintf(
		L"\n"
		L"Usage:\n"
		L"  client.exe query_disks\n"
		L"  client.exe query_disk <devno>\n"
		L"\n"
	);
}


// 打印磁盘大小
void PrintSize(uint64_t size)
{
	double gb = (double)size / (1024.0 * 1024.0 * 1024.0);
	wprintf(L"%llu bytes (%.2f GB)", size, gb);
}

// 打印磁盘信息
void PrintDiskInfo(const DiskInfo& disk)
{

	wprintf(L"\n====================================\n");
	wprintf(L"Disk       : %d\n", disk.DevNo);
	wprintf(L"Path       : %s\n", disk.PhysicalPath.c_str());
	wprintf(L"Size       : ", disk.Size);

	PrintSize(disk.Size);
	wprintf(L"\n");
	wprintf(L"SectorSize : %u\n", disk.BytesPerSector);
	wprintf(L"Vendor     : %s\n", disk.Vendor.c_str());
	wprintf(L"Model      : %s\n", disk.Model.c_str());
	wprintf(L"Serial     : %s\n", disk.SerialNumber.c_str());

	if (disk.IsGPT)
	{
		wprintf(L"Partition Style : GPT\n");
		wprintf(L"GPT Header      : %llu\n", disk.GPT.PrimaryHeaderOffset);
		wprintf(L"GPT Backup      : %llu\n", disk.GPT.BackupHeaderOffset);
		wprintf(L"GPT Array       : %llu\n", disk.GPT.PartitionEntryOffset);
	}
	else if (disk.IsMBR)
	{
		wprintf(L"Partition Style : MBR\n");
		wprintf(L"MBR Signature   : %08X\n", disk.MBR.Signature);
	}

	wprintf(L"\nPartitions:\n");


	for (auto& p : disk.Partitions)
	{
		wprintf(L"------------------------------------\n");
		wprintf(L"Index  : %d\n", p.Index);
		wprintf(L"Offset : %llu\n", p.Offset);
		wprintf(L"Size   : %llu\n", p.Size);

		if (p.HasDriveLetter)
		{
			wprintf(L"Drive  : %s\n", p.DriveLetter.c_str());
		}
		else
		{
			wprintf(L"Drive  : None\n");
		}

	}

	wprintf(L"\nUnallocated:\n");


	for (auto& r : disk.Unallocated)
	{
		wprintf(L"Offset=%llu Size=%llu\n", r.Offset, r.Size);
	}
	wprintf(L"====================================\n");

}

int wmain(int argc, wchar_t* argv[])
{
	Logger::Instance().Initialize(L"backup.log", true);



	LOG_INFO(L"Backup System Test Start");
	if (!PrivilegeManager::IsAdministrator())
	{
		LOG_ERROR(L"Administrator privilege missing");
		PrivilegeManager::ShowPrivilegeError();
		return -1;
	}



	if (argc < 2)
	{
		PrintHelp();
		return 0;
	}

	DiskScanner scanner;
	//
   // 查询所有磁盘
   //
	if (wcscmp(argv[1], L"query_disks") == 0)
	{

		std::vector<DiskInfo> disks;
		if (!scanner.EnumerateAll(disks))
		{
			wprintf(L"Query disks failed\n");
			return -1;
		}

		for (auto& disk : disks)
		{
			PrintDiskInfo(disk);
		}

	}

	else if (wcscmp(argv[1], L"query_disk") == 0)
	{
		//
		// 查询指定磁盘
		//

		if (argc < 3)
		{
			PrintHelp();
			return -1;
		}

		int devno = _wtoi(argv[2]);
		DiskInfo disk;

		if (!scanner.Enumerate(devno, disk))
		{
			wprintf(L"Query disk %d failed\n", devno);
			return -1;
		}
		PrintDiskInfo(disk);
	}
	else
	{

		PrintHelp();
	}



	Logger::Instance().Shutdown();
	return 0;
}