#pragma once
#include <windows.h>
#include <winioctl.h>
#include <cstdint>
#include <string>
#include <vector>
#include "DiskParser.h"


namespace VolumeMapping
{

//
// 卷基本信息
// 对应一个 Windows 卷设备（\\?\Volume{GUID}）
//
struct VolumeInfo
{
	std::wstring VolumeGuid;     // \\?\Volume{GUID}\  卷 GUID 路径
	std::wstring DriveLetter;    // "C:" 或空（未分配盘符）
	int DiskNumber;              // PhysicalDrive 编号（-1 表示未知）
	uint64_t DiskOffset;         // 在物理磁盘上的起始偏移（字节）
	uint64_t Size;               // 卷大小（字节）
};

//
// 已匹配的分区信息
// 将 DiskParser 输出的分区与 Windows 卷设备关联
//
struct MappedPartition
{
	Disk::PartitionInfo Partition;   // 来自 DiskParser 的分区结构（含四分类标记）
	std::wstring VolumeGuid;         // 对应的卷 GUID 路径
	std::wstring DriveLetter;        // 盘符（"C:" 等），未分配时为空
};

//
// VolumeMapper：卷映射模块
//
// 功能：
// 1. 枚举系统中所有 Windows 卷设备（FindFirstVolume / FindNextVolume）
// 2. 查询每个卷的物理磁盘扩展信息（IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS）
// 3. 查询每个卷的盘符（GetVolumePathNamesForVolumeName）
// 4. 将卷与 DiskParser 输出的分区列表按偏移匹配
//
// 为 VSS 快照和备份引擎提供分区 → 卷设备路径的映射
//
class VolumeMapper
{

public:

	VolumeMapper();
	~VolumeMapper();

	//
	// 执行卷映射：枚举卷 → 匹配分区
	// layout: DiskParser::Parse 输出的磁盘布局
	// 返回 true 表示枚举成功（即使部分卷匹配失败）
	//
	bool Map(const Disk::DiskLayout& layout);

	//
	// 获取原始卷信息列表（所有被枚举到的卷）
	//
	const std::vector<VolumeInfo>& GetVolumes() const { return m_volumes; }

	//
	// 获取已匹配的分区列表（包含卷 GUID 和盘符）
	// 未匹配到卷的分区不在此列表中
	//
	const std::vector<MappedPartition>& GetMappedPartitions() const { return m_mapped; }

	//
	// 按分区偏移查找对应的卷 GUID
	// offset: 分区在物理磁盘上的起始偏移（字节）
	// 返回空字符串表示未找到
	//
	std::wstring FindVolumeGuid(uint64_t offset) const;

private:

	//
	// 枚举所有卷 GUID 路径（FindFirstVolumeW / FindNextVolumeW）
	//
	static std::vector<std::wstring> EnumerateVolumes();

	//
	// 查询卷的物理磁盘扩展信息
	// volumeGuid: \\?\Volume{GUID}\  格式的卷路径
	// info: [输出] 卷的磁盘扩展信息
	//
	bool GetVolumeDiskExtents(const std::wstring& volumeGuid, VolumeInfo& info);

	//
	// 查询卷的盘符
	// volumeGuid: \\?\Volume{GUID}\  格式的卷路径
	// 返回 "C:" 格式的盘符，无盘符时返回空字符串
	//
	static std::wstring GetDriveLetter(const std::wstring& volumeGuid);

	std::vector<VolumeInfo> m_volumes;
	std::vector<MappedPartition> m_mapped;

};


} // namespace VolumeMapping
