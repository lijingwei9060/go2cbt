#include "go2cbt.h"


// ========================================
// 核心: 查找或创建 Hook 表条目 (去重!)
// ========================================

/**
 * FindOrCreateHookEntry - 查找已有的 Hook 条目, 或创建新的
 *
 * 关键逻辑:
 * - 如果 DriverObject 已在表中 → 返回已有条目 (避免二次 Hook!)
 * - 如果 DriverObject 不在表中 → 创建新条目, 保存 OriginalWrite, Hook MajorFunction
 *
 * 这保证了:
 * 1. 同一个 DriverObject (如 disk.sys) 不会被 Hook 两次
 * 2. 不同 DriverObject (如 disk.sys 和 vhdmp.sys) 各有独立的 OriginalWrite
 * 3. OriginalWrite 不会被覆盖
 */
PHOOK_ENTRY FindOrCreateHookEntry(PDRIVER_OBJECT DriverObject) {
	// ---- 第一步: 在已有表中查找 ----
	for (ULONG i = 0; i < g_HookListCount; i++) {
		if (g_HookList[i].DriverObject == DriverObject) {
			KdPrint(("CBT: DriverObject 0x%p already hooked (entry [%lu], RefCount=%lu)\n",
				DriverObject, i, g_HookList[i].RefCount));
			g_HookList[i].RefCount++;  // 引用计数增加
			return &g_HookList[i];
		}
	}

	// ---- 第二步: 不在表中, 创建新条目 ----
	if (g_HookListCount >= MAX_HOOKED_DRIVERS) {
		KdPrint(("CBT: Too many hooked drivers! Max=%d\n", MAX_HOOKED_DRIVERS));
		return NULL;
	}

	PHOOK_ENTRY newEntry = &g_HookList[g_HookListCount];
	newEntry->DriverObject = DriverObject;
	newEntry->RefCount = 1;

	// ---- 第三步: 保存原始函数指针 (在 Hook 之前!) ----
	newEntry->OriginalWrite = DriverObject->MajorFunction[IRP_MJ_WRITE];

	KdPrint(("CBT: New hook entry [%lu] for DriverObject 0x%p\n", g_HookListCount, DriverObject));
	KdPrint(("CBT:   OriginalWrite = 0x%p\n", (PVOID)newEntry->OriginalWrite));

	// ---- 第四步: 执行 Hook (原子替换) ----
	InterlockedExchangePointer((PVOID*)&DriverObject->MajorFunction[IRP_MJ_WRITE], (PVOID)HwReadWrite);

	// ---- 第五步: 自验证 ----
	PDRIVER_DISPATCH verifyAddr = DriverObject->MajorFunction[IRP_MJ_WRITE];
	if (verifyAddr == (PDRIVER_DISPATCH)HwReadWrite) {
		newEntry->HookInstalled = TRUE;
		KeQuerySystemTime(&newEntry->HookInstallTime);
		KdPrint(("CBT: Hook verified! MajorFunction[4] = 0x%p (HwReadWrite)\n", (PVOID)verifyAddr));
	}
	else {
		newEntry->HookInstalled = FALSE;
		KdPrint(("CBT: Hook FAILED! MajorFunction[4] = 0x%p, expected 0x%p\n", (PVOID)verifyAddr, (PVOID)HwReadWrite));
		// 可能另一个驱动已经 Hook 了
		// 检查 verifyAddr 是否是某个已知的 Hook 函数
	}

	g_HookListCount++;
	return newEntry;
}

// ========================================
// DriverEntry: 构建两个表
// ========================================

