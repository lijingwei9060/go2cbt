#include "go2cbt.h"

// ========================================
// IOCTL 处理 - 用户态查询接口
// ========================================
NTSTATUS CbtDispatchDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
	UNREFERENCED_PARAMETER(DeviceObject);

	PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
	NTSTATUS status;
	ULONG bytesReturned = 0;

	switch (irpSp->Parameters.DeviceIoControl.IoControlCode) {

	case IOCTL_QUERY_CBT_DATA:
		status = HandleIoctlQuery(Irp, irpSp, &bytesReturned);
		break;

	case IOCTL_RESET_CBT_DATA:
		status = HandleIoctlReset(Irp, irpSp, &bytesReturned);
		break;

	default:
		status = STATUS_INVALID_DEVICE_REQUEST;
		break;
	}

	Irp->IoStatus.Status = status;
	Irp->IoStatus.Information = bytesReturned;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);

	return status;
}

// ================================================
// FindDiskMapEntryByNumber - 在 g_DiskMap 中按 DiskNumber 查找条目
//
// 返回: 指向 DISK_MAP_ENTRY 的指针, 或 NULL (未找到)
// ================================================
static PDISK_MAP_ENTRY FindDiskMapEntryByNumber(_In_ ULONG deviceNumber) {
	for (ULONG i = 0; i < g_DiskMapCount; i++) {
		if (g_DiskMap[i].DiskNumber == deviceNumber) {
			return &g_DiskMap[i];
		}
	}
	return NULL;
}

// ================================================
// HandleIoctlQuery - 处理 IOCTL_CBT_QUERY
//
// 用户传入 deviceNumber → 返回对应磁盘的:
//   TotalBits, ChangedBlockCount, 以及完整位图数据
// ================================================
static NTSTATUS HandleIoctlQuery(
	_In_ PIRP Irp,
	_In_ PIO_STACK_LOCATION irpSp,
	_Out_ PULONG pBytesReturned
)
{
	ULONG inLen = irpSp->Parameters.DeviceIoControl.InputBufferLength;
	ULONG outLen = irpSp->Parameters.DeviceIoControl.OutputBufferLength;
	*pBytesReturned = 0;

	// ---- Step 1: 校验输入参数 ----
	if (inLen < sizeof(CBT_IOCTL_INPUT)) {
		return STATUS_INVALID_PARAMETER;    // 缺少输入参数
	}

	PCBT_IOCTL_INPUT input = (PCBT_IOCTL_INPUT)Irp->AssociatedIrp.SystemBuffer;
	ULONG devNo = input->DeviceNumber;

	// ---- Step 2: 查找目标磁盘 ----
	PDISK_MAP_ENTRY mapEntry = FindDiskMapEntryByNumber(devNo);
	if (!mapEntry) {
		KdPrint(("CBT-QUERY: Disk %lu not found in g_DiskMap (count=%lu)\n", devNo, g_DiskMapCount));
		return STATUS_NOT_FOUND;            // 该磁盘未被追踪
	}

	PDISK_CBT_STATE cbtState = &mapEntry->CbtState;

	// ---- Step 3: 检查 CBT 是否已初始化 ----
	if (!cbtState->Buffer || cbtState->TotalBits == 0) {
		KdPrint(("CBT-QUERY: Disk %lu CBT state not initialized\n", devNo));
		return STATUS_INVALID_DEVICE_STATE; // 位图未初始化
	}

	// ---- Step 4: 计算输出所需空间 ----
	ULONGLONG totalBits = cbtState->TotalBits;
	ULONG totalBytes = (ULONG)cbtState->TotalBytes;

	// 输出 = 固定头部(CBT_QUERY_OUTPUT) + 变长位图数据(bitmapByteSize)
	ULONG requiredSize = FIELD_OFFSET(CBT_QUERY_OUTPUT, BitmapData) + totalBytes;

	if (outLen < requiredSize) {
		// ---- 关键改动: 尽量先填充头部信息再报错 ----
	   // 即使缓冲区不足以容纳完整位图,
	   // 只要能放下 CBT_QUERY_OUTPUT 头部 (至少 16 字节),
	   // 就先把 TotalBits / TotalBytes 填进去.
	   // 这样用户态可以从缓冲区直接读到所需大小, 而不依赖 bytesReturned 回填.

		if (outLen >= (ULONG)FIELD_OFFSET(CBT_QUERY_OUTPUT, BitmapData)) {
			// 缓冲区 >= 16 字节: 可以安全写入头部
			PCBT_QUERY_OUTPUT output = (PCBT_QUERY_OUTPUT)Irp->AssociatedIrp.SystemBuffer;
			output->TotalBits = totalBits;
			output->TotalBytes = totalBytes;
			*pBytesReturned = FIELD_OFFSET(CBT_QUERY_OUTPUT, BitmapData);  // 告知实际写了多少
		}
		else {
			// 缓冲区连头部都装不下: 只能纯靠 pBytesReturned 告知大小 (不常见)
			*pBytesReturned = requiredSize;
		}

		KdPrint(("CBT-QUERY: Buffer too small. Need=%lu, Got=%lu, TotalBits=%llu, TotalBytes=%lu\n",
			requiredSize, outLen, totalBits, totalBytes));
		return STATUS_BUFFER_OVERFLOW;
	}

	// ---- Step 5: 填充输出 (在自旋锁保护下读取位图) ----
	PCBT_QUERY_OUTPUT output = (PCBT_QUERY_OUTPUT)Irp->AssociatedIrp.SystemBuffer;

	// 填充固定头部
	output->TotalBits = totalBits;
	output->TotalBytes = totalBytes;

	// ---- 复制位图原始数据 (需要锁保护!) ----
	// 原因: 如果不锁, RlSetBits 可能正在并发修改同一个 ULONG
	// 导致我们读到 "半设置" 的不一致状态
	{
		KIRQL oldIrql= 0;
		KeAcquireSpinLock(&cbtState->Lock, &oldIrql);

		// 直接 memcpy 整个位图缓冲区
		RtlCopyMemory(output->BitmapData, cbtState->Buffer, totalBytes);

		KeReleaseSpinLock(&cbtState->Lock, oldIrql);
	}

	*pBytesReturned = requiredSize;

	KdPrint(("CBT-QUERY: Disk %lu -> TotalBits=%llu BitmapBytes=%lu\n", devNo, totalBits, totalBytes));

	return STATUS_SUCCESS;
}

// ================================================
// HandleIoctlReset - 处理 IOCTL_CBT_RESET
//
// 用户传入 deviceNumber → 将对应磁盘的 CBT 位图全部清零
// (开始一个新的快照周期)
// ================================================
static NTSTATUS HandleIoctlReset(
	_In_ PIRP Irp,
	_In_ PIO_STACK_LOCATION irpSp,
	_Out_ PULONG pBytesReturned
)
{
	ULONG inLen = irpSp->Parameters.DeviceIoControl.InputBufferLength;
	*pBytesReturned = 0;

	// ---- Step 1: 校验输入参数 ----
	if (inLen < sizeof(CBT_IOCTL_INPUT)) {
		return STATUS_INVALID_PARAMETER;
	}

	PCBT_IOCTL_INPUT input = (PCBT_IOCTL_INPUT)Irp->AssociatedIrp.SystemBuffer;
	ULONG devNo = input->DeviceNumber;

	// ---- Step 2: 查找目标磁盘 ----
	PDISK_MAP_ENTRY mapEntry = FindDiskMapEntryByNumber(devNo);
	if (!mapEntry) {
		KdPrint(("CBT-RESET: Disk %lu not found\n", devNo));
		return STATUS_NOT_FOUND;
	}

	PDISK_CBT_STATE cbtState = &mapEntry->CbtState;

	// ---- Step 3: 检查 CBT 是否已初始化 ----
	if (!cbtState->Buffer || cbtState->TotalBits == 0) {
		KdPrint(("CBT-RESET: Disk %lu CBT state not initialized\n", devNo));
		return STATUS_INVALID_DEVICE_STATE;
	}

	// ---- Step 4: 加锁清零位图 ----
	{
		KIRQL oldIrql = 0;
		KeAcquireSpinLock(&cbtState->Lock, &oldIrql);

		// 清零所有 bit (一条指令, 极快)
		RtlClearAllBits(&cbtState->Bitmap);
		KeReleaseSpinLock(&cbtState->Lock, oldIrql);
	}

	KdPrint(("CBT-RESET: Disk %lu bitmap cleared.\n", devNo));

	return STATUS_SUCCESS;
}