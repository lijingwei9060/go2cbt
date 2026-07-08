#include "go2cbt.h"

// ========================================
// IOCTL 处理 - 用户态查询接口
// ========================================
NTSTATUS CbtDispatchDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}