NTSTATUS BuildDiskAndHookTables(PDRIVER_OBJECT DriverObject) {
	UNREFERENCED_PARAMETER(DriverObject);
	NTSTATUS status;

	KdPrint(("CBT: ===== Building Disk Map & Hook Tables =====\n"));

	for (ULONG diskNum = 0; diskNum < MAX_DISKS; diskNum++) {

		for (ULONG partNum = 0; partNum < MAX_PARTITIONS; partNum++) {
			WCHAR nameBuf[64];
			UNICODE_STRING devName;

			// 构建设备名
			RtlStringCchPrintfW(
				nameBuf,
				RTL_NUMBER_OF(nameBuf),
				L"\\Device\\Harddisk%lu\\Partition%1u",
				diskNum,
				partNum
			);
			RtlInitUnicodeString(&devName, nameBuf);

			PFILE_OBJECT fileObj = NULL;
			PDEVICE_OBJECT devObj = NULL;

			status = IoGetDeviceObjectPointer(&devName, FILE_READ_ATTRIBUTES, &fileObj, &devObj);
			if (!NT_SUCCESS(status)) {
				if (partNum == 0) {
					KdPrint(("CBT: Disk %lu not found (status=0x%08x). Stopping disk scan.\n",
						diskNum, status));
				}
				break;  // 分区不存在, 停止扫描此磁盘的分区
			}

			PDRIVER_OBJECT drvObj = devObj->DriverObject;
			KdPrint(("CBT: Found %ws -> DevObj=0x%p DrvObj=0x%p (driver=%ws)\n",
				nameBuf, devObj, drvObj,
				drvObj ? L"unknown" : L"NULL"));

			// ---- 核心操作: FindOrCreateHookEntry ----
			// 这会:
			//   1. 检查 drvObj 是否已被 Hook (去重)
			//   2. 如果没被 Hook, 保存 OriginalWrite 并执行 Hook
			//   3. 返回 Hook 表条目指针

			PHOOK_ENTRY hookEntry = FindOrCreateHookEntry(drvObj);
			if (!hookEntry) {
				KdPrint(("CBT: Cannot create hook entry for %ws. Skipping.\n", nameBuf));
				ObDereferenceObject(fileObj);
				continue;
			}

			// ---- 填充 Disk Map 表 ----
			if (g_DiskMapCount < MAX_DISK_MAP_ENTRIES) {
				PDISK_MAP_ENTRY mapEntry = &g_DiskMap[g_DiskMapCount];
				mapEntry->DeviceObject = devObj;
				mapEntry->HookEntry = hookEntry;           // ← 关键! 指向 Hook 表
				mapEntry->DiskNumber = diskNum;
				mapEntry->PartitionNumber = partNum;
				mapEntry->IsPartition0 = (partNum == 0);

				status = QueryPartitionInfoEx(devObj,
					&mapEntry->PartitionStartingOffset,
					&mapEntry->PartitionLength);
				if (!NT_SUCCESS(status)) {
					KdPrint(("CBT: Failed to query partition info for %ws, using defaults", nameBuf));
					// fallback 保持 0（和原来行为一致）
					mapEntry->PartitionStartingOffset.QuadPart = 0;
					mapEntry->PartitionLength.QuadPart = 0;
				}

				status = InitCbtState(&mapEntry->CbtState, mapEntry->PartitionLength.QuadPart);
				if (!NT_SUCCESS(status)) {
					// ⚠️ 策略选择（二选一）:

					// 【方案 A】降级运行: 该磁盘不追踪 CBT，但仍然 Hook 写入并透传
					//   → 优点: 驱动仍然工作，其他磁盘正常追踪
					//   → 缺点: 这个磁盘的变更不会被记录（增量备份会回退到全量备份）
					KdPrint(("CBT: WARNING: Disk %lu CBT init FAILED (0x%08x). Running without CBT tracking for this disk.\n", diskNum, status));
					// mapEntry->CbtState 保持零初始化状态 (Buffer=NULL)
					// 继续往下走，g_DiskMapCount++

					// 【方案 B】致命失败: 整个驱动不加载
					//   → 优点: 要么全有要么全无，行为可预测
					//   → 缺点: 因为一个位图分配失败导致整个驱动不可用
					//
					// return status;  // 直接向上冒泡，DriverEntry 返回失败
				}

				g_DiskMapCount++;

				KdPrint(("CBT: DiskMap[%lu] Disk=%lu Part=%lu DevObj=0x%p "
					"HookEntry=0x%p OrigWrite=0x%p\n",
					g_DiskMapCount - 1, diskNum, partNum,
					devObj, hookEntry, (PVOID)hookEntry->OriginalWrite));
			}

			ObDereferenceObject(fileObj);
		}
	}

	// ---- 打印汇总 ----
	KdPrint(("CBT: ===== Summary =====\n"));
	KdPrint(("CBT: Hooked drivers: %lu\n", g_HookListCount));
	for (ULONG i = 0; i < g_HookListCount; i++) {
		PHOOK_ENTRY e = &g_HookList[i];
		UNREFERENCED_PARAMETER(e);
		KdPrint(("CBT:   [%lu] DriverObject=0x%p OrigWrite=0x%p "
			"Installed=%s RefCount=%lu\n",
			i, e->DriverObject, (PVOID)e->OriginalWrite,
			e->HookInstalled ? "YES" : "NO", e->RefCount));
	}

	KdPrint(("CBT: Disk map entries: %lu\n", g_DiskMapCount));
	for (ULONG i = 0; i < g_DiskMapCount; i++) {
		PDISK_MAP_ENTRY e = &g_DiskMap[i];
		UNREFERENCED_PARAMETER(e);
		KdPrint(("CBT:   [%lu] Disk=%lu Part=%lu DevObj=0x%p "
			"HookEntryIdx=0x%p StartOff=0x%llx Length=0x%llx\n",
			i, e->DiskNumber, e->PartitionNumber,
			e->DeviceObject, e->HookEntry,
			e->PartitionStartingOffset.QuadPart,
			e->PartitionLength.QuadPart));
	}

	return STATUS_SUCCESS;
}




