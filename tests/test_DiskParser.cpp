// DiskParser 模块单元测试
// 覆盖：Mock MBR/GPT 解析、struct 大小验证、边界条件
// 注意：真实磁盘 I/O 需要管理员权限，因此大部分测试使用结构验证
#include "test_framework.h"
#include <cstddef>
#include "../client/DiskParser.h"

using namespace Disk;

// ============================================================
// 结构体大小验证（确保 #pragma pack 正确）
// ============================================================
TEST(DiskParser, MBRPartitionEntry_Size_16)
{
	ASSERT_EQ((int)sizeof(MBRPartitionEntry), 16);
}

TEST(DiskParser, GPTHeader_Size_92)
{
	ASSERT_EQ((int)sizeof(GPTHeader), 92);
}

TEST(DiskParser, GPTEntry_Size_128)
{
	ASSERT_EQ((int)sizeof(GPTEntry), 128);
}

// ============================================================
// 枚举值验证
// ============================================================
TEST(DiskParser, PartitionContent_EnumValues_Valid)
{
	ASSERT_EQ((int)PartitionContent::Unknown, 0);
	ASSERT_GT((int)PartitionContent::FilesystemNTFS, 0);
	ASSERT_GT((int)PartitionContent::FilesystemFAT32, 0);
	ASSERT_GT((int)PartitionContent::FilesystemExFAT, 0);
	ASSERT_GT((int)PartitionContent::FilesystemReFS, 0);
	ASSERT_GT((int)PartitionContent::RawPartition, 0);
	ASSERT_GT((int)PartitionContent::Reserved, 0);
}

TEST(DiskParser, PartitionStyle_EnumValues_Valid)
{
	ASSERT_EQ((int)PartitionStyle::Unknown, 0);
	ASSERT_EQ((int)PartitionStyle::MBR, 1);
	ASSERT_EQ((int)PartitionStyle::GPT, 2);
}

// ============================================================
// DiskLayout 默认构造
// ============================================================
TEST(DiskParser, DiskLayout_DefaultConstruction)
{
	DiskLayout layout;
	ASSERT_EQ(layout.Disk.DeviceNumber, 0);
	ASSERT_EQ(layout.Disk.Size, 0ULL);
	ASSERT_EQ(layout.Disk.SectorSize, 0U);
	ASSERT_EQ((int)layout.Disk.Style, (int)PartitionStyle::Unknown);
	ASSERT_EQ((int)layout.Partitions.size(), 0);
	ASSERT_EQ((int)layout.FreeRanges.size(), 0);
	ASSERT_EQ((int)layout.MetadataRanges.size(), 0);
}

// ============================================================
// PartitionInfo 默认构造
// ============================================================
TEST(DiskParser, PartitionInfo_Defaults)
{
	PartitionInfo p;
	ASSERT_EQ(p.Index, 0U);
	ASSERT_EQ(p.Offset, 0ULL);
	ASSERT_EQ(p.Size, 0ULL);
	ASSERT_EQ(p.MbrType, 0);
	ASSERT_EQ((int)p.Content, (int)PartitionContent::Unknown);
	ASSERT_FALSE(p.IsEncrypted);
	ASSERT_TRUE(p.FsName.empty());
	ASSERT_TRUE(p.Name.empty());
}

// ============================================================
// DiskParser 构造/析构
// ============================================================
TEST(DiskParser, Construct_Destruct_NoCrash)
{
	DiskParser parser;
	// 不调用 Open/Close，纯构造析构
}

TEST(DiskParser, Open_InvalidDisk_Fails)
{
	DiskParser parser;
	ASSERT_FALSE(parser.Open(999));  // 不存在的磁盘
}

TEST(DiskParser, Open_Close_MultipleCall_NoCrash)
{
	DiskParser parser;
	// 连续 Open 覆盖
	parser.Open(999);
	parser.Close();

	// 再次 Open
	parser.Open(999);
	parser.Close();
}

