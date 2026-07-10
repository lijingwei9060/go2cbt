/* ============================================================
 * firmware.cpp  -  固件引导模式检测实现
 *
 * 检测策略 (按优先级):
 *   A. GetFirmwareType()               Windows 8+ 原生 API
 *   B. GetFirmwareEnvironmentVariableA  探测 UEFI 命名空间 (回退)
 * ============================================================ */

#include "firmware.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* ================================================================
 * 内部: Windows SDK 的 FIRMWARE_TYPE 枚举值
 *
 * 不直接引用 <windows.h> 的同名枚举, 用独立常量做运行时映射,
 * 彻底避免 C2365 / C2371 重定义错误.
 * ================================================================ */
static const int WIN_FW_UEFI   = 2;  /* FirmwareTypeUefi  */
static const int WIN_FW_BIOS   = 1;  /* FirmwareTypeBios   */
static const int WIN_FW_UNKNOWN = 0; /* FirmwareTypeUnknown */

typedef VOID(WINAPI* PfnGetFirmwareType)(_Out_ DWORD* pFirmwareType);

/* ================================================================
 * 内部: 尝试启用 SeSystemEnvironmentPrivilege
 *
 * GetFirmwareEnvironmentVariableA 需要此权限.
 * 失败不致命 - 仅导致回退方法不可用.
 * ================================================================ */
static BOOL EnableSystemEnvPriv(void)
{
	HANDLE hToken;
	if (!OpenProcessToken(GetCurrentProcess(),
		TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
		return FALSE;

	TOKEN_PRIVILEGES tp = { .PrivilegeCount = 1 };
	tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

	if (!LookupPrivilegeValueW(NULL, L"SeSystemEnvironmentPrivilege",
		&tp.Privileges[0].Luid)) {
		CloseHandle(hToken);
		return FALSE;
	}

	BOOL ok = AdjustTokenPrivileges(hToken, FALSE, &tp, 0, NULL, NULL);
	CloseHandle(hToken);

	/* 权限不存在(非管理员) 也算失败 */
	return ok && GetLastError() != ERROR_NOT_ALL_ASSIGNED;
}

/* ================================================================
 * 方法 B: 通过 UEFI 环境变量命名空间探测
 *
 * 原理:
 *   UEFI 固件拥有全局 GUID 命名空间 {8BE4DF61-93CA-...}.
 *   Legacy BIOS 不存在此命名空间.
 *   读一个 dummy 变量名:
 *     返回 > 0          => UEFI
 *     ERROR_INVALID_FUNCTION => Legacy
 *     其他错误          => Unknown
 * ================================================================ */
static CbtFwType ProbeViaEnvVar(void)
{
	static const char GUID[] =
		"{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}";
	char buf[4] = { 0 };

	DWORD ret = GetFirmwareEnvironmentVariableA("", GUID, buf, sizeof(buf));
	if (ret > 0) return CBT_FW_UEFI;

	DWORD err = GetLastError();
	if (err == ERROR_INVALID_FUNCTION || err == ERROR_BAD_ENVIRONMENT)
		return CBT_FW_LEGACY;

	return CBT_FW_UNKNOWN;
}

/* ================================================================
 * 公开接口: CbtDetectFirmware
 * ================================================================ */
CbtFwType CbtDetectFirmware(void)
{
	CbtFwType result = CBT_FW_UNKNOWN;

	/* ---- 方法 A: GetFirmwareType() (Windows 8+) ---- */
	HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
	if (hK32) {
		PfnGetFirmwareType pFn =
			(PfnGetFirmwareType)GetProcAddress(hK32, "GetFirmwareType");

		if (pFn) {
			DWORD winVal = WIN_FW_UNKNOWN;
			pFn(&winVal);

			switch (winVal) {
			case WIN_FW_UEFI:   result = CBT_FW_UEFI;   break;
			case WIN_FW_BIOS:   result = CBT_FW_LEGACY;  break;
			default:            result = CBT_FW_UNKNOWN; break;
			}

			if (result != CBT_FW_UNKNOWN)
				return result;
		}
	}

	/* ---- 方法 B: 回退到环境变量探测 ---- */
	EnableSystemEnvPriv();
	result = ProbeViaEnvVar();
	return result;
}

/* ================================================================
 * 公开接口: CbtFwToString
 * ================================================================ */
const char* CbtFwToString(CbtFwType type)
{
	switch (type) {
	case CBT_FW_UEFI:   return "UEFI";
	case CBT_FW_LEGACY: return "Legacy";
	default:            return "Unknown";
	}
}
