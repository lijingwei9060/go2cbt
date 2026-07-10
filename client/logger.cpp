/* ============================================================
 * 日志模块实现 (logger.cpp)
 *
 * 设计要点:
 *   - 零依赖: 只用 Windows 原生 API + CRT
 *   - 线程安全: CRITICAL_SECTION 保护共享状态
 *   - 性能:
 *       编译期级别过滤 (LOGGER_MIN_LEVEL) → 不匹配的宏展开为 (void)0
 *       运行时级别检查在锁外进行, 大多数情况无需获取锁
 * ============================================================ */

#include "logger.h"
#include <stdarg.h>
#include <string.h>

/* ---- 日志级别对应的控制台颜色 ---- */

/*
 * Windows Console Color:
 *   Foreground: 0x0-0x7  (黑/深蓝/深绿/深青/深红/深紫/黄/浅灰)
 *   Foreground bright: 0x8-0xF  (暗灰/亮蓝/亮绿/亮青/亮红/亮紫/亮白)
 *   Background: 0x00-0x70 (同理, 高4位是背景色)
 *
 * FOREGROUND_* 宏定义:
 *   0x1 RED, 0x2 GREEN, 0x4 BLUE,
 *   0x8 INTENSITY (加亮), 0x80 BACKGROUND bit
 */
typedef struct _LEVEL_COLOR {
	WORD attr;          /* SetConsoleTextAttribute 参数 */
	const char* label;  /* 级别标签 */
} LEVEL_COLOR;

static const LEVEL_COLOR g_levelColors[] = {
	{ 0x08, "TRACE" },    /* Dark gray — 最不显眼 */
	{ 0x07, "DEBUG" },    /* White — 正常亮度 */
	{ 0x0A, "INFO " },    /* Green — 积极、正常 */
	{ 0x0E, "WARN " },    /* Yellow — 警告但非致命 */
	{ 0x0C, "ERROR" },    /* Red — 错误，必须关注 */
};
#define COLOR_DEFAULT  0x07  /* 默认控制台颜色: 白字黑底 */

/* ---- 全局状态 ---- */

static struct {
	CRITICAL_SECTION cs;      /* 保护以下所有字段 */
	LOG_LEVEL consoleLevel;   /* 控制台最低级别 */
	LOG_LEVEL fileLevel;      /* 文件最低级别 */
	BOOL colorEnabled;
	BOOL timestampEnabled;
	BOOL sourceLocation;
	FILE* fpLog;              /* 日志文件句柄 */
	char logFilePath[260];    /* 文件路径副本 */
	HANDLE hConsoleStdout;    /* stdout 控制台句柄 (用于颜色) */
	WORD defaultAttr;         /* 默认控制台属性 (用于恢复) */
	BOOL initialized;
} g_logger = { 0 };

/* ================================================================
 * 内部辅助函数
 * ================================================================ */

/**
 * 提取文件名 (去掉路径前缀)
 *
 * 输入: "E:\\project\\src\\main.c"
 * 输出: "main.c"
 */
static const char* ExtractFileName(const char* path)
{
	if (!path) return "(null)";
	const char* p = path + strlen(path);
	while (p > path && *(p - 1) != '\\' && *(p - 1) != '/') {
		p--;
	}
	return p;
}

/**
 * 格式化当前时间为字符串
 *
 * 格式: "2026-07-10 12:30:45.123"
 * 缓冲区需 >= 24 字节
 */
