#pragma once

#include <windows.h>
#include <stdio.h>

/* ============================================================
 * 日志模块 (logger.h / logger.cpp)
 *
 * 功能:
 *   - 5 级日志 (TRACE / DEBUG / INFO / WARN / ERROR)
 *   - 彩色控制台输出 (Windows Console API)
 *   - 精确到毫秒的时间戳
 *   - 自动记录源码位置 (文件名:行号 函数名)
 *   - 可选同步写入日志文件
 *   - 可配置最低输出级别 (编译期 + 运行期双重过滤)
 *   - 线程安全 (CRITICAL_SECTION)
 *
 * 用法:
 *   #include "logger.h"
 *
 *   LOG_INFO("Connected to %s:%d", host, port);
 *   LOG_ERROR("Read failed: %s (code=%lu)", errMsg, errCode);
 *   LOG_DEBUG("Block[%llu] hash=%016llx", blockNum, hash);
 *
 * 初始化/清理:
 *   LoggerInit("myapp.log", LOG_LEVEL_DEBUG);  // 可选: 同时写文件
 *   LoggerSetLevel(LOG_LEVEL_INFO);            // 运行时调整级别
 *   LoggerCleanup();                            // 程序退出时调用
 * ============================================================ */

/* ---- 日志级别 ---- */
typedef enum _LOG_LEVEL {
	LOG_LEVEL_TRACE = 0,    /* 最详细: 函数入口/出口、循环迭代等 */
	LOG_LEVEL_DEBUG = 1,    /* 调试: 变量值、中间状态 */
	LOG_LEVEL_INFO  = 2,    /* 一般信息: 操作进度、状态变化 */
	LOG_LEVEL_WARN  = 3,    /* 警告: 可恢复的异常情况 */
	LOG_LEVEL_ERROR = 4     /* 错误: 操作失败、需要关注 */
} LOG_LEVEL;

/* ---- 日志配置选项 ---- */
typedef struct _LOGGER_CONFIG {
	LOG_LEVEL       consoleLevel;      /* 控制台输出的最低级别 */
	LOG_LEVEL       fileLevel;         /* 文件输出的最低级别 (LOG_LEVEL_NONE=不写文件) */
	BOOL            colorEnabled;      /* 是否启用彩色输出 */
	BOOL            timestampEnabled;  /* 是否显示时间戳 */
	BOOL            sourceLocation;    /* 是否显示 文件:行号 */
	const char*     filePath;          /* 日志文件路径 (NULL=不写文件) */
} LOGGER_CONFIG;

#define LOG_LEVEL_NONE ((LOG_LEVEL)99)  /* 特殊值: 表示禁用 */

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * 核心接口
 * ================================================================ */

/**
 * 初始化日志系统 (可选, 不调用则使用默认配置)
 *
 * 默认行为:
 *   - 输出到 stdout, 级别 >= LOG_LEVEL_INFO
 *   - 启用颜色和时间戳
 *   - 显示源码位置
 *   - 不写文件
 *
 * @param logFile  日志文件路径 (NULL=不写文件)
 * @param level    文件输出的最低级别 (LOG_LEVEL_NONE=不写文件)
 */
void LoggerInit(_In_opt_z_ const char* logFile, LOG_LEVEL level);

/**
 * 关闭日志系统, 释放资源 (关闭文件句柄等)
 *
 * 通常在程序退出前调用.
 * 不调用也不会泄漏 (进程退出时 OS 会回收),
 * 但显式调用可以确保文件 flush 完整.
 */
void LoggerCleanup(void);

/**
 * 运行时修改控制台输出级别
 */
void LoggerSetConsoleLevel(LOG_LEVEL level);

/**
 * 运行时修改文件输出级别
 */
void LoggerSetFileLevel(LOG_LEVEL level);

/**
 * 获取当前控制台输出级别
 */
LOG_LEVEL LoggerGetConsoleLevel(void);

/* ================================================================
 * 内部核心函数 (通常不直接调用, 用下面的宏即可)
 * ================================================================ */

/**
 * 写一条日志 (内部实现)
 *
 * @param level    日志级别
 * @param file     源文件路径 (__FILE__)
 * @param line     源码行号 (__LINE__)
 * @param func     函数名 (__FUNCTION__)
 * @param fmt      printf 风格格式字符串
 * @param ...      格式参数
 */
void LogWrite(
	_In_ LOG_LEVEL level,
	_In_z_ const char* file,
	_In_ int line,
	_In_z_ const char* func,
	_In_z_ const char* fmt,
	...
);

/* ================================================================
 * 便捷宏 (推荐使用方式)
 *
 * 自动填充 __FILE__, __LINE__, __FUNCTION__
 * 编译期级别过滤: 低于 LOGGER_MIN_LEVEL 的宏展开为空操作 (零开销)
 * ================================================================ */

/*
 * 编译期最低级别: 在包含 logger.h 之前定义可自定义,
 * 默认为 LOG_LEVEL_TRACE (不过滤任何级别).
 * 发布版本可定义为 LOG_LEVEL_INFO 或更高以完全移除 TRACE/DEBUG 调用.
 */
#ifndef LOGGER_MIN_LEVEL
#define LOGGER_MIN_LEVEL LOG_LEVEL_TRACE
#endif

#if LOGGER_MIN_LEVEL <= LOG_LEVEL_TRACE
#define LOG_TRACE(fmt, ...) LogWrite(LOG_LEVEL_TRACE, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)
#else
#define LOG_TRACE(fmt, ...) ((void)0)
#endif

#if LOGGER_MIN_LEVEL <= LOG_LEVEL_DEBUG
#define LOG_DEBUG(fmt, ...) LogWrite(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)
#else
#define LOG_DEBUG(fmt, ...) ((void)0)
#endif

#if LOGGER_MIN_LEVEL <= LOG_LEVEL_INFO
#define LOG_INFO(fmt, ...)  LogWrite(LOG_LEVEL_INFO,  __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)
#else
#define LOG_INFO(fmt, ...)  ((void)0)
#endif

#if LOGGER_MIN_LEVEL <= LOG_LEVEL_WARN
#define LOG_WARN(fmt, ...)  LogWrite(LOG_LEVEL_WARN,  __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)
#else
#define LOG_WARN(fmt, ...)  ((void)0)
#endif

#if LOGGER_MIN_LEVEL <= LOG_LEVEL_ERROR
#define LOG_ERROR(fmt, ...) LogWrite(LOG_LEVEL_ERROR, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)
#else
#define LOG_ERROR(fmt, ...) ((void)0)
#endif

/* ---- 条件日志: 仅当 condition 为真时才输出 ---- */
#define LOG_IF(level, condition, fmt, ...) \
	do { if (condition) LogWrite(level, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__); } while(0)

/* ---- Windows API 错误日志: 自动追加 GetLastError() 说明 ---- */
#define LOG_ERRNO(fmt, ...) \
	do { \
		DWORD _le_ = GetLastError(); \
		char _ebuf_[256] = {0}; \
		FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, _le_, \
			0, _ebuf_, sizeof(_ebuf_) - 1, NULL); \
		LogWrite(LOG_LEVEL_ERROR, __FILE__, __LINE__, __FUNCTION__, \
			"%s [errno=%lu: %s]", fmt, _le_, _ebuf_); \
	} while(0)

#ifdef __cplusplus
}
#endif
