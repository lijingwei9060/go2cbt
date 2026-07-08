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

// 设备扩展
typedef struct _DEVICE_EXTENSION {
	PDEVICE_OBJECT       DeviceObject;

	// Hook 信息
	PDRIVER_OBJECT       DiskDriverObject;     // 被 Hook 的磁盘驱动对象
	PDRIVER_DISPATCH     OriginalWrite;        // 原始 IRP_MJ_WRITE 函数指针
	PDRIVER_DISPATCH     OriginalRead;         // 原始 IRP_MJ_READ 函数指针
	BOOLEAN              HookInstalled;        // Hook 是否成功安装

	// 统计
	ULONG                WriteIrpsReceived;    // 收到的写 IRP 总数
	ULONG                ReadIrpsReceived;     // 收到的读 IRP 总数
	ULONG                DiskCount;            // Hook 的磁盘数

	
	ULONG                CbtRecordCount;

	// 验证信息
	PVOID                HookVerifyAddr;       // MajorFunction[4] 被替换后的实际值
	LARGE_INTEGER        HookInstallTime;      // Hook 安装时间

} DEVICE_EXTENSION, * PDEVICE_EXTENSION;

// 全局变量
extern PDEVICE_OBJECT g_ControlDevice;
extern PDEVICE_EXTENSION g_DevExt;

// ========================================
// Forward declarations
// ========================================
NTSTATUS HwReadWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS CbtDispatchCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS CbtDispatchClose(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS CbtDispatchDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp);
VOID CbtUnload(PDRIVER_OBJECT DriverObject);
NTSTATUS InstallAndVerifyHook(PDRIVER_OBJECT DriverObject);