static void FormatTimestamp(char* buf, int bufSize)
{
	SYSTEMTIME st;
	GetLocalTime(&st);
	_snprintf_s(buf, bufSize, bufSize - 1,
		"%04d-%02d-%02d %02d:%02d:%02d.%03d",
		st.wYear, st.wMonth, st.wDay,
		st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

/**
 * 设置控制台颜色 (带缓存优化)
 */
static void SetConsoleColor(WORD attr)
{
	if (!g_logger.hConsoleStdout) return;

	static WORD lastAttr = 0xFFFF;  /* 缓存上次颜色 */
	if (lastAttr == attr) return;     /* 相同则跳过系统调用 */

	SetConsoleTextAttribute(g_logger.hConsoleStdout, attr);
	lastAttr = attr;
}

/**
 * 恢复控制台默认颜色
 */
static void RestoreConsoleColor(void)
{
	SetConsoleColor(g_logger.defaultAttr);

	/* 强制重置缓存, 下次一定调用 API */
	static WORD* pLast = NULL;
	if (!pLast) {
		/* 用一个技巧: 取 static 变量地址 */
		WORD fake = 0xFFFF;
		pLast = &fake;
	}
	*pLast = 0xFFFE;  /* 使缓存失效 */
}

/* ================================================================
 * 公开接口实现
 * ================================================================ */

void LoggerInit(_In_opt_z_ const char* logFile, LOG_LEVEL level)
{
	InitializeCriticalSection(&g_logger.cs);

	g_logger.consoleLevel = LOG_LEVEL_INFO;   /* 默认: 控制台 INFO 及以上 */
	g_logger.fileLevel = level;               /* 用户指定 */
	g_logger.colorEnabled = TRUE;
	g_logger.timestampEnabled = TRUE;
	g_logger.sourceLocation = TRUE;
	g_logger.fpLog = NULL;
	g_logger.initialized = TRUE;

	/* 获取 stdout 的控制台句柄 (用于彩色输出) */
	g_logger.hConsoleStdout = GetStdHandle(STD_OUTPUT_HANDLE);
	if (g_logger.hConsoleStdout != INVALID_HANDLE_VALUE &&
		g_logger.hConsoleStdout != NULL) {
		CONSOLE_SCREEN_BUFFER_INFO csbi;
		if (GetConsoleScreenBufferInfo(g_logger.hConsoleStdout, &csbi)) {
			g_logger.defaultAttr = csbi.wAttributes;
		}
	}

	/* 打开日志文件 */
	if (logFile && level != LOG_LEVEL_NONE) {
		strncpy_s(g_logger.logFilePath, sizeof(g_logger.logFilePath),
			logFile, _TRUNCATE);
		errno_t err = fopen_s(&g_logger.fpLog, g_logger.logFilePath, "a");
		if (err || !g_logger.fpLog) {
			/* 文件打开失败不致命, 继续用控制台输出 */
			g_logger.fpLog = NULL;
			g_logger.fileLevel = LOG_LEVEL_NONE;
		}
	}
}

void LoggerCleanup(void)
{
	if (!g_logger.initialized) return;

	EnterCriticalSection(&g_logger.cs);

	if (g_logger.fpLog) {
		fflush(g_logger.fpLog);
		fclose(g_logger.fpLog);
		g_logger.fpLog = NULL;
	}

	LeaveCriticalSection(&g_logger.cs);
	DeleteCriticalSection(&g_logger.cs);

	RtlZeroMemory(&g_logger, sizeof(g_logger));
}

void LoggerSetConsoleLevel(LOG_LEVEL level)
{
	if (g_logger.initialized) {
		EnterCriticalSection(&g_logger.cs);
		g_logger.consoleLevel = level;
		LeaveCriticalSection(&g_logger.cs);
	}
}

void LoggerSetFileLevel(LOG_LEVEL level)
{
	if (g_logger.initialized) {
		EnterCriticalSection(&g_logger.cs);
		g_logger.fileLevel = level;
		LeaveCriticalSection(&g_logger.cs);
	}
}

LOG_LEVEL LoggerGetConsoleLevel(void)
{
	if (!g_logger.initialized) return LOG_LEVEL_INFO;
	EnterCriticalSection(&g_logger.cs);
	LOG_LEVEL lvl = g_logger.consoleLevel;
	LeaveCriticalSection(&g_logger.cs);
	return lvl;
}

/* ================================================================
 * 核心日志输出
 * ================================================================ */

void LogWrite(
	_In_ LOG_LEVEL level,
	_In_z_ const char* file,
	_In_ int line,
	_In_z_ const char* func,
	_In_z_ const char* fmt,
	...
)
{
	/* 运行时级别快速检查 (无锁) */
	if ((level < g_logger.consoleLevel) && (level < g_logger.fileLevel)) {
		return;
	}

	/* ---- 格式化用户消息 ---- */
	char msgBuf[2048];
	va_list args;
	va_start(args, fmt);
	int msgLen = vsnprintf_s(msgBuf, sizeof(msgBuf), _TRUNCATE, fmt, args);
	va_end(args);
	if (msgLen < 0) msgLen = 0;

	/* ---- 构造完整日志行 ---- */
	char fullLine[2304];   /* 头部 ~200 字节 + 消息 2048 */
	int pos = 0;

	/* [时间戳] */
	if (g_logger.timestampEnabled) {
		char ts[32] = { 0 };
		FormatTimestamp(ts, sizeof(ts));
		pos += snprintf(fullLine + pos, sizeof(fullLine) - pos, "[%s] ", ts);
	}

	/* [级别标签] */
	if (level >= LOG_LEVEL_TRACE && level <= LOG_LEVEL_ERROR) {
		pos += snprintf(fullLine + pos, sizeof(fullLine) - pos, "%-5s ",
			g_levelColors[level].label);
	}

	/* [源码位置] */
	if (g_logger.sourceLocation && file && func) {
		pos += snprintf(fullLine + pos, sizeof(fullLine) - pos, "[%s:%d %s] ",
			ExtractFileName(file), line, func);
	}

	/* 消息正文 */
	pos += snprintf(fullLine + pos, sizeof(fullLine) - pos, "%s", msgBuf);

	/* 确保换行 */
	if (pos > 0 && fullLine[pos - 1] != '\n') {
		fullLine[pos++] = '\n';
		fullLine[pos] = '\0';
	}

	/* ---- 加锁后输出 ---- */
	EnterCriticalSection(&g_logger.cs);

	/* 控制台输出 */
	if (level >= g_logger.consoleLevel && level <= LOG_LEVEL_ERROR) {
		if (g_logger.colorEnabled && g_logger.hConsoleStdout) {
			SetConsoleColor(g_levelColors[level].attr);
		}
		fwrite(fullLine, 1, pos, stdout);
		fflush(stdout);   /* 实时输出, 不等缓冲 */
		if (g_logger.colorEnabled) {
			RestoreConsoleColor();
		}
	}

	/* 文件输出 (纯文本, 无颜色代码) */
	if (level >= g_logger.fileLevel && g_logger.fpLog) {
		fwrite(fullLine, 1, pos, g_logger.fpLog);
		fflush(g_logger.fpLog);
	}

	LeaveCriticalSection(&g_logger.cs);
}
