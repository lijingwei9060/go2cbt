#include "go2cbt.h"



// 全局变量
PDEVICE_OBJECT g_ControlDevice = NULL;
PDEVICE_EXTENSION g_DevExt = NULL;

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
    status = IoCreateDevice(DriverObject, sizeof(DEVICE_EXTENSION),
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
    g_DevExt = (PDEVICE_EXTENSION)deviceObject->DeviceExtension;
    RtlZeroMemory(g_DevExt, sizeof(DEVICE_EXTENSION));
    g_DevExt->DeviceObject = deviceObject;


    // 设置控制设备的 MajorFunction (不是 Hook, 是正常的分发函数)
    DriverObject->MajorFunction[IRP_MJ_CREATE] = CbtDispatchCreate;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = CbtDispatchClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = CbtDispatchDeviceControl;
    DriverObject->DriverUnload = CbtUnload;

    deviceObject->Flags |= DO_BUFFERED_IO;
    deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    // ---- 安装 Hook 并自验证 ----
    KdPrint(("CBT: ===== Installing MajorFunction Hook =====\n"));
    status = InstallAndVerifyHook(DriverObject);

    if (!NT_SUCCESS(status)) {
        KdPrint(("CBT: Hook installation failed. Driver will still load for diagnostics.\n"));
        // 驱动仍然加载, 但 HookInstalled = FALSE
        // 用户态可以通过 IOCTL 查询失败原因
    }
    else {
        KdPrint(("CBT: ===== Hook Verification Summary =====\n"));
        KdPrint(("CBT: HookInstalled    = %s\n", g_DevExt->HookInstalled ? "TRUE" : "FALSE"));
        KdPrint(("CBT: OriginalWrite    = 0x%p\n", (PVOID)g_DevExt->OriginalWrite));
        KdPrint(("CBT: HookVerifyAddr   = 0x%p (should match HwReadWrite)\n", g_DevExt->HookVerifyAddr));
        KdPrint(("CBT: DiskCount        = %lu\n", g_DevExt->DiskCount));
        KdPrint(("CBT: DiskDriverObject = 0x%p\n", (PVOID)g_DevExt->DiskDriverObject));
        KdPrint(("CBT: ===== Verification Complete =====\n"));
    }

    return STATUS_SUCCESS;  // 即使 Hook 失败也返回成功, 让用户态诊断
}

// ========================================
// Unload - 恢复原始 MajorFunction 并验证
// ========================================
VOID CbtUnload(PDRIVER_OBJECT DriverObject) {
    UNREFERENCED_PARAMETER(DriverObject);
    KdPrint(("CBT: Unload called - restoring original MajorFunction\n"));

    // ---- 验证当前 Hook 状态 ----
    if (g_DevExt->DiskDriverObject && g_DevExt->HookInstalled) {
        // 检查当前 MajorFunction[4] 是否仍然是我们的 hook
        PDRIVER_DISPATCH currentWrite = g_DevExt->DiskDriverObject->MajorFunction[IRP_MJ_WRITE];
        KdPrint(("CBT: Current MajorFunction[4] = 0x%p\n", (PVOID)currentWrite));
        KdPrint(("CBT: HwReadWrite address = 0x%p\n", (PVOID)HwReadWrite));

        if (currentWrite == (PDRIVER_DISPATCH)HwReadWrite) {
            KdPrint(("CBT: Hook still active, restoring original...\n"));

            // 原子恢复
            InterlockedExchangePointer(
                (PVOID*)&g_DevExt->DiskDriverObject->MajorFunction[IRP_MJ_WRITE],
                (PVOID)g_DevExt->OriginalWrite
            );

            // 验证恢复
            PDRIVER_DISPATCH restoredWrite = g_DevExt->DiskDriverObject->MajorFunction[IRP_MJ_WRITE];
            if (restoredWrite == g_DevExt->OriginalWrite) {
                KdPrint(("CBT: Original IRP_MJ_WRITE handler restored (0x%p)\n",
                    (PVOID)restoredWrite));
            }
            else {
                KdPrint(("CBT: Restoration may have failed! Current=0x%p Expected=0x%p\n",
                    (PVOID)restoredWrite, (PVOID)g_DevExt->OriginalWrite));
                // 可能另一个驱动在我们之后也 Hook 了这个位置
                // 安全做法: 确保不是我们的函数就行
                if (restoredWrite != (PDRIVER_DISPATCH)HwReadWrite) {
                    KdPrint(("CBT:At least our hook is removed (not HwReadWrite anymore)\n"));
                }
            }
        }
        else {
            KdPrint(("CBT: MajorFunction[4] changed by another driver (0x%p)\n",
                (PVOID)currentWrite));
            // 另一个驱动可能也 Hook 了同一个位置
            // 恢复原始值 (可能覆盖另一个驱动的 Hook)
            // 更安全的做法: 只恢复如果 currentWrite == HwReadWrite
            KdPrint(("CBT: Skipping restoration - another driver's hook detected\n"));
        }
    }

    // 删除符号链接和设备
    UNICODE_STRING symlinkName;
    RtlInitUnicodeString(&symlinkName, CBT_SYMLINK_NAME);
    IoDeleteSymbolicLink(&symlinkName);

    if (g_ControlDevice) {
        IoDeleteDevice(g_ControlDevice);
    }

    KdPrint(("CBT: Unload complete\n"));
}