// ========================================
// 获取分区信息: StartingOffset + PartitionLength
//
// 输入:  PDEVICE_OBJECT (已通过 IoGetDeviceObjectPointer 获取的设备对象)
// 输出: pStartingOffset, pPartitionLength
// 返回: NTSTATUS
// ========================================
NTSTATUS
QueryPartitionInfoEx(
	_In_ PDEVICE_OBJECT DeviceObject,
	_Out_ PLARGE_INTEGER pStartingOffset,
	_Out_ PLARGE_INTEGER pPartitionLength
)
{
	if (!DeviceObject || !pStartingOffset || !pPartitionLength) {
		return STATUS_INVALID_PARAMETER;
	}

	if (pStartingOffset) pStartingOffset->QuadPart = 0;
	if (pPartitionLength) pPartitionLength->QuadPart = 0;

	// ---- 安全检查: 确保在 PASSIVE_LEVEL ----
	KIRQL currentIrql = KeGetCurrentIrql();
	if (currentIrql != PASSIVE_LEVEL) {
		KdPrint(("CBT-QUERY: Cannot query at IRQL %u, must be PASSIVE\n",
			currentIrql));
		return STATUS_INVALID_DEVICE_STATE;   // 不崩溃，优雅退出
	}

	PARTITION_INFORMATION_EX partInfoEx = { 0 };
	IO_STATUS_BLOCK ioStatusBlock;
	KEVENT completionEvent;

	// ---- 使用事件做异步模式（更安全，避免线程阻塞问题）----
	KeInitializeEvent(&completionEvent, SynchronizationEvent, FALSE);

	PIRP irp = IoBuildDeviceIoControlRequest(
		IOCTL_DISK_GET_PARTITION_INFO_EX,
		DeviceObject,
		NULL,
		0,
		&partInfoEx,
		sizeof(PARTITION_INFORMATION_EX),
		FALSE,
		&completionEvent,       // ← 用事件，不依赖线程阻塞语义
		&ioStatusBlock
	);

	if (!irp) {
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	// ---- 引用设备栈顶层 ----
	PDEVICE_OBJECT targetDevice = IoGetAttachedDeviceReference(DeviceObject);

	// ---- 下发 IRP ----
	IoCallDriver(targetDevice, irp);

	// ---- 等待完成（仅在 PASSIVE_LEVEL 安全）----
	// 使用超时防止永久挂起（30秒）
	NTSTATUS waitStatus = KeWaitForSingleObject(
		&completionEvent,
		Executive,
		KernelMode,
		FALSE,               // 不可中断
		&(LARGE_INTEGER){.QuadPart = -30 * 10000 * 1000 }  // -30秒(负数=相对时间)
	);

	ObDereferenceObject(targetDevice);

	if (waitStatus == STATUS_TIMEOUT) {
		KdPrint(("CBT-QUERY: Timeout waiting for partition info!\n"));
		return STATUS_IO_TIMEOUT;
	}

	if (NT_SUCCESS(ioStatusBlock.Status)) {
		if (pStartingOffset)   *pStartingOffset = partInfoEx.StartingOffset;
		if (pPartitionLength)  *pPartitionLength = partInfoEx.PartitionLength;
	}

	return ioStatusBlock.Status;
}