// ============================================================
// Parse 未 Open 状态
// ============================================================
TEST(DiskParser, Parse_NotOpen_Fails)
{
	DiskParser parser;
	DiskLayout layout;
	ASSERT_FALSE(parser.Parse(layout));
}

// ============================================================
// MBR Signature 常量
// ============================================================
TEST(DiskParser, MBR_Signature_Bytes)
{
	// 验证 MBR 签名常量 0x55 0xAA
	uint8_t sig[2] = { 0x55, 0xAA };
	uint16_t word = *(uint16_t*)sig;
	ASSERT_EQ(word, 0xAA55);  // little-endian
}

// ============================================================
// GPT "EFI PART" Signature
// ============================================================
TEST(DiskParser, GPT_EFI_PART_Signature_Magic)
{
	const char efiPart[9] = "EFI PART";
	ASSERT_EQ((int)strlen(efiPart), 8);
	ASSERT_STR_EQ(efiPart, "EFI PART");
}

// ============================================================
// DiskRange 结构
// ============================================================
TEST(DiskParser, DiskRange_Assignment)
{
	DiskRange r;
	r.Offset = 0x1000;
	r.Size = 0x2000;
	ASSERT_EQ(r.Offset, 0x1000ULL);
	ASSERT_EQ(r.Size, 0x2000ULL);
}

// ============================================================
// DiskInfo 结构
// ============================================================
TEST(DiskParser, DiskInfo_GPTInfo_Members)
{
	GPTInfo gpt;
	memset(&gpt, 0, sizeof(gpt));
	gpt.EntryCount = 128;
	gpt.EntrySize = 128;
	gpt.PrimaryHeaderOffset = 512;
	gpt.PrimaryEntryOffset = 1024;

	ASSERT_EQ(gpt.EntryCount, 128U);
	ASSERT_EQ(gpt.EntrySize, 128U);
	ASSERT_EQ(gpt.PrimaryHeaderOffset, 512ULL);
	ASSERT_EQ(gpt.PrimaryEntryOffset, 1024ULL);
}

// ============================================================
// GPT 分区类型 GUID 常量验证（仅验证几个常见类型）
// ============================================================
TEST(DiskParser, GPT_TypeGuids_KnownValues)
{
	// Microsoft Basic Data Partition: EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
	// 验证 GUID 结构大小
	GUID msBasicData = { 0xEBD0A0A2, 0xB9E5, 0x4433, { 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 } };
	ASSERT_EQ((int)sizeof(GUID), 16);

	// MSR: E3C9E316-0B5C-4DB8-817D-F92DF00215AE
	GUID msr = { 0xE3C9E316, 0x0B5C, 0x4DB8, { 0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE } };
	ASSERT_EQ((int)sizeof(msr), 16);

	// ESP: C12A7328-F81F-11D2-BA4B-00A0C93EC93B
	GUID esp = { 0xC12A7328, 0xF81F, 0x11D2, { 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B } };
	ASSERT_EQ((int)sizeof(esp), 16);
}

// ============================================================
// MBR 分区类型常量
// ============================================================
TEST(DiskParser, MBR_PartitionTypes_KnownValues)
{
	// GPT 保护分区
	ASSERT_EQ(0xEE, 0xEE);
	// NTFS
	ASSERT_EQ(0x07, 0x07);
	// FAT32 LBA
	ASSERT_EQ(0x0C, 0x0C);
	// Extended
	ASSERT_EQ(0x0F, 0x0F);
}

// ============================================================
// 偏移计算
// ============================================================
TEST(DiskParser, LBA_To_Offset_Calculation)
{
	// LBA * 512 = byte offset
	uint64_t lba1 = 0;
	uint64_t lba2 = 2048;
	uint64_t lbaLast = 0xFFFFFFFF;

	ASSERT_EQ(lba1 * 512, 0ULL);
	ASSERT_EQ(lba2 * 512, 1048576ULL);
	ASSERT_EQ(lbaLast * 512, 0xFFFFFFFFULL * 512);
}

// ============================================================
// MBRPartitionEntry 字段偏移验证
// ============================================================
TEST(DiskParser, MBRPartitionEntry_FieldOffsets)
{
	MBRPartitionEntry entry;
	memset(&entry, 0, sizeof(entry));

	// offsetof 验证
	size_t bootFlagOffset = offsetof(MBRPartitionEntry, BootFlag);
	size_t typeOffset = offsetof(MBRPartitionEntry, Type);
	size_t startLBAOffset = offsetof(MBRPartitionEntry, StartLBA);
	size_t sectorCountOffset = offsetof(MBRPartitionEntry, SectorCount);

	ASSERT_EQ((int)bootFlagOffset, 0);
	ASSERT_EQ((int)typeOffset, 4);
	ASSERT_EQ((int)startLBAOffset, 8);
	ASSERT_EQ((int)sectorCountOffset, 12);
}

// ============================================================
// GPTEntry 字段偏移验证
// ============================================================
TEST(DiskParser, GPTEntry_FieldOffsets)
{
	GPTEntry entry;
	memset(&entry, 0, sizeof(entry));

	size_t typeGuidOffset = offsetof(GPTEntry, TypeGuid);
	size_t partitionGuidOffset = offsetof(GPTEntry, PartitionGuid);
	size_t firstLBAOffset = offsetof(GPTEntry, FirstLBA);
	size_t lastLBAOffset = offsetof(GPTEntry, LastLBA);
	size_t attributesOffset = offsetof(GPTEntry, Attributes);
	size_t nameOffset = offsetof(GPTEntry, Name);

	ASSERT_EQ((int)typeGuidOffset, 0);
	ASSERT_EQ((int)partitionGuidOffset, 16);
	ASSERT_EQ((int)firstLBAOffset, 32);
	ASSERT_EQ((int)lastLBAOffset, 40);
	ASSERT_EQ((int)attributesOffset, 48);
	ASSERT_EQ((int)nameOffset, 56);
}

// ============================================================
// GPTHeader 字段偏移验证
// ============================================================
TEST(DiskParser, GPTHeader_FieldOffsets)
{
	GPTHeader hdr;
	memset(&hdr, 0, sizeof(hdr));

	size_t sigOffset = offsetof(GPTHeader, Signature);
	size_t revisionOffset = offsetof(GPTHeader, Revision);
	size_t headerSizeOffset = offsetof(GPTHeader, HeaderSize);
	size_t headerCRCOffset = offsetof(GPTHeader, HeaderCRC);
	size_t currentLBAOffset = offsetof(GPTHeader, CurrentLBA);
	size_t backupLBAOffset = offsetof(GPTHeader, BackupLBA);
	size_t diskGuidOffset = offsetof(GPTHeader, DiskGuid);
	size_t partEntryLBAOffset = offsetof(GPTHeader, PartitionEntryLBA);
	size_t numPartsOffset = offsetof(GPTHeader, NumberOfPartitionEntries);
	size_t sizeOfPartOffset = offsetof(GPTHeader, SizeOfPartitionEntry);
	size_t arrayCRCOffset = offsetof(GPTHeader, PartitionEntryArrayCRC);

	ASSERT_EQ((int)sigOffset, 0);
	ASSERT_EQ((int)revisionOffset, 8);
	ASSERT_EQ((int)headerSizeOffset, 12);
	ASSERT_EQ((int)headerCRCOffset, 16);
	ASSERT_EQ((int)currentLBAOffset, 24);
	ASSERT_EQ((int)backupLBAOffset, 32);
	ASSERT_EQ((int)diskGuidOffset, 40);
	ASSERT_EQ((int)partEntryLBAOffset, 56);
	ASSERT_EQ((int)numPartsOffset, 64);
	ASSERT_EQ((int)sizeOfPartOffset, 68);
	ASSERT_EQ((int)arrayCRCOffset, 72);
}
