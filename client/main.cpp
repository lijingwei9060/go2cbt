#include <windows.h>
#include <stdio.h>

#include "Logger.h"
#include "PrivilegeManager.h"
#include "DiskScanner.h"
#include "DiskParser.h"
#include "VolumeMapper.h"
#include "BackupEngine.h"
#include "VssManager.h"

using namespace BackupCommon;
using namespace BackupSecurity;
using namespace BackupSystem;
using Disk::DiskParser;
using Disk::DiskLayout;
using Disk::PartitionInfo;
using Disk::PartitionContent;
using VolumeMapping::VolumeMapper;
using VolumeMapping::MappedPartition;


//
// 将分区内容分类转换为可读字符串
//
const wchar_t* PartitionContentToStr(PartitionContent content)
{
	switch (content)
	{
	case PartitionContent::Unknown:          return L"Unknown";
	case PartitionContent::FilesystemNTFS:   return L"NTFS";
	case PartitionContent::FilesystemFAT32:  return L"FAT32";
	case PartitionContent::FilesystemExFAT:  return L"exFAT";
	case PartitionContent::FilesystemReFS:   return L"ReFS";
	case PartitionContent::RawPartition:     return L"Raw";
	case PartitionContent::Reserved:         return L"Reserved";
	default:                                 return L"???";
	}
}

//
// 获取分区对应的备份策略说明
//
const wchar_t* GetBackupStrategy(PartitionContent content)
{
	switch (content)
	{
	case PartitionContent::FilesystemNTFS:
	case PartitionContent::FilesystemFAT32:
	case PartitionContent::FilesystemExFAT:
	case PartitionContent::FilesystemReFS:
		return L"VSS Snapshot";
	case PartitionContent::RawPartition:
		return L"Physical Read";
	case PartitionContent::Reserved:
		return L"Metadata Backup";
	default:
		return L"Skip";
	}
}


//
// 打印文件大小（人类可读格式）
//
void PrintSize(uint64_t size)
{
	double gb = (double)size / (1024.0 * 1024.0 * 1024.0);
	if (gb >= 1.0)
	{
		wprintf(L"%.2f GB (%llu bytes)", gb, size);
	}
	else
	{
		double mb = (double)size / (1024.0 * 1024.0);
		wprintf(L"%.2f MB (%llu bytes)", mb, size);
	}
}


