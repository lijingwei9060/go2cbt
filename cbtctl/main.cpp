#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>

/* ============================================================
 * IOCTL 和结构体定义 (必须与驱动 go2cbt.h 完全一致!)
 * ============================================================ */

#define IOCTL_CBT_QUERY    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_CBT_RESET    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

#pragma pack(push, 8)

 /* ---- 输入参数 ---- */
typedef struct _CBT_IOCTL_INPUT {
	ULONG DeviceNumber;        /* 0=Harddisk0, 1=Harddisk1, ... */
} CBT_IOCTL_INPUT, * PCBT_IOCTL_INPUT;

/* ---- Query 输出 (变长结构体) ---- */
typedef struct _CBT_QUERY_OUTPUT {
	ULONGLONG   TotalBits;            /* 位图总 bit 数 (= 总块数) */
	ULONGLONG   TotalBytes;           /* 位图缓冲区字节大小 */
	UCHAR       BitmapData[1];         /* 变长: 位图原始字节流 */
} CBT_QUERY_OUTPUT, * PCBT_QUERY_OUTPUT;

#pragma pack(pop)

/* ============================================================
 * 全局常量
 * ============================================================ */
static const wchar_t* CBT_DEVICE_NAME = L"\\\\.\\CbtMonitor";
static const ULONGLONG CBT_BLOCK_SIZE = 1024 * 1024;  /* 必须与驱动中 CBT_BLOCK_SIZE 一致! */

/* ============================================================
 * 工具函数
 * ============================================================ */

 /**
  * 打开驱动设备句柄
  */
HANDLE OpenCbtDevice(void)
{
	HANDLE hDevice = CreateFileW(
		CBT_DEVICE_NAME,
		GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL
	);

	if (hDevice == INVALID_HANDLE_VALUE) {
		wprintf(L"ERROR: Cannot open device %ls\n", CBT_DEVICE_NAME);
		wprintf(L"  Make sure driver is loaded.\n");
		return INVALID_HANDLE_VALUE;
	}
	return hDevice;
}

void CloseCbtDevice(HANDLE hDevice)
{
	if (hDevice && hDevice != INVALID_HANDLE_VALUE) {
		CloseHandle(hDevice);
	}
}

/* ============================================================
 * 功能: Reset - 重置 CBT 位图
 * ============================================================ */
BOOL ResetCbtBitmap(HANDLE hDevice, ULONG diskNo)
{
	CBT_IOCTL_INPUT input;
	memset(&input, 0, sizeof(input));
	input.DeviceNumber = diskNo;

	DWORD bytesReturned = 0;
	BOOL success = DeviceIoControl(hDevice, IOCTL_CBT_RESET,
		&input, sizeof(input), NULL, 0, &bytesReturned, NULL);

	if (!success) {
		fprintf(stderr, "RESET Disk%lu FAILED (err=%lu)\n", diskNo, GetLastError());
		return FALSE;
	}

	printf("OK: Disk%lu CBT bitmap RESET successfully.\n", diskNo);
	return TRUE;
}

/* ============================================================
 * 功能: Query - 从驱动获取 CBT 位图
 * ============================================================ */
