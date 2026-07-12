// VolumeMapper::GetVolumeDiskExtents 独立诊断程序
// 对每个卷 GUID 逐步调用 API，打印每一步的结果和错误码
// 用法: test_VolumeMapper_diag.exe
//       或 test_VolumeMapper_diag.exe <volume_guid>   (只测试指定卷)

#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <string>
#include <cstdarg>

// 从 VolumeMapper.cpp 复制 GetDriveLetter 逻辑
void GetDriveLetter(const wchar_t* volumeGuid, wchar_t* outDrive, size_t outSize)
{
	outDrive[0] = L'\0';
	wchar_t pathNames[MAX_PATH * 4] = {};
	DWORD pathLen = 0;

	BOOL result = GetVolumePathNamesForVolumeNameW(
		volumeGuid, pathNames, ARRAYSIZE(pathNames), &pathLen);

	if (result && pathNames[0] != L'\0')
	{
		// 取第一个路径，去掉末尾反斜杠: "C:\" → "C:"
		wcscpy_s(outDrive, outSize, pathNames);
		size_t len = wcslen(outDrive);
		if (len > 0 && outDrive[len - 1] == L'\\')
			outDrive[len - 1] = L'\0';
	}
}

void Log(const wchar_t* fmt, ...)
{
	SYSTEMTIME st;
	GetLocalTime(&st);
	wprintf(L"[%02d:%02d:%02d.%03d] ",
		st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
	va_list args;
	va_start(args, fmt);
	vwprintf(fmt, args);
	va_end(args);
	wprintf(L"\n");
	fflush(stdout);
}

void TestVolume(const wchar_t* volumeGuid)
{
	Log(L"========================================");
	Log(L"Testing: %s", volumeGuid);
	Log(L"========================================");

	// Step 1: GetDriveLetter
	wchar_t driveLetter[16] = {};
	Log(L"Step 1: GetVolumePathNamesForVolumeNameW...");
	GetDriveLetter(volumeGuid, driveLetter, 16);
	Log(L"  Result: drive letter = \"%s\" %s",
		driveLetter[0] ? driveLetter : L"(none)",
		driveLetter[0] ? L"" : L"→ WILL SKIP (no drive letter)");
	fflush(stdout);

	if (driveLetter[0] == L'\0')
	{
		Log(L"  SKIPPED: no drive letter, not a backup target");
		Log(L"");
		return;
	}

	// Step 2: 去掉末尾反斜杠
	WCHAR devicePath[MAX_PATH]; wcscpy_s(devicePath, volumeGuid);
	size_t dpLen = wcslen(devicePath);
	if (dpLen > 0 && devicePath[dpLen-1] == L'\\')
		devicePath[dpLen-1] = L'\0';
	Log(L"Step 2: devicePath = %s", devicePath);
	fflush(stdout);

	// Step 3: CreateFileW
	Log(L"Step 3: CreateFileW (access=0, share=RW)...");
	fflush(stdout);
	HANDLE hVolume = CreateFileW(
		devicePath,
		0,                           // 0 = 仅查询
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, OPEN_EXISTING, 0, NULL);
	DWORD createErr = GetLastError();

	if (hVolume == INVALID_HANDLE_VALUE)
	{
		Log(L"  FAILED: error=%lu (0x%08X)", createErr, createErr);
		if (createErr == ERROR_ACCESS_DENIED)
			Log(L"  → ACCESS_DENIED (needs admin)");
		else if (createErr == ERROR_FILE_NOT_FOUND)
			Log(L"  → FILE_NOT_FOUND");
		else if (createErr == ERROR_SHARING_VIOLATION)
			Log(L"  → SHARING_VIOLATION (volume locked)");
		Log(L"");
		return;
	}
	Log(L"  OK: handle=0x%p", hVolume);
	fflush(stdout);

	// Step 4: DeviceIoControl
	Log(L"Step 4: DeviceIoControl(IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS)...");
	fflush(stdout);
	BYTE buffer[sizeof(VOLUME_DISK_EXTENTS) + 8 * sizeof(DISK_EXTENT)] = {};
	DWORD bytesReturned = 0;

	BOOL ioctlResult = DeviceIoControl(
		hVolume,
		IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
		NULL, 0,
		buffer, sizeof(buffer),
		&bytesReturned,
		NULL);
	DWORD ioctlErr = GetLastError();

	if (!ioctlResult)
	{
		Log(L"  FAILED: error=%lu (0x%08X)", ioctlErr, ioctlErr);
		CloseHandle(hVolume);
		Log(L"");
		return;
	}
	Log(L"  OK: bytesReturned=%lu", bytesReturned);

	// Step 5: Parse extents
	PVOLUME_DISK_EXTENTS extents = reinterpret_cast<PVOLUME_DISK_EXTENTS>(buffer);
	Log(L"Step 5: NumberOfDiskExtents = %lu", extents->NumberOfDiskExtents);

	for (DWORD i = 0; i < extents->NumberOfDiskExtents && i < 8; i++)
	{
		PDISK_EXTENT extent = &extents->Extents[i];
		Log(L"  Extent[%lu]: DiskNumber=%lu, StartingOffset=0x%llx, ExtentLength=%llu",
			i, extent->DiskNumber,
			extent->StartingOffset.QuadPart,
			extent->ExtentLength.QuadPart);
	}

	// Step 6: CloseHandle
	CloseHandle(hVolume);
	Log(L"Step 6: CloseHandle done");
	Log(L"");
	fflush(stdout);
}

int wmain(int argc, wchar_t* argv[])
{
	Log(L"=== VolumeMapper::GetVolumeDiskExtents Diagnostic Tool ===");
	Log(L"");

	if (argc >= 2)
	{
		// 测试指定卷 GUID
		TestVolume(argv[1]);
	}
	else
	{
		// 枚举所有卷
		wchar_t volumeName[MAX_PATH] = {};
		HANDLE hFind = FindFirstVolumeW(volumeName, ARRAYSIZE(volumeName));

		if (hFind == INVALID_HANDLE_VALUE)
		{
			Log(L"FindFirstVolumeW failed, error=%lu", GetLastError());
			return 1;
		}

		int count = 0;
		do
		{
			count++;
			TestVolume(volumeName);
		} while (FindNextVolumeW(hFind, volumeName, ARRAYSIZE(volumeName)));

		FindVolumeClose(hFind);
		Log(L"=== DONE: %d volume(s) tested ===", count);
	}

	return 0;
}
