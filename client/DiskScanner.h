#pragma once

#include <windows.h>
#include <winioctl.h>

#include <cstdint>
#include <string>
#include <vector>


//
// 磁盘扫描模块
//
// 功能：
// 1. 枚举 PhysicalDrive
// 2. 获取磁盘容量
// 3. 获取 GPT / MBR 类型
// 4. 获取分区布局
// 5. 获取分区 offset / size
// 6. 计算未分配空间
//
// 注意：
// - 只读操作
// - 不修改磁盘
// - 不支持 Dynamic Disk
// - RAID按照Windows暴露的逻辑磁盘处理
//


namespace BackupSystem
{


    //
    // GPT布局信息
    //
    // 用于后续备份恢复定位GPT区域
    //
    // 不保存GPT Header内容
    //
    struct GPTLayout
    {

        //
        // GPT主Header位置
        //
        uint64_t PrimaryHeaderOffset = 0;


        //
        // GPT备份Header位置
        //
        uint64_t BackupHeaderOffset = 0;



        //
        // GPT分区表数组位置
        //
        uint64_t PartitionEntryOffset = 0;



        //
        // 分区表数量
        //
        uint32_t PartitionEntryCount = 0;



        //
        // 每个Partition Entry大小
        //
        uint32_t PartitionEntrySize = 0;

    };




    //
    // MBR布局信息
    //
    struct MBRLayout
    {

        //
        // MBR起始位置
        //
        uint64_t Offset = 0;



        //
        // MBR Signature
        //
        uint32_t Signature = 0;

    };


    #pragma pack(push,1)

    struct GPT_HEADER
    {
        char Signature[8];

        uint32_t Revision;
        uint32_t HeaderSize;
        uint32_t HeaderCRC32;
        uint32_t Reserved;
        uint64_t CurrentLBA;
        uint64_t BackupLBA;
        uint64_t FirstUsableLBA;
        uint64_t LastUsableLBA;
        GUID DiskGUID;
        uint64_t PartitionEntryLBA;
        uint32_t NumberOfPartitionEntries;
        uint32_t SizeOfPartitionEntry;
        uint32_t PartitionEntryArrayCRC32;
    };
    #pragma pack(pop)



    //
    // 分区信息
    //
    // 只保存备份需要的信息
    //
    struct PartitionInfo
    {

        //
        // 分区序号
        //
        int Index = -1;



        //
        // 分区起始偏移
        //
        uint64_t Offset = 0;



        //
        // 分区大小
        //
        uint64_t Size = 0;



        //
        // 是否存在盘符
        //
        bool HasDriveLetter = false;



        //
        // 盘符
        //
        // 例如:
        // C:
        //
        std::wstring DriveLetter;



        //
        // Volume GUID路径
        //
        // 例如:
        // \\?\Volume{xxxx}
        //
        std::wstring VolumePath;

    };






    //
    // 未分配空间
    //
    // 用于备份时跳过
    //
    struct UnallocatedRange
    {

        //
        // 起始偏移
        //
        uint64_t Offset = 0;



        //
        // 长度
        //
        uint64_t Size = 0;

    };







    //
    // 磁盘信息
    //
    struct DiskInfo
    {

        //
        // Windows磁盘编号
        //
        // PhysicalDriveN
        //
        int DevNo = -1;




        //
        // 物理设备路径
        //
        // \\.\PhysicalDrive0
        //
        std::wstring PhysicalPath;



        //
        // 磁盘总容量
        //
        uint64_t Size = 0;



        //
        // 逻辑扇区大小
        //
        // 后续GPT/LBA计算需要
        //
        uint32_t BytesPerSector = 0;




        //
        // 厂商
        //
        std::wstring Vendor;



        //
        // 型号
        //
        std::wstring Model;



        //
        // 序列号
        //
        std::wstring SerialNumber;




        //
        // GPT磁盘
        //
        bool IsGPT = false;



        //
        // MBR磁盘
        //
        bool IsMBR = false;



        //
        // GPT信息
        //
        GPTLayout GPT;



        //
        // MBR信息
        //
        MBRLayout MBR;



        //
        // 已存在分区
        //
        std::vector<PartitionInfo> Partitions;



        //
        // 未分配区域
        //
        std::vector<UnallocatedRange> Unallocated;

    };









    class DiskScanner
    {

    public:


        DiskScanner();


        ~DiskScanner();



    public:


        //
        // 查询所有磁盘
        //
        //
        // 返回:
        // true  成功
        // false 失败
        //
        bool EnumerateAll(
            std::vector<DiskInfo>& disks
        );




        //
        // 查询指定磁盘
        //
        //
        // devno:
        // PhysicalDrive编号
        //
        bool Enumerate(
            int devno,
            DiskInfo& disk
        );




    private:


        //
        // 打开PhysicalDrive
        //
        bool OpenDisk(
            int devno,
            HANDLE& handle
        );




        //
        // 查询磁盘容量
        //
        bool QueryDiskSize(
            HANDLE hDisk,
            uint64_t& size
        );




        //
        // 查询扇区大小
        //
        bool QueryGeometry(
            HANDLE hDisk,
            DiskInfo& disk
        );




        //
        // 查询GPT/MBR和分区布局
        //
        bool QueryLayout(
            HANDLE hDisk,
            DiskInfo& disk
        );




        //
        // 查询厂商、型号、序列号
        //
        bool QueryStorageInfo(
            HANDLE hDisk,
            DiskInfo& disk
        );




        //
        // 查询分区盘符
        //
        void QueryDriveLetter(
            int devno,
            PartitionInfo& partition
        );




        //
        // 计算未分配空间
        //
        void CalculateUnallocated(
            DiskInfo& disk
        );

        //
        // 读取GPT Header
        //
        // GPT Header位于:
        // LBA1
        //
        bool ReadGPTHeader(
            HANDLE hDisk,
            DiskInfo& disk
        );

    };


}