BOOL QueryCbtData(
	HANDLE hDevice,
	ULONG diskNo,
	PCBT_QUERY_OUTPUT* ppOutput,
	DWORD* pOutputSize
)
{
	if (!ppOutput || !pOutputSize) return FALSE;
	*ppOutput = NULL;
	*pOutputSize = 0;

	CBT_IOCTL_INPUT input;
	memset(&input, 0, sizeof(input));
	input.DeviceNumber = diskNo;

	/* ===== 第一步: 用固定小缓冲区获取 TotalBits + TotalBytes ===== */
	// 只需要前 16 字节 (两个 ULONGLONG)，不需要完整位图
	BYTE metaBuf[16];  // 比 FIELD_OFFSET(CBT_QUERY_OUTPUT, BitmapData)=16 大一些，够放头
	memset(metaBuf, 0, sizeof(metaBuf));

	DWORD bytesReturned = 0;
	BOOL ok = DeviceIoControl(hDevice, IOCTL_CBT_QUERY,
		&input, sizeof(input),
		metaBuf, sizeof(metaBuf),   // ← 传入非 NULL 小缓冲区
		&bytesReturned, NULL);

	if (!ok && GetLastError() != ERROR_MORE_DATA) {
		fprintf(stderr, "QUERY Disk%lu FAILED (err=%lu)\n", diskNo, GetLastError());
		return FALSE;
	}

	// 无论成功还是 ERROR_MORE_DATA，metaBuf 里应该已有 TotalBits/TotalBytes
	// （驱动端: 即使缓冲区不足也会先尽量拷贝头部信息）
	PCBT_QUERY_OUTPUT pMeta = (PCBT_QUERY_OUTPUT)metaBuf;
	if (pMeta->TotalBytes == 0 || pMeta->TotalBits == 0) {
		fprintf(stderr, "QUERY Disk%lu: driver returned zero-sized bitmap\n", diskNo);
		return FALSE;
	}


	/* ===== 第二步: 用 TotalBytes 分配精确大小，正式获取完整数据 ===== */
	DWORD requiredSize = (DWORD)(FIELD_OFFSET(CBT_QUERY_OUTPUT, BitmapData) + pMeta->TotalBytes);
	PCBT_QUERY_OUTPUT output = (PCBT_QUERY_OUTPUT)malloc(requiredSize);
	if (!output) {
		fprintf(stderr, "malloc(%lu) failed.\n", requiredSize);
		return FALSE;
	}
	memset(output, 0, requiredSize);

	bytesReturned = 0;
	BOOL success = DeviceIoControl(hDevice, IOCTL_CBT_QUERY,
		&input, sizeof(input), output, requiredSize, &bytesReturned, NULL);

	if (!success) {
		fprintf(stderr, "QUERY Disk%lu failed on 2nd call (err=%lu)\n", diskNo, GetLastError());
		free(output);
		return FALSE;
	}

	*ppOutput = output;
	*pOutputSize = bytesReturned;
	return TRUE;
}

/* ============================================================
 * 格式化辅助: 字节数 → 人类可读字符串 (静态版本, 用于 fprintf)
 * ============================================================ */
char* FormatByteSizeStr(ULONGLONG bytes, _Out_writes_(bufSize) char* buf, int bufSize)
{
	if (bufSize < 32) { buf[0] = '\0'; return buf; }
	const char* units[] = { "B", "KB", "MB", "GB", "TB", "PB" };
	int u = 0;
	double sz = (double)bytes;
	while (sz >= 1024.0 && u < 5) { sz /= 1024.0; u++; }
	snprintf(buf, bufSize, "%6.2f %s", sz, units[u]);
	return buf;
}

const char* FormatByteSizeStatic(ULONGLONG bytes)
{
	static char buf[32];
	FormatByteSizeStr(bytes, buf, sizeof(buf));
	return buf;
}


/* ============================================================
 * 功能: Dump - 可视化展示位图
 *
 * 显示格式:
 *   每行 64 bit (8 组, 每组 8 bit)
 *   '0' = 未变更   '*' = 已变更
 *   行首标注该行对应的起始块号和偏移地址
 *
 * 示例:
 *   Block[000000] Off=0x0000000000000000: 00000*** ****0*** ******** ********
 *   Block[000064] Off=0x0000000000040000: ******** ******** ******** ***00000
 * ============================================================ */
