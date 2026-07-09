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

		for (ULONG partNum = 0; partNum <= MAX_PARTITIONS; partNum++) {
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
				mapEntry->PartitionStartingOffset.QuadPart = 0; // 默认值, 需要用户态 IOCTL 补充

				if (partNum == 0) {
					// Partition0 = 整个磁盘, StartingOffset = 0
					mapEntry->PartitionStartingOffset.QuadPart = 0;
				}
				// 分区的 StartingOffset 需要通过用户态 IOCTL 传入
				// (参见 setup_partition_offsets.py)

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
		KdPrint(("CBT:   [%lu] DriverObject=0x%p OrigWrite=0x%p "
			"Installed=%s RefCount=%lu\n",
			i, e->DriverObject, (PVOID)e->OriginalWrite,
			e->HookInstalled ? "YES" : "NO", e->RefCount));
	}

	KdPrint(("CBT: Disk map entries: %lu\n", g_DiskMapCount));
	for (ULONG i = 0; i < g_DiskMapCount; i++) {
		PDISK_MAP_ENTRY e = &g_DiskMap[i];
		KdPrint(("CBT:   [%lu] Disk=%lu Part=%lu DevObj=0x%p "
			"HookEntryIdx=? StartOff=0x%llx\n",
			i, e->DiskNumber, e->PartitionNumber,
			e->DeviceObject, e->PartitionStartingOffset.QuadPart));
	}

	return STATUS_SUCCESS;
}




