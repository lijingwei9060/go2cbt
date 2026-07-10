#pragma once
#include <ntddk.h>
#include <ntdddisk.h>
#include <ntddstor.h>
#include <ntstrsafe.h>

#define CBT_DEVICE_NAME     L"\\Device\\CbtMonitor"
#define CBT_SYMLINK_NAME    L"\\DosDevices\\CbtMonitor"

// 每个追踪块大小: 1MB
// 可配置, 越小越精确但内存越大
#define CBT_BLOCK_SIZE              (1024 * 1024)

// Pool tag for CBT bitmap memory allocation
#define CBT_BITMAP_POOL_TAG         'tBCG'

// IOCTL (与 verify_hook_test.cpp 对应)
#define IOCTL_QUERY_HOOK_STATUS  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_QUERY_WRITE_STATS  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_QUERY_CBT_DATA     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RESET_CBT_DATA    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)    


// ---- CBT State per disk (one bitmap) ----
typedef struct _DISK_CBT_STATE {
	RTL_BITMAP      Bitmap;             // 位图结构体
	PULONG          Buffer;             // 位图内存 (NonPagedPool)
	ULONGLONG       TotalBits;          // 总bit数量，一个bit代表一个块
	ULONGLONG       TotalBytes;         // 总字节数，为了对其Long(4byte),
	KSPIN_LOCK      Lock;               // 自旋锁: 保护 Bitmap 的 Set/Clear 并发访问
} DISK_CBT_STATE, * PDISK_CBT_STATE;

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
#define MAX_DISKS           32    // 最多检查 32 个物理磁盘
#define MAX_PARTITIONS      1   // 只需要监控Partition0就可以了


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
	DISK_CBT_STATE   CbtState;                // 每个分区的 CBT 位图状态 (RTL_BITMAP + Lock)
} DISK_MAP_ENTRY, * PDISK_MAP_ENTRY;

// ---- 输入: 用户传给驱动的参数 ----
typedef struct _CBT_IOCTL_INPUT {
	ULONG DeviceNumber;        // 磁盘编号: 0=Harddisk0, 1=Harddisk1, ...
} CBT_IOCTL_INPUT, * PCBT_IOCTL_INPUT;

// ---- Query 输出: 驱动返回给用户的 CBT 数据 ----
typedef struct _CBT_QUERY_OUTPUT {
	ULONGLONG   TotalBits;            // 位图总 bit 数 = TotalBlocks (每个块一个bit)
	ULONGLONG   TotalBytes;           // 位图缓冲区实际字节大小 ((TotalBits+31)/32 * 4)
	// 后面紧跟着实际的位图数据 (变长)
	UCHAR BitmapData[1];             // 位图原始数据
} CBT_QUERY_OUTPUT, * PCBT_QUERY_OUTPUT;

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

NTSTATUS
InitCbtState(
	_Out_ PDISK_CBT_STATE cbtState,
	_In_  ULONGLONG diskSizeBytes
);

// ================================================
// MarkBlockChanged: Mark a range of disk blocks as "changed" in the CBT bitmap.
//
// This is the HOT PATH - called on every single disk WRITE operation.
// The critical section (spinlock held) is only 1 instruction: RlSetBits().
//
// Thread safety:
//   - KSPIN_LOCK protects against concurrent RlSetBits vs RlClearAllBits
//   - Lost set due to race is unacceptable (would cause data corruption in backup),
//     so we always acquire the lock before modifying the bitmap.
// ================================================
static __inline void MarkBlockChanged(
	_In_ PDISK_CBT_STATE cbtState,
	_In_ ULONGLONG offset,
	_In_ ULONG length
)
{
	// ===== 第一道防线: 检查 CbtState 是否已正确初始化 =====
	if (!cbtState || !cbtState->Buffer || cbtState->TotalBits == 0) {
		// CbtState 未初始化或初始化失败(Buffer=NULL)
		// → 安全跳过，只做透传，不标记任何块
		return;   // ✓ 不崩溃，不影响写操作
	}

	if (offset >= (cbtState->TotalBits * CBT_BLOCK_SIZE)) {
		// Write offset beyond disk bounds (should not happen, but be safe)
		return;
	}

	ULONGLONG startBlock = offset / CBT_BLOCK_SIZE;
	ULONGLONG endBlock = (offset + (ULONGLONG)length - 1) / CBT_BLOCK_SIZE;

	if (endBlock >= cbtState->TotalBits) {
		endBlock = cbtState->TotalBits - 1;
	}

	if (startBlock > endBlock) {
		return;  // Zero-length or invalid range
	}

	// ---- Acquire spinlock, mark bits, release ----
	// Critical section = exactly 1 line of code (~10-20ns)
	// Disk I/O itself takes microseconds to milliseconds, so this overhead is negligible
	KIRQL oldIrql = 0;
	KeAcquireSpinLock(&cbtState->Lock, &oldIrql);

	RtlSetBits(&cbtState->Bitmap, (ULONG)startBlock, (ULONG)(endBlock - startBlock + 1));

	KeReleaseSpinLock(&cbtState->Lock, oldIrql);

}


void CleanupCbtState(_In_ PDISK_CBT_STATE cbtState);
static NTSTATUS HandleIoctlQuery(
	_In_ PIRP Irp,
	_In_ PIO_STACK_LOCATION irpSp,
	_Out_ PULONG pBytesReturned
);
static NTSTATUS HandleIoctlReset(
	_In_ PIRP Irp,
	_In_ PIO_STACK_LOCATION irpSp,
	_Out_ PULONG pBytesReturned
);