void DumpCbtBitmap(PCBT_QUERY_OUTPUT output, FILE* fpOut)
{
	ULONGLONG totalBits = output->TotalBits;
	const UCHAR* data = output->BitmapData;
	ULONGLONG totalBytes = output->TotalBytes;

	/* ----- 统计信息收集 ----- */
	ULONGLONG changedBlocks = 0;      /* 已变更的块总数 */
	ULONGLONG changedRanges = 0;      /* 连续已变更区间数 */
	ULONGLONG maxRunLength = 0;       /* 最长连续已变更区间长度 */
	ULONGLONG currentRunStart = 0;    /* 当前连续区间的起始位置 */
	BOOL inChangedRun = FALSE;        /* 是否正在扫描一个已变更区间 */

	fprintf(fpOut, "================================================================================\n");
	fprintf(fpOut, "                    CBT BITMAP VISUALIZATION - Disk Report\n");
	fprintf(fpOut, "================================================================================\n");
	fprintf(fpOut, "\n");
	fprintf(fpOut, "  Total bits (blocks): %llu\n", totalBits);
	fprintf(fpOut, "  Block size:          %llu KB (%llu bytes)\n", CBT_BLOCK_SIZE / 1024, CBT_BLOCK_SIZE);
	fprintf(fpOut, "  Bitmap data size:    %llu bytes\n", totalBytes);
	fprintf(fpOut, "  Disk approx size:    %s\n", FormatByteSizeStatic(totalBits * CBT_BLOCK_SIZE));
	fprintf(fpOut, "\n");
	fprintf(fpOut, "  Legend: '0' = unchanged    '*' = changed\n");
	fprintf(fpOut, "  Each row = 64 blocks (%llu KB)\n", 64ULL * CBT_BLOCK_SIZE / 1024);
	fprintf(fpOut, "  Each group of 8 chars = 8 blocks (8 KB for 1MB block size)\n");
	fprintf(fpOut, "\n");

	/* ----- 主循环: 逐 bit 扫描并显示 ----- */
	for (ULONGLONG i = 0; i < totalBits; ) {
		/* 每行开始: 打印行号和偏移地址 */
		ULONGLONG startBlockOfRow = i;
		ULONGLONG offsetOfRow = i * CBT_BLOCK_SIZE;

		fprintf(fpOut, "  [%010llu] 0x%016llx: ", startBlockOfRow, offsetOfRow);

		/* 一行输出 64 bit = 8 组 × 8 bit */
		for (int group = 0; group < 8 && i < totalBits; group++) {
			if (group > 0) {
				fputc(' ', fpOut);   /* 组间空格 */
			}

			/* 一组输出 8 bit */
			for (int b = 0; b < 8 && i < totalBits; b++, i++) {
				/* 计算第 i 个 bit 所在的字节和字节内的位偏移 */
				ULONGLONG byteIndex = i / 8;
				int bitInByte = 7 - (i % 8);   /* MSB first: 高位在前 */

				if (byteIndex >= totalBytes) break;

				BOOL isSet = (data[byteIndex] >> bitInByte) & 1;

				if (isSet) {
					fputc('*', fpOut);
					changedBlocks++;

					/* 更新连续区间追踪 */
					if (!inChangedRun) {
						inChangedRun = TRUE;
						currentRunStart = i;
					}
					/* else: 继续当前 run */
				}
				else {
					fputc('0', fpOut);

					/* 当前 run 结束? */
					if (inChangedRun) {
						ULONGLONG runLen = i - currentRunStart;
						changedRanges++;
						if (runLen > maxRunLength) {
							maxRunLength = runLen;
						}
						inChangedRun = FALSE;
					}
				}
			}
		}

		fputc('\n', fpOut);
	}

	/* 处理最后一行的末尾可能未闭合的 run */
	if (inChangedRun) {
		ULONGLONG runLen = totalBits - currentRunStart;
		changedRanges++;
		if (runLen > maxRunLength) {
			maxRunLength = runLen;
		}
	}

	/* ----- 摘要统计 ----- */
	double changePct = (totalBits > 0) ? (double)changedBlocks / (double)totalBits * 100.0 : 0.0;
	char bufChanged[32], bufMaxRun[32];

	fprintf(fpOut, "\n");
	fprintf(fpOut, "================================================================================\n");
	fprintf(fpOut, "                              SUMMARY STATISTICS\n");
	fprintf(fpOut, "================================================================================\n");
	fprintf(fpOut, "\n");
	fprintf(fpOut, "  Changed Blocks:       %12llu  / %llu  (%.4f%%)\n",
		changedBlocks, totalBits, changePct);
	fprintf(fpOut, "  Unchanged Blocks:     %12llu  / %llu  (%.4f%%)\n",
		totalBits - changedBlocks, totalBits, 100.0 - changePct);
	fprintf(fpOut, "  Changed Data Size:    %-20s\n", FormatByteSizeStr(changedBlocks * CBT_BLOCK_SIZE, bufChanged, sizeof(bufChanged)));
	fprintf(fpOut, "  Changed Range Count:  %12llu  contiguous region(s)\n", changedRanges);
	if (changedRanges > 0) {
		fprintf(fpOut, "  Max Contiguous Run:   %-20s  (%llu blocks)\n",
			FormatByteSizeStr(maxRunLength * CBT_BLOCK_SIZE, bufMaxRun, sizeof(bufMaxRun)), maxRunLength);
		fprintf(fpOut, "  Avg Run Length:       %.2f blocks\n", (double)changedBlocks / (double)changedRanges);
	}
	else {
		fprintf(fpOut, "  Max Contiguous Run:   N/A (no changes)\n");
	}
	fprintf(fpOut, "\n");
	fprintf(fpOut, "  Total Rows Printed:    %llu  (each row = 64 blocks)\n", (totalBits + 63) / 64);
	fprintf(fpOut, "================================================================================\n");


	/* ----- 密度直方图 (横向条形图) ----- */
	/*
	 * 将位图分为 64 段, 统计每段的变更密度, 用字符条形图可视化
	 */
	fprintf(fpOut, "\n");
	fprintf(fpOut, "  CHANGE DENSITY MAP (64 segments, each = 1.5625%% of disk):\n");
	fprintf(fpOut, "  ");

	ULONGLONG segSize = (totalBits + 63) / 64;  /* 每段的块数 */
	for (int seg = 0; seg < 64; seg++) {
		ULONGLONG segStart = (ULONGLONG)seg * segSize;
		ULONGLONG segEnd = segStart + segSize;
		if (segEnd > totalBits) segEnd = totalBits;
		if (segStart >= totalBits) break;

		/* 统计此段中的 set bit 数 */
		ULONGLONG segChanged = 0;
		for (ULONGLONG j = segStart; j < segEnd; j++) {
			ULONGLONG byteIdx = j / 8;
			int bitIdx = 7 - (j % 8);
			if (byteIdx < totalBytes && ((data[byteIdx] >> bitIdx) & 1)) {
				segChanged++;
			}
		}

		double density = (segEnd > segStart) ? (double)segChanged / (double)(segEnd - segStart) : 0.0;

		/* 根据密度选择字符 */
		char ch;
		if (density == 0.0)       ch = ' ';      /* 空: 无变更 */
		else if (density < 0.01)  ch = '.';      /* 极稀疏 */
		else if (density < 0.05)  ch = ':';      /* 稀疏 */
		else if (density < 0.15)  ch = 'o';      /* 中等 */
		else if (density < 0.30)  ch = 'O';      /* 较密 */
		else if (density < 0.50)  ch = '#';      /* 密集 */
		else                      ch = '@';      /* 极密集/满 */

		fputc(ch, fpOut);
	}
	fputc('\n', fpOut);

	fprintf(fpOut, "  ");
	for (int s = 0; s < 64; s += 8) fputc('+', fpOut);
	fputc('\n', fpOut);
	fprintf(fpOut, "  Legend: .=<1%%  :=<5%%  o=<15%%  O=<30%%  #=<50%%  @=>50%%\n");
	fprintf(fpOut, "\n");
}




/* ============================================================
 * main - 命令行入口
 * ============================================================ */
void PrintUsage(const char* prog)
{
	printf("go2cbt CBT Control Tool\n\n");
	printf("Usage:\n");
	printf("  %s query  <diskno>              Query CBT bitmap summary\n", prog);
	printf("  %s reset  <diskno>              Reset CBT bitmap (new snapshot cycle)\n", prog);
	printf("  %s dump   <diskno> [outfile.txt] Visualize bitmap as ASCII art\n\n", prog);
	printf("Examples:\n");
	printf("  %s query  0                     Show Harddisk0 CBT status\n", prog);
	printf("  %s reset  0                     Reset Harddisk0 after backup\n", prog);
	printf("  %s dump   0                     Print bitmap to screen\n", prog);
	printf("  %s dump   0 changes.txt         Save to file\n", prog);
}

int main(int argc, char* argv[])
{
	if (argc < 3) {
		PrintUsage(argv[0]);
		return 1;
	}

	const char* cmd = argv[1];
	ULONG diskNo = (ULONG)atol(argv[2]);

	HANDLE hDevice = OpenCbtDevice();
	if (hDevice == INVALID_HANDLE_VALUE) return 1;

	int ret = 0;

	if (_stricmp(cmd, "reset") == 0) {
		ret = ResetCbtBitmap(hDevice, diskNo) ? 0 : 2;
	}
	else if (_stricmp(cmd, "query") == 0) {
		PCBT_QUERY_OUTPUT output = NULL;
		DWORD outSize = 0;

		if (QueryCbtData(hDevice, diskNo, &output, &outSize)) {
			if (output && output->TotalBits > 0) {
				printf("=== Disk%lu CBT Summary ===\n", diskNo);
				printf("  Total Bits (blocks):  %llu\n", output->TotalBits);
				printf("  Bitmap Bytes:         %llu\n", output->TotalBytes);
				printf("  Block Size:            %llu KB\n", CBT_BLOCK_SIZE / 1024);
				printf("  Disk Approx Size:      %s\n",
					FormatByteSizeStatic(output->TotalBits * CBT_BLOCK_SIZE));

				/* 快速估算变更率: 只看前几个字节 */
				ULONGLONG quickChanged = 0;
				ULONGLONG scanBits = output->TotalBits;
				if (scanBits > 10000) scanBits = 10000;  /* 只扫前 10K bit 估测 */
				for (ULONGLONG i = 0; i < scanBits; i++) {
					if ((output->BitmapData[i / 8] >> (7 - (i % 8))) & 1) {
						quickChanged++;
					}
				}
				double estPct = (double)quickChanged / (double)scanBits * 100.0;
				printf("  Est. Change Rate:      %.2f%% (sampled first %llu blocks)\n",
					estPct, scanBits);
				printf("\nUse 'dump' command for full visualization and exact statistics.\n");
			}
			else {
				printf("Disk%lu: No CBT data available.\n", diskNo);
			}
			if (output) free(output);
		}
		else ret = 3;
	}
	else if (_stricmp(cmd, "dump") == 0) {
		PCBT_QUERY_OUTPUT output = NULL;
		DWORD outSize = 0;

		if (QueryCbtData(hDevice, diskNo, &output, &outSize)) {
			FILE* fpOut = stdout;
			if (argc >= 4) {
				errno_t e = fopen_s(&fpOut, argv[3], "w");
				if (e || !fpOut) {
					fprintf(stderr, "Cannot open '%s'\n", argv[3]);
					free(output); CloseCbtDevice(hDevice);
					return 4;
				}
				printf("Writing to '%s'...\n", argv[3]);
			}

			DumpCbtBitmap(output, fpOut);

			if (fpOut != stdout) {
				fclose(fpOut);
				printf("Done. Output written to '%s'.\n", argv[3]);
			}
			if (output) free(output);
		}
		else ret = 5;
	}
	else {
		fprintf(stderr, "Unknown command: '%s'\n\n", cmd);
		PrintUsage(argv[0]);
		ret = 6;
	}

	CloseCbtDevice(hDevice);
	return ret;
}