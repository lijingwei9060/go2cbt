#include "go2cbt.h"

// ========================================
// Hook 安装与验证 - 核心逻辑
// ========================================
NTSTATUS InstallAndVerifyHook(PDRIVER_OBJECT DriverObject) {
	UNREFERENCED_PARAMETER(DriverObject);
	NTSTATUS status;
	UNICODE_STRING diskDeviceName;
	PFILE_OBJECT fileObject = NULL;
	PDEVICE_OBJECT diskDeviceObject = NULL;
	PDRIVER_OBJECT diskDriverObject = NULL;

	// ---- Step 1: 找到磁盘设备对象 ----
	// 尝试 \Device\Harddisk0\Partition0 到 Harddisk3
	ULONG hookedCount = 0;

	for (ULONG diskNum = 0; diskNum < 4; diskNum++) {
		//RtlInitUnicodeString(&diskDeviceName, L"\\Device\\Harddisk0\\Partition0");
		// 动态构建设备名 (sprintf 风格)
		WCHAR nameBuf[64];
		//swprintf(nameBuf, 64, L"\\Device\\Harddisk%d\\Partition0", diskNum);
		RtlStringCchPrintfW(
			nameBuf,
			RTL_NUMBER_OF(nameBuf),
			L"\\Device\\Harddisk%lu\\Partition0",
			diskNum
		);
		RtlInitUnicodeString(&diskDeviceName, nameBuf);

		KdPrint(("CBT: Trying to open %wZ\n", &diskDeviceName));

		status = IoGetDeviceObjectPointer(&diskDeviceName, FILE_READ_ATTRIBUTES,
			&fileObject, &diskDeviceObject);
		if (!NT_SUCCESS(status)) {
			KdPrint(("CBT: Disk %lu not found (status=0x%08x). Skipping.\n",
				diskNum, status));
			continue;
		}

		// ---- Step 2: 获取磁盘驱动的 DriverObject ----
		diskDriverObject = diskDeviceObject->DriverObject;
		KdPrint(("CBT: Disk %lu DriverObject = 0x%p\n", diskNum, diskDriverObject));

		// ---- Step 3: 验证 MajorFunction[4] 当前值 ----
		PDRIVER_DISPATCH currentWrite = diskDriverObject->MajorFunction[IRP_MJ_WRITE];
		PDRIVER_DISPATCH currentRead = diskDriverObject->MajorFunction[IRP_MJ_READ];
		KdPrint(("CBT: Original IRP_MJ_WRITE handler = 0x%p\n", currentWrite));
		KdPrint(("CBT: Original IRP_MJ_READ  handler = 0x%p\n", currentRead));

		// ---- Step 4: 保存原始函数指针 ----
		g_DevExt->OriginalWrite = currentWrite;
		g_DevExt->OriginalRead = currentRead;
		g_DevExt->DiskDriverObject = diskDriverObject;

		// ---- Step 5: 原子替换 MajorFunction[IRP_MJ_WRITE] ----
		KdPrint(("CBT: Hooking IRP_MJ_WRITE with HwReadWrite (0x%p)...\n", (PVOID)HwReadWrite));

		InterlockedExchangePointer(
			(PVOID*)&diskDriverObject->MajorFunction[IRP_MJ_WRITE],
			(PVOID)HwReadWrite
		);

		// ---- Step 6: 自验证 - 读取替换后的值确认成功 ----
		PDRIVER_DISPATCH newWrite = diskDriverObject->MajorFunction[IRP_MJ_WRITE];
		KdPrint(("CBT: After hook, MajorFunction[IRP_MJ_WRITE] = 0x%p\n", newWrite));
		KdPrint(("CBT: HwReadWrite actual address = 0x%p\n", (PVOID)HwReadWrite));

		if (newWrite == (PDRIVER_DISPATCH)HwReadWrite) {
			g_DevExt->HookInstalled = TRUE;
			g_DevExt->HookVerifyAddr = (PVOID)newWrite;
			KeQuerySystemTime(&g_DevExt->HookInstallTime);
			hookedCount++;
			KdPrint(("CBT: HOOK VERIFIED for Disk %lu! MajorFunction[4] = 0x%p matches HwReadWrite\n",
				diskNum, newWrite));
		}
		else {
			g_DevExt->HookInstalled = FALSE;
			KdPrint(("CBT: HOOK FAILED for Disk %lu! MajorFunction[4] = 0x%p, expected 0x%p\n",
				diskNum, newWrite, (PVOID)HwReadWrite));
			// 可能的原因:
			// 1. 另一个驱动已经 Hook 了这个位置
			// 2. diskDriverObject 不是真正的磁盘类驱动
			// 3. 内存保护 (不太可能, MajorFunction 是可写数据段)
		}

		// ---- Step 7: 可选 - 也 Hook IRP_MJ_READ ----
		// 如果需要跟踪读操作 (CBT 通常只需要写)
		// InterlockedExchangePointer(
		//     (PVOID*)&diskDriverObject->MajorFunction[IRP_MJ_READ],
		//     (PVOID)HwReadWrite  // 或单独的 HwRead 函数
		// );

		// fileObject 需要解除引用 (IoGetDeviceObjectPointer 增加了引用)
		if (fileObject) {
			ObDereferenceObject(fileObject);
			fileObject = NULL;
		}

		// 只需要 Hook 第一个成功的磁盘 (后续可以扩展为多磁盘)
		if (g_DevExt->HookInstalled) {
			break;
		}
	}

	g_DevExt->DiskCount = hookedCount;

	if (hookedCount > 0) {
		KdPrint(("CBT: Successfully hooked %lu disk(s)\n", hookedCount));
		KdPrint(("CBT: Hook verification passed at DriverEntry level\n"));
		KdPrint(("CBT: System should remain stable - MajorFunction[] is not PatchGuard protected\n"));
		return STATUS_SUCCESS;
	}
	else {
		KdPrint(("CBT: Failed to hook any disk\n"));
		return STATUS_UNSUCCESSFUL;
	}
}