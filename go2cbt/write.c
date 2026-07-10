#include "go2cbt.h"

static LONG g_WriteDebugCount = 0;
// ========================================
// HwReadWrite - 用 HookEntry 查找正确的 OriginalWrite
// ========================================
NTSTATUS HwReadWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
	PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);

	// ---- 只处理 IRP_MJ_WRITE ----
	if (irpSp->MajorFunction != IRP_MJ_WRITE) {
		// IRP_MJ_READ 等, 需要找到对应的 OriginalRead
		// 这里简化处理: 只 Hook 了 WRITE, READ 用 DriverObject 的当前值
		// (如果也 Hook 了 READ, 需要类似查找逻辑)
		PDRIVER_OBJECT drvObj = DeviceObject->DriverObject;
		return drvObj->MajorFunction[irpSp->MajorFunction](DeviceObject, Irp);
	}

	LARGE_INTEGER byteOffset = irpSp->Parameters.Write.ByteOffset;
	ULONG byteCount = irpSp->Parameters.Write.Length;

	// ---- Step 1: 在 g_DiskMap 中查找 DeviceObject ----
	PDISK_MAP_ENTRY diskEntry = NULL;
	for (ULONG i = 0; i < g_DiskMapCount; i++) {
		if (g_DiskMap[i].DeviceObject == DeviceObject) {
			diskEntry = &g_DiskMap[i];
			break;
		}
	}

	if (!diskEntry) {
		// 未知设备对象
		// 可能是驱动加载后新创建的磁盘设备
		// 安全做法: 用 DeviceObject->DriverObject 直接查找 Hook 表
		PDRIVER_OBJECT drvObj = DeviceObject->DriverObject;
		PHOOK_ENTRY hookEntry = NULL;
		for (ULONG i = 0; i < g_HookListCount; i++) {
			if (g_HookList[i].DriverObject == drvObj) {
				hookEntry = &g_HookList[i];
				break;
			}
		}

		if (hookEntry && hookEntry->HookInstalled) {
			// 找到了对应的 Hook 条目, 但磁盘映射未知
			// 透传, 但 KdPrint 记录这个 IRP 以便调试
			/*KdPrint(("CBT-WRITE: Unknown disk device 0x%p (DrvObj=0x%p), "
				"Offset=0x%llx Len=0x%lx. Passing through.\n",
				DeviceObject, drvObj, byteOffset.QuadPart, byteCount));*/
			return hookEntry->OriginalWrite(DeviceObject, Irp);
		}

		// 完全未知, 直接用当前 MajorFunction 值 (可能已被我们 Hook 了)
		KdPrint(("CBT-WRITE: Completely unknown device 0x%p. Emergency passthrough.\n", DeviceObject));
		return drvObj->MajorFunction[IRP_MJ_WRITE](DeviceObject, Irp);
		// 注意: 这里不能调 HwReadWrite, 否则死循环!
		// drvObj->MajorFunction[IRP_MJ_WRITE] 可能就是 HwReadWrite
		// 但如果 HookInstalled=FALSE 或不在我们的表中, 就不是 HwReadWrite
	}

	// ---- Step 2: 获取 OriginalWrite (通过 DiskEntry → HookEntry) ----
	PHOOK_ENTRY hookEntry = diskEntry->HookEntry;
	PDRIVER_DISPATCH originalWrite = hookEntry->OriginalWrite;

	// ---- Step 3: 偏移转换 ----
	LARGE_INTEGER diskAbsoluteOffset;
	diskAbsoluteOffset.QuadPart = byteOffset.QuadPart + diskEntry->PartitionStartingOffset.QuadPart;

	if (InterlockedIncrement(&g_WriteDebugCount) <= 100)
	{

		KdPrint(("CBT-WRITE: Disk%lu Part%lu DevObj=0x%p "
			"PartOff=0x%llx + StartOff=0x%llx = DiskOff=0x%llx "
			"Len=0x%lx OrigWrite=0x%p\n",
			diskEntry->DiskNumber, diskEntry->PartitionNumber, DeviceObject,
			byteOffset.QuadPart,
			diskEntry->PartitionStartingOffset.QuadPart,
			diskAbsoluteOffset.QuadPart,
			byteCount, (PVOID)originalWrite));

	}

	// ---- Step 4: 记录 CBT ----
	// DiskNum + DiskAbsoluteOffset + ByteCount = 完整变更信息
	// (存入 SLIST 等, 此处省略)

	MarkBlockChanged(&diskEntry->CbtState, (ULONGLONG)diskAbsoluteOffset.QuadPart, byteCount);

	// ---- Step 5: 透传到对应的原始函数 ----
	// 关键: 每个磁盘用的是自己的 OriginalWrite!
	// disk.sys 管理的磁盘 → disk.sys 的原始写函数
	// vhdmp.sys 管理的磁盘 → vhdmp.sys 的原始写函数
	return originalWrite(DeviceObject, Irp);
}