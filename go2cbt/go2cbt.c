#include "go2cbt.h"



// 全局变量
PDEVICE_OBJECT g_ControlDevice = NULL;
// 全局表
HOOK_ENTRY     g_HookList[MAX_HOOKED_DRIVERS] = { 0 };
ULONG          g_HookListCount = 0;

DISK_MAP_ENTRY g_DiskMap[MAX_DISK_MAP_ENTRIES] = { 0 };
ULONG          g_DiskMapCount = 0;

// ========================================
// DriverEntry
// ========================================
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
	UNREFERENCED_PARAMETER(RegistryPath);
	NTSTATUS status;
	UNICODE_STRING deviceName, symlinkName;
	PDEVICE_OBJECT deviceObject = NULL;

	KdPrint(("CBT: DriverEntry called\n"));

	// ---- 创建控制设备 (用于 IOCTL 通信) ----
	RtlInitUnicodeString(&deviceName, CBT_DEVICE_NAME);
	status = IoCreateDevice(DriverObject, 0,
		&deviceName, FILE_DEVICE_UNKNOWN,
		0, FALSE, &deviceObject);
	if (!NT_SUCCESS(status)) {
		KdPrint(("CBT: IoCreateDevice failed: 0x%08x\n", status));
		return status;
	}
	// 创建符号链接 (用户态可访问)
	RtlInitUnicodeString(&symlinkName, CBT_SYMLINK_NAME);
	status = IoCreateSymbolicLink(&symlinkName, &deviceName);
	if (!NT_SUCCESS(status)) {

		IoDeleteDevice(deviceObject);

		KdPrint(("CBT: IoCreateSymbolicLink failed: 0x%08x\n", status));
		return status;
	}

	// 初始化设备扩展
	g_ControlDevice = deviceObject;

	// 设置控制设备的 MajorFunction (不是 Hook, 是正常的分发函数)
	DriverObject->MajorFunction[IRP_MJ_CREATE] = CbtDispatchCreate;
	DriverObject->MajorFunction[IRP_MJ_CLOSE] = CbtDispatchClose;
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = CbtDispatchDeviceControl;
	DriverObject->DriverUnload = CbtUnload;

	deviceObject->Flags |= DO_BUFFERED_IO;
	deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

	// ---- 安装 Hook 并自验证 ----
	KdPrint(("CBT: ===== Installing MajorFunction Hook =====\n"));
	status = BuildDiskAndHookTables(DriverObject);

	if (!NT_SUCCESS(status)) {
		KdPrint(("CBT: Hook installation failed. Driver will still load for diagnostics.\n"));
		// 驱动仍然加载, 但 HookInstalled = FALSE
		// 用户态可以通过 IOCTL 查询失败原因
	}


	return STATUS_SUCCESS;  // 即使 Hook 失败也返回成功, 让用户态诊断
}

// ========================================
// Unload: 按 Hook 表逐个恢复, 按 RefCount 安全卸载
// ========================================
VOID CbtUnload(PDRIVER_OBJECT DriverObject) {
	UNREFERENCED_PARAMETER(DriverObject);
	KdPrint(("CBT: ===== Unload: Restoring hooks =====\n"));

	// ---- 按 Hook 表逐个恢复 OriginalWrite ----
	for (ULONG i = 0; i < g_HookListCount; i++) {
		PHOOK_ENTRY entry = &g_HookList[i];

		if (!entry->HookInstalled) {
			KdPrint(("CBT: [%lu] Not installed, skipping\n", i));
			continue;
		}

		// 验证当前 MajorFunction[4] 是否仍然是我们的 Hook
		PDRIVER_DISPATCH currentWrite = entry->DriverObject->MajorFunction[IRP_MJ_WRITE];

		if (currentWrite == (PDRIVER_DISPATCH)HwReadWrite) {
			// ✓ Hook 仍然是我们安装的, 可以安全恢复
			KdPrint(("CBT: [%lu] Hook still active. Restoring OriginalWrite=0x%p "
				"to DriverObject=0x%p\n",
				i, (PVOID)entry->OriginalWrite, (PVOID)entry->DriverObject));

			InterlockedExchangePointer(
				(PVOID*)&entry->DriverObject->MajorFunction[IRP_MJ_WRITE],
				(PVOID)entry->OriginalWrite
			);

			// 自验证恢复
			PDRIVER_DISPATCH restoredWrite = entry->DriverObject->MajorFunction[IRP_MJ_WRITE];
			if (restoredWrite == entry->OriginalWrite) {
				KdPrint(("CBT: [%lu] Restored successfully (0x%p)\n",
					i, (PVOID)restoredWrite));
			}
			else {
				KdPrint(("CBT: [%lu] Restoration FAILED! Current=0x%p Expected=0x%p\n",
					i, (PVOID)restoredWrite, (PVOID)entry->OriginalWrite));
			}
		}
		else if (currentWrite == entry->OriginalWrite) {
			// Hook 已经被其他人恢复了 (可能是另一个驱动做的)
			KdPrint(("CBT: [%lu] Hook already restored by someone else (0x%p)\n",
				i, (PVOID)currentWrite));
		}
		else {
			// ⚠ 另一个驱动在我们之后也 Hook 了同一个位置
			// currentWrite 既不是 HwReadWrite, 也不是 OriginalWrite
			KdPrint(("CBT: [%lu] Another driver hooked this! Current=0x%p "
				"Our=0x%p Orig=0x%p\n",
				i, (PVOID)currentWrite, (PVOID)HwReadWrite,
				(PVOID)entry->OriginalWrite));

			// 策略选择:
			// A) 不恢复 → 安全但另一个驱动可能期望调用我们的 Hook
			// B) 恢复 OriginalWrite → 可能破坏另一个驱动的 Hook 链

			// 推荐: 只恢复如果 currentWrite == HwReadWrite
			// 如果 currentWrite != HwReadWrite, 说明另一个驱动覆盖了我们
			// 需要更复杂的 Hook 链管理 (参见下面的 "Hook 链" 说明)
			KdPrint(("CBT: [%lu] Skipping restoration to avoid breaking other driver's hook\n", i));
		}
	}

	KdPrint(("CBT: ===== Unload complete =====\n"));
}