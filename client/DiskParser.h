#pragma once
#include <windows.h>
#include <stdint.h>
#include <string>
#include <vector>


namespace Disk
{
	enum class PartitionStyle
	{
		Unknown = 0,
		MBR,
		GPT
	};

	// 分区内容分类（备份系统的四分类基础）
	enum class PartitionContent
	{
		Unknown = 0,        // 未检测
		FilesystemNTFS,     // NTFS 文件系统 → VSS 快照备份
		FilesystemFAT32,    // FAT32 文件系统 → VSS 快照备份
		FilesystemExFAT,    // exFAT 文件系统 → VSS 快照备份
		FilesystemReFS,     // ReFS 文件系统 → VSS 快照备份
		RawPartition,       // 有分区但无已知文件系统 → 物理磁盘直接读取备份
		Reserved,           // 系统保留分区（MSR 等）→ 归入磁盘元数据备份
	};

	// 磁盘范围结构体
	struct DiskRange
	{
		uint64_t Offset;
		uint64_t Size;
	};

	struct MBRInfo
	{
		uint32_t Signature; //  MBR Signature, 0x55AA, 0号扇区MBR的签名
	};

	struct GPTInfo
	{
		uint64_t PrimaryHeaderOffset; // GPT Header位置
		uint64_t BackupHeaderOffset;  // Backup Header位置
		uint64_t PrimaryEntryOffset;         // Partition Entry Array
		uint64_t BackupEntryOffset;
		uint32_t EntryCount; // 分区表参数
		uint32_t EntrySize;
		GUID DiskGuid;  // Disk GUID
	};

	struct PartitionInfo
	{
		uint32_t Index;  // 分区编号
		uint64_t Offset;   // 起始偏移
		uint64_t Size;  // 大小
		uint8_t MbrType;  // MBR类型
		GUID TypeGuid; // GPT类型GUID
		GUID PartitionGuid;  // GPT分区GUID
		uint64_t Attributes;  // GPT属性
		std::wstring Name;  // GPT名称

		PartitionContent Content = PartitionContent::Unknown;  // 分区内容分类
		bool IsEncrypted = false;                               // BitLocker 等加密标记
		std::wstring FsName;                                    // 文件系统名称（"NTFS", "FAT32" 等）
	};

	//
	// 磁盘属性
	//
	struct DiskInfo
	{
		int DeviceNumber; // PhysicalDrive编号
		uint64_t Size;  // 容量
		uint32_t SectorSize;         // 扇区大小
		PartitionStyle Style;         // 分区类型
		GPTInfo GPT;         // GPT信息
		MBRInfo MBR;         // MBR信息
	};

	//
	// 最终磁盘布局
	//
	struct DiskLayout
	{
		DiskInfo Disk; // 磁盘信息
		std::vector<PartitionInfo> Partitions; // 分区列表
		std::vector<DiskRange> FreeRanges; // 未使用区域
		std::vector<DiskRange> MetadataRanges; // 元数据区域
	};

#pragma pack(push,1)

	// 引导代码 (Boot Code)	446 字节 | Offset: 0x000 ~0x1BD
	// 分区表 (Partition Table) — 64 字节 | Offset: 0x1BE ~ 0x1FD
	struct MBRPartitionEntry // 16个字节，MBR分区表项
	{
		BYTE BootFlag;     // 启动标志 0x80: 可引导, 0x00: 不可引导
		BYTE StartCHS[3];  // 起始CHS[头/扇区/柱面]
		BYTE Type;         // 类型 07: NTFS, 0B: FAT32, 0C: FAT32 LBA, 83: Linux, 82: Linux Swap, EE: GPT保护分区，MBR占位
		BYTE EndCHS[3];    // 结束CHS[头/扇区/柱面]
		DWORD StartLBA;    // uint32 LE 起始LBA
		DWORD SectorCount; // uint32 LE 扇区数
	};
	// 签名 (Signature) — 2 字节 | Offset: 0x1FE ~ 0x1FF | 固定值: 0x55 0xAA

	// MBR里面有GPT展位分区，GPT 头部 (GPT Header) — 92 字节，位于 LBA 1
	struct GPTHeader
	{
		BYTE Signature[8];             // 签名 Signature "EFI PART"
		DWORD Revision;                // 修订版 uint32 LE
		DWORD HeaderSize;              // 92, 头大小
		DWORD HeaderCRC;               // 头部CRC32
		DWORD Reserved;                // 保留
		ULONGLONG CurrentLBA;          // 当前LBA，通常为1
		ULONGLONG BackupLBA;           // 备份LBA，通常为最后一个LBA， uint64
		ULONGLONG FirstUsableLBA;      // 第一个可用LBA，通常为34
		ULONGLONG LastUsableLBA;       // 最后一个可用LBA，通常为倒数第34个LBA
		GUID DiskGuid;                 // 磁盘GUID
		ULONGLONG PartitionEntryLBA;   // 分区表起始LBA，通常为2
		DWORD NumberOfPartitionEntries;// 分区表项数，通常为128
		DWORD SizeOfPartitionEntry;    // 分区表项大小，通常为128
		DWORD PartitionEntryArrayCRC;  // 分区表CRC32
	};
	// GPT分区表项 (GPT Partition Entry) — 128 字节，位于 LBA 2 开始到LBA 33， 一共32个扇区，每个扇区4个表项，一共128个表项
	struct GPTEntry
	{
		GUID TypeGuid;                 // 分区类型GUID	16 字节 — 标识此分区的文件系统 / 用途类型
		GUID PartitionGuid;            // 分区GUID 唯一分区 GUID (Unique Partition GUID)
		ULONGLONG FirstLBA;            // 起始LBA
		ULONGLONG LastLBA;             // 结束LBA
		ULONGLONG Attributes;          // 属性
		WCHAR Name[36];                // 分区名称，UTF-16LE编码，最多36个字符
	};

	// 备份分区项阵列 (Backup Partition Entry Array)，从-33 到 -2，也是32个扇区，每个扇区4个表项，一共128个表项
	// 备份GPT头部 (Backup GPT Header) — 92 字节，位于最后一个LBA

#pragma pack(pop)

	//
// DiskParser
//
// 从物理磁盘读取MBR/GPT
// 输出DiskLayout
//
	class DiskParser
	{

	public:


		DiskParser();
		~DiskParser();

		// 打开物理磁盘
		bool Open(int deviceNumber);
		// 关闭
		void Close();
		// 分析磁盘
		bool Parse(DiskLayout& layout);

	private:

		// 查询磁盘总大小（IOCTL_DISK_GET_LENGTH_INFO）
		bool QueryDiskSize(DiskLayout& layout);

		// 查询扇区大小（IOCTL_DISK_GET_DRIVE_GEOMETRY_EX）
		// 失败时回退到默认 512 字节
		bool QuerySectorSize(DiskLayout& layout);

		HANDLE m_hDisk;
		int m_DeviceNumber;
	private:

		// 读取指定偏移
		bool Read(uint64_t offset, void* buffer, uint32_t size);
		// 解析MBR
		bool ParseMBR(DiskLayout& layout);
		// 解析GPT
		bool ParseGPT(DiskLayout& layout);

		// 计算未分配空间
		void CalculateFreeSpace(DiskLayout& layout);

		// 检测分区内容：读取 VBR 扇区，匹配文件系统签名
		void DetectPartitionContent(PartitionInfo& p, uint32_t sectorSize);

		// GPT 分区类型预分类：MSR/ESP 等系统保留分区
		void DetectPartitionContentGpt(PartitionInfo& p);
	};


}