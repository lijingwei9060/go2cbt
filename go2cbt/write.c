#include "go2cbt.h"

static LONG g_WriteDebugCount = 0;
// ========================================
// HwReadWrite - IRP 拦截与 CBT 记录
// ========================================
NTSTATUS HwReadWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);

    // ---- IRP 内容验证 ----
    if (irpSp->MajorFunction == IRP_MJ_WRITE) {
        LARGE_INTEGER byteOffset = irpSp->Parameters.Write.ByteOffset;
        ULONG byteCount = irpSp->Parameters.Write.Length;

        // 统计
        g_DevExt->WriteIrpsReceived++;

        if (InterlockedIncrement(&g_WriteDebugCount) <= 10)
        {
            KdPrint(("CBT: WRITE received offset=%lld length=%lu\n",
                byteOffset,
                byteCount
                ));

        }
    }

    // ---- 透传: 调用原始磁盘驱动的写处理函数 ----
    // 这是 Hook 的核心: 我们拦截了 IRP, 记录了变更, 然后原样传递
    return g_DevExt->OriginalWrite(DeviceObject, Irp);
}