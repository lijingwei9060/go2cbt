#pragma once

/* ============================================================
 * firmware.h  -  固件引导模式检测 (UEFI / Legacy BIOS)
 *
 * 功能:
 *   检测当前系统固件引导模式, 输出 "UEFI" 或 "Legacy"
 *
 * 用法:
 *   #include "firmware.h"
 *   CbtFwType t = CbtDetectFirmware();
 *   printf("%s\n", CbtFwToString(t));
 *
 * 注意:
 *   - 本模块所有公开符号均以 CbtFw / CBT_FW_ 开头,
 *     避免与 <windows.h> 的 FIRMWARE_TYPE / FirmwareTypeUefi 等冲突.
 *   - 线程安全, 无副作用 (内部权限修改会自动恢复).
 * ============================================================ */

#ifdef __cplusplus
extern "C" {
#endif

/** 固件类型 */
typedef enum _CbtFwType {
	CBT_FW_UNKNOWN = 0,   /* 无法确定 */
	CBT_FW_UEFI   = 1,    /* UEFI 固件 */
	CBT_FW_LEGACY = 2     /* Legacy BIOS (MBR 引导) */
} CbtFwType;

/**
 * 检测当前系统固件引导模式
 *
 * @return  CBT_FW_UEFI / CBT_FW_LEGACY / CBT_FW_UNKNOWN
 */
CbtFwType CbtDetectFirmware(void);

/**
 * 枚举值转可打印字符串
 *
 * @return  "UEFI" / "Legacy" / "Unknown"  (常量指针, 无需 free)
 */
const char* CbtFwToString(CbtFwType type);

#ifdef __cplusplus
}
#endif
