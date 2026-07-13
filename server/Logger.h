#pragma once

// 线程安全、单例化、带级别的日志器
// - 默认输出到 stdout
// - --verbose 开启 DEBUG
// - 格式: [2026-07-13 14:30:05.123] [INFO] [thread] 消息内容
// - 宏: LOG_DEBUG / LOG_INFO / LOG_WARNING / LOG_ERROR

#include <cstdarg>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace Logger
{

enum class Level : int
{
    DEBUG   = 0,
    INFO    = 1,
    WARNING = 2,
    ERROR   = 3,
};

void Init(Level minLevel = Level::INFO);
void SetLevel(Level minLevel);
Level GetLevel();

// 内部使用，外部请用宏
void LogRaw(Level level, const char* fmt, ...);

// 哈希格式化工具：将 32 字节哈希转为 64 字符十六进制字符串
std::string HashToHex(const uint8_t hash[32]);

// 版本类型 UTF-16LE → UTF-8 字符串
std::string Utf16leToUtf8(const uint16_t* data, size_t count);

} // namespace Logger

#define LOG_DEBUG(...)   ::Logger::LogRaw(::Logger::Level::DEBUG,   __VA_ARGS__)
#define LOG_INFO(...)    ::Logger::LogRaw(::Logger::Level::INFO,    __VA_ARGS__)
#define LOG_WARNING(...) ::Logger::LogRaw(::Logger::Level::WARNING, __VA_ARGS__)
#define LOG_ERROR(...)   ::Logger::LogRaw(::Logger::Level::ERROR,   __VA_ARGS__)
