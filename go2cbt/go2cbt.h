#pragma once
#include <ntddk.h>
#include <ntdddisk.h>
#include <ntddstor.h>
#include <ntstrsafe.h>

#define CBT_DEVICE_NAME     L"\\Device\\CbtMonitor"
#define CBT_SYMLINK_NAME    L"\\DosDevices\\CbtMonitor"

// IOCTL (与 verify_hook_test.cpp 对应)
#define IOCTL_QUERY_HOOK_STATUS  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_QUERY_WRITE_STATS  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_QUERY_CBT_DATA     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RESET_STATS     

// CBT 变更记录
typedef struct _CBT_RECORD {
	ULONG DiskNumber;
	LARGE_INTEGER ByteOffset;
	ULONG ByteCount;
	ULONG SequenceNumber;
} CBT_RECORD, * PCBT_RECORD;

// SLIST 链表节点 (无锁队列)
typedef struct _CBT_SLIST_ENTRY {
	SLIST_ENTRY ListEntry;
	CBT_RECORD  Record;
} CBT_SLIST_ENTRY, * PCBT_SLIST_ENTRY;

#define MAX_CBT_SLIST_POOL  4096
#define MAX_HOOKED_DRIVERS  8    // 最多 Hook 8 个不同的驱动
#define MAX_DISK_MAP_ENTRIES 64   // 最多 64 个磁盘/分区设备
#define MAX_DISKS           8    // 最多检查 8 个物理磁盘
#define MAX_PARTITIONS      16   // 每磁盘最多 16 个分区


// ========================================
// 数据结构设计 - 两个独立的表
// ========================================

// Hook 表: 按 DriverObject 存储 (每个被 Hook 的驱动一条记录)
// 这个表解决 "OriginalWrite 不止一个" 的问题
typedef struct _HOOK_ENTRY {
	PDRIVER_OBJECT   DriverObject;           // 被 Hook 的驱动对象 (查找键)
	PDRIVER_DISPATCH OriginalWrite;           // 该驱动的原始 IRP_MJ_WRITE 函数
	BOOLEAN          HookInstalled;           // 是否已安装 Hook
	ULONG            RefCount;                // 多少个磁盘设备引用此驱动
	LARGE_INTEGER    HookInstallTime;         // Hook 安装时间
} HOOK_ENTRY, * PHOOK_ENTRY;

// 磁盘映射表: 按 DeviceObject 存储 (每个磁盘/分区设备一条记录)
// 这个表解决 "DiskNum 保存在哪里" 和 "偏移转换" 的问题
typedef struct _DISK_MAP_ENTRY {
	PDEVICE_OBJECT   DeviceObject;           // IRP 中的 DeviceObject 参数 (查找键)
	PHOOK_ENTRY      HookEntry;              // → 指向 Hook 表中的对应条目 (核心!)
	ULONG            DiskNumber;             // Harddisk%d 中的 %d
	ULONG            PartitionNumber;         // Partition%d 中的 %d
	LARGE_INTEGER    PartitionStartingOffset; // 分区在磁盘上的起始偏移
	LARGE_INTEGER    PartitionLength;         // 分区在磁盘上的大小
	BOOLEAN          IsPartition0;            // 是否代表整个磁盘
} DISK_MAP_ENTRY, * PDISK_MAP_ENTRY;


// 全局变量
extern PDEVICE_OBJECT g_ControlDevice;
extern HOOK_ENTRY     g_HookList[MAX_HOOKED_DRIVERS];
extern ULONG          g_HookListCount;

extern DISK_MAP_ENTRY g_DiskMap[MAX_DISK_MAP_ENTRIES];
extern ULONG          g_DiskMapCount;

// ========================================
// Forward declarations
// ========================================
NTSTATUS HwReadWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS CbtDispatchCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS CbtDispatchClose(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS CbtDispatchDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp);
VOID CbtUnload(PDRIVER_OBJECT DriverObject);
//NTSTATUS InstallAndVerifyHook(PDRIVER_OBJECT DriverObject);


PHOOK_ENTRY FindOrCreateHookEntry(PDRIVER_OBJECT DriverObject);
NTSTATUS BuildDiskAndHookTables(PDRIVER_OBJECT DriverObject);

NTSTATUS
QueryPartitionInfoEx(
	_In_ PDEVICE_OBJECT DeviceObject,
	_Out_ PLARGE_INTEGER pStartingOffset,
	_Out_ PLARGE_INTEGER pPartitionLength
);