//
// 从 DiskParser + VolumeMapper 打印详细磁盘布局（query_disk 使用）
//
void PrintDiskLayout(int devno)
{
	// ===== Step 1: 使用 DiskParser 解析磁盘分区布局 =====
	DiskParser parser;
	if (!parser.Open(devno))
	{
		wprintf(L"[ERROR] Cannot open PhysicalDrive%d\n", devno);
		return;
	}

	DiskLayout layout;
	if (!parser.Parse(layout))
	{
		wprintf(L"[ERROR] DiskParser::Parse failed for PhysicalDrive%d\n", devno);
		parser.Close();
		return;
	}
	parser.Close();

	// ===== 磁盘基本信息 =====
	wprintf(L"\n");
	wprintf(L"===============================================================================\n");
	wprintf(L"  DISK LAYOUT REPORT - PhysicalDrive%d\n", devno);
	wprintf(L"===============================================================================\n");
	wprintf(L"\n");
	wprintf(L"  [Disk Info]\n");
	wprintf(L"    Device Number : %d\n", layout.Disk.DeviceNumber);
	wprintf(L"    Sector Size   : %u bytes\n", layout.Disk.SectorSize);
	wprintf(L"    Total Size    : ");
	PrintSize(layout.Disk.Size);
	wprintf(L"\n");
	wprintf(L"    Partition Style: %s\n",
		layout.Disk.Style == Disk::PartitionStyle::GPT ? L"GPT" :
		layout.Disk.Style == Disk::PartitionStyle::MBR ? L"MBR" : L"Unknown");

	if (layout.Disk.Style == Disk::PartitionStyle::GPT)
	{
		wprintf(L"    GPT Disk GUID : {%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}\n",
			layout.Disk.GPT.DiskGuid.Data1,
			layout.Disk.GPT.DiskGuid.Data2,
			layout.Disk.GPT.DiskGuid.Data3,
			layout.Disk.GPT.DiskGuid.Data4[0], layout.Disk.GPT.DiskGuid.Data4[1],
			layout.Disk.GPT.DiskGuid.Data4[2], layout.Disk.GPT.DiskGuid.Data4[3],
			layout.Disk.GPT.DiskGuid.Data4[4], layout.Disk.GPT.DiskGuid.Data4[5],
			layout.Disk.GPT.DiskGuid.Data4[6], layout.Disk.GPT.DiskGuid.Data4[7]);
	}
	if (layout.Disk.Style == Disk::PartitionStyle::MBR)
	{
		wprintf(L"    MBR Signature  : 0x%08X\n", layout.Disk.MBR.Signature);
	}

	// ===== 磁盘元数据区域 =====
	if (!layout.MetadataRanges.empty())
	{
		wprintf(L"\n  [Metadata Regions] (%zu region(s))\n", layout.MetadataRanges.size());
		for (size_t i = 0; i < layout.MetadataRanges.size(); i++)
		{
			const auto& m = layout.MetadataRanges[i];
			wprintf(L"    [%zu] Offset=0x%010llx  Size=", i, m.Offset);
			PrintSize(m.Size);
			wprintf(L"\n");
		}
	}

	// ===== 分区列表（含四分类） =====
	if (!layout.Partitions.empty())
	{
		wprintf(L"\n  [Partitions] (%zu partition(s))\n", layout.Partitions.size());
		wprintf(L"  %-6s %-14s %-14s %-12s %-8s %-s\n",
			L"Index", L"Offset", L"Size", L"Content", L"Encrypt", L"Backup Strategy / FS Name");
		wprintf(L"  %-6s %-14s %-14s %-12s %-8s %-s\n",
			L"-----", L"--------------", L"--------------", L"------------", L"--------", L"--------------------------");

		for (const auto& p : layout.Partitions)
		{
			wprintf(L"  %-6u 0x%010llx  ", p.Index, p.Offset);
			wchar_t sizeStr[64];
			double gb = (double)p.Size / (1024.0 * 1024.0 * 1024.0);
			if (gb >= 1.0)
				swprintf_s(sizeStr, L"%.2f GB", gb);
			else
				swprintf_s(sizeStr, L"%llu MB", p.Size / (1024 * 1024));
			wprintf(L"%-14s ", sizeStr);

			wprintf(L"%-12s ", PartitionContentToStr(p.Content));
			wprintf(L"%-8s ", p.IsEncrypted ? L"YES" : L"no");

			// 备份策略 + 文件系统名
			if (p.Content == PartitionContent::FilesystemNTFS ||
				p.Content == PartitionContent::FilesystemFAT32 ||
				p.Content == PartitionContent::FilesystemExFAT ||
				p.Content == PartitionContent::FilesystemReFS)
			{
				wprintf(L"VSS  [%ls]", p.FsName.c_str());
			}
			else if (p.Content == PartitionContent::RawPartition)
			{
				wprintf(L"RAW  [%ls]", p.FsName.c_str());
			}
			else if (p.Content == PartitionContent::Reserved)
			{
				wprintf(L"SKIP [%ls]", p.FsName.c_str());
			}
			else
			{
				wprintf(L"???");
			}

			// 分区名（GPT）
			if (!p.Name.empty())
			{
				wprintf(L"  \"%ls\"", p.Name.c_str());
			}

			wprintf(L"\n");
		}
	}

	// ===== 未分配空间 =====
	if (!layout.FreeRanges.empty())
	{
		wprintf(L"\n  [Unallocated Space] (%zu range(s)) - will NOT be backed up\n", layout.FreeRanges.size());
		for (size_t i = 0; i < layout.FreeRanges.size(); i++)
		{
			const auto& f = layout.FreeRanges[i];
			wprintf(L"    [%zu] Offset=0x%010llx  Size=", i, f.Offset);
			PrintSize(f.Size);
			wprintf(L"\n");
		}
	}

	// ===== Step 2: 使用 VolumeMapper 映射卷设备 =====
	wprintf(L"\n  [Volume Mapping]\n");
	VolumeMapper mapper;
	if (mapper.Map(layout))
	{
		const auto& mapped = mapper.GetMappedPartitions();
		wprintf(L"    %-6s %-14s %-8s %-s\n",
			L"Part#", L"Offset", L"Drive", L"Volume GUID");
		wprintf(L"    %-6s %-14s %-8s %-s\n",
			L"-----", L"--------------", L"--------", L"------------------------------------");

		for (const auto& mp : mapped)
		{
			wprintf(L"    %-6u 0x%010llx  %-8s %s\n",
				mp.Partition.Index,
				mp.Partition.Offset,
				mp.DriveLetter.empty() ? L"(none)" : mp.DriveLetter.c_str(),
				mp.VolumeGuid.c_str());
		}
	}
	else
	{
		wprintf(L"    No volume mappings found (admin privileges required)\n");
	}

	// ===== 备份摘要 =====
	int fsCount = 0, rawCount = 0, reservedCount = 0, unknownCount = 0;
	uint64_t fsBytes = 0, rawBytes = 0;
	for (const auto& p : layout.Partitions)
	{
		switch (p.Content)
		{
		case PartitionContent::FilesystemNTFS:
		case PartitionContent::FilesystemFAT32:
		case PartitionContent::FilesystemExFAT:
		case PartitionContent::FilesystemReFS:
			fsCount++; fsBytes += p.Size; break;
		case PartitionContent::RawPartition:
			rawCount++; rawBytes += p.Size; break;
		case PartitionContent::Reserved:
			reservedCount++; break;
		default:
			unknownCount++; break;
		}
	}

	wprintf(L"\n  ===============================================================================\n");
	wprintf(L"  BACKUP SUMMARY\n");
	wprintf(L"  ===============================================================================\n");
	wprintf(L"    Filesystem partitions : %2d  (VSS backup)\n", fsCount);
	wprintf(L"    Raw / bare partitions : %2d  (physical read)\n", rawCount);
	wprintf(L"    Reserved partitions   : %2d  (metadata backup)\n", reservedCount);
	wprintf(L"    Unknown / unreadable  : %2d\n", unknownCount);
	wprintf(L"    Unallocated ranges    : %2zu  (skip)\n", layout.FreeRanges.size());
	wprintf(L"\n");
	wprintf(L"    Filesystem data size  : ");
	PrintSize(fsBytes);
	wprintf(L"\n");
	wprintf(L"    Raw partition size    : ");
	PrintSize(rawBytes);
	wprintf(L"\n");
	wprintf(L"    Metadata size         : ~%llu KB\n",
		(uint64_t)layout.MetadataRanges.size() * layout.Disk.SectorSize / 1024);
}


//
// 快速列举所有磁盘（query_disks 使用 DiskScanner）
//
void PrintDiskInfoBrief(const DiskInfo& disk)
{
	wprintf(L"  Disk %-3d  ", disk.DevNo);
	PrintSize(disk.Size);
	wprintf(L"  %s  %s  %-20s",
		disk.IsGPT ? L"GPT" : (disk.IsMBR ? L"MBR" : L"RAW"),
		disk.Vendor.c_str(),
		disk.Model.c_str());
	if (!disk.SerialNumber.empty())
	{
		wprintf(L"  S/N:%s", disk.SerialNumber.c_str());
	}
	wprintf(L"\n");
}


//
// 打印帮助
//
void PrintHelp()
{
	wprintf(L"\n");
	wprintf(L"Usage:\n");
	wprintf(L"  client.exe query_disks              Enumerate all physical disks (fast scan)\n");
	wprintf(L"  client.exe query_disk  <devno>      Detailed disk layout + volume mapping\n");
	wprintf(L"  client.exe backup <all|devno>       Backup disk(s) to server\n");
	wprintf(L"  client.exe cleanup-shadows          Delete orphaned VSS snapshots (admin)\n");
	wprintf(L"         --cbt        Use CBT incremental tracking\n");
	wprintf(L"         --serverip   Server IP address\n");
	wprintf(L"         --port       Server port\n");
	wprintf(L"         --dryrun     Simulated mode (no server)\n");
	wprintf(L"         --retry N    Retry count (default 3)\n");
	wprintf(L"         --state-dir  State file directory\n");
	wprintf(L"\n");
	wprintf(L"Examples:\n");
	wprintf(L"  client.exe query_disks\n");
	wprintf(L"  client.exe query_disk  0\n");
	wprintf(L"  client.exe backup 0 --serverip 192.168.1.100 --port 9000\n");
	wprintf(L"  client.exe backup all --cbt --dryrun\n");
	wprintf(L"\n");
}


int wmain(int argc, wchar_t* argv[])
{
	Logger::Instance().Initialize(L"backup.log", true);

	LOG_INFO(L"Backup System Start");

	if (!PrivilegeManager::IsAdministrator())
	{
		LOG_ERROR(L"Administrator privilege required");
		PrivilegeManager::ShowPrivilegeError();
		Logger::Instance().Shutdown();
		return -1;
	}

	if (argc < 2)
	{
		PrintHelp();
		Logger::Instance().Shutdown();
		return 0;
	}

	// ============================================================
	// query_disks: 快速扫描所有物理磁盘
	// ============================================================
	if (wcscmp(argv[1], L"query_disks") == 0)
	{
		DiskScanner scanner;
		std::vector<DiskInfo> disks;

		if (!scanner.EnumerateAll(disks))
		{
			wprintf(L"[ERROR] Failed to enumerate disks\n");
			Logger::Instance().Shutdown();
			return -1;
		}

		wprintf(L"\n");
		wprintf(L"===============================================================================\n");
		wprintf(L"  DISK LIST (%zu disk(s) found)\n", disks.size());
		wprintf(L"===============================================================================\n");
		wprintf(L"  %-6s %-14s %-5s %-20s %s\n",
			L"Disk#", L"Size", L"Style", L"Model", L"Serial");
		wprintf(L"  %-6s %-14s %-5s %-20s %s\n",
			L"-----", L"--------------", L"-----", L"--------------------", L"------------------");

		for (const auto& disk : disks)
		{
			PrintDiskInfoBrief(disk);
		}
		wprintf(L"\n");
		wprintf(L"  Use 'query_disk <N>' for detailed layout of a specific disk.\n");
		wprintf(L"\n");

		Logger::Instance().Shutdown();
		return 0;
	}

	// ============================================================
	// query_disk <devno>: 详细磁盘布局（DiskParser + VolumeMapper）
	// ============================================================
	if (wcscmp(argv[1], L"query_disk") == 0)
	{
		if (argc < 3)
		{
			wprintf(L"[ERROR] Missing disk number\n");
			PrintHelp();
			Logger::Instance().Shutdown();
			return -1;
		}

		int devno = _wtoi(argv[2]);

		// 先用 DiskScanner 获取硬件信息
		DiskScanner scanner;
		DiskInfo diskInfo;
		if (!scanner.Enumerate(devno, diskInfo))
		{
			wprintf(L"[ERROR] DiskScanner::Enumerate failed for PhysicalDrive%d\n", devno);
			wprintf(L"  Make sure the disk exists and you have administrator privileges.\n");
			Logger::Instance().Shutdown();
			return -1;
		}

		// 打印硬件信息
		wprintf(L"  Vendor : %s\n", diskInfo.Vendor.c_str());
		wprintf(L"  Model  : %s\n", diskInfo.Model.c_str());
		wprintf(L"  Serial : %s\n", diskInfo.SerialNumber.c_str());

		// 详细布局分析（DiskParser + VolumeMapper）
		PrintDiskLayout(devno);

		Logger::Instance().Shutdown();
		return 0;
	}

	// ============================================================
	// 未知命令
	// ============================================================
	// ============================================================
	// backup <all|devno>: backup disk(s) to server
	// ============================================================
	if (wcscmp(argv[1], L"backup") == 0)
	{
		if (argc < 3)
		{
			wprintf(L"[ERROR] Missing target disk number\n");
			PrintHelp();
			Logger::Instance().Shutdown();
			return -1;
		}

		BackupEngine::BackupConfig config;
		std::vector<int> targets;

		// parse target
		if (wcscmp(argv[2], L"all") == 0)
		{
			for (int d = 0; d < 8; d++) targets.push_back(d);
		}
		else
		{
			targets.push_back(_wtoi(argv[2]));
		}

		// parse options
		for (int i = 3; i < argc; i++)
		{
			if (wcscmp(argv[i], L"--cbt") == 0)
				config.UseCbt = true;
			else if (wcscmp(argv[i], L"--dryrun") == 0)
				config.DryRun = true;
			else if (wcscmp(argv[i], L"--serverip") == 0 && i + 1 < argc)
			{
				i++;
				char ipBuf[64];
				WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, ipBuf, sizeof(ipBuf), nullptr, nullptr);
				config.ServerIp = ipBuf;
			}
			else if (wcscmp(argv[i], L"--port") == 0 && i + 1 < argc)
				config.Port = (uint16_t)_wtoi(argv[++i]);
			else if (wcscmp(argv[i], L"--retry") == 0 && i + 1 < argc)
				config.RetryCount = _wtoi(argv[++i]);
			else if (wcscmp(argv[i], L"--state-dir") == 0 && i + 1 < argc)
				config.StateDir = argv[++i];
		}

		// validate
		if (!config.DryRun && config.ServerIp.empty())
		{
			wprintf(L"[ERROR] --serverip required (or use --dryrun)\n");
			Logger::Instance().Shutdown();
			return -1;
		}
		if (!config.DryRun && config.Port == 0)
		{
			wprintf(L"[ERROR] --port required (or use --dryrun)\n");
			Logger::Instance().Shutdown();
			return -1;
		}

		// show config
		wprintf(L"\n========================================================\n");
		wprintf(L"  BACKUP %s%s\n",
			config.UseCbt ? L"INCREMENTAL (CBT)" : L"FULL",
			config.DryRun ? L" [DRY RUN]" : L"");
		wprintf(L"========================================================\n");
		wprintf(L"  Targets : "); for (auto d : targets) wprintf(L"Disk%d ", d); wprintf(L"\n");
		if (!config.DryRun) wprintf(L"  Server  : %hs:%hu\n", config.ServerIp.c_str(), config.Port);
		wprintf(L"  Retry   : %d\n  State   : %s\n\n", config.RetryCount, config.StateDir.c_str());

		// run backup
		BackupEngine::BackupEngine engine;
		std::vector<BackupEngine::BackupStats> stats;
		if (engine.Run(config, targets, stats))
		{
			wprintf(L"\n  BACKUP COMPLETE\n");
			for (auto& s : stats)
				wprintf(L"  Disk%d: sent=%llu acked=%llu\n", s.DevNo, s.SentBlocks, s.AckedBlocks);
		}
		else
			wprintf(L"\n  BACKUP FAILED\n");
		wprintf(L"\n");

		Logger::Instance().Shutdown();
		return 0;
	}

	// ============================================================
	// cleanup-shadows: 清理残留的 VSS 快照
	// 用于进程异常退出后，vssadmin 无法删除 Backup 类型快照的场景
	// ============================================================
	if (wcscmp(argv[1], L"cleanup-shadows") == 0)
	{
		wprintf(L"\nCleaning up orphaned VSS snapshots...\n");

		int deleted = VssSnapshot::VssManager::DeleteOrphanedSnapshots();

		if (deleted >= 0)
		{
			wprintf(L"  Deleted: %d snapshot set(s)\n\n", deleted);
		}
		else
		{
			wprintf(L"  Cleanup failed (check backup.log for details)\n\n");
			Logger::Instance().Shutdown();
			return -1;
		}

		// 验证是否清理干净
		wprintf(L"Verifying with vssadmin...\n");
		system("vssadmin list shadows");
		wprintf(L"\n");

		Logger::Instance().Shutdown();
		return 0;
	}

	wprintf(L"[ERROR] Unknown command: %s\n", argv[1]);
	PrintHelp();
	Logger::Instance().Shutdown();
	return -1;
}
