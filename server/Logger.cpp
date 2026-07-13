#include "Logger.h"

#include <cstdio>
#include <ctime>
#include <sys/syscall.h>
#include <unistd.h>
#include <codecvt>
#include <locale>
#include <sstream>

namespace Logger
{

static Level          g_minLevel  = Level::INFO;
static std::mutex     g_mutex;
static FILE*          g_stream    = stdout;

static const char* LevelTag(Level l)
{
    switch (l)
    {
        case Level::DEBUG:   return "DEBUG";
        case Level::INFO:    return "INFO ";
        case Level::WARNING: return "WARN ";
        case Level::ERROR:   return "ERROR";
    }
    return "?    ";
}

void Init(Level minLevel)
{
    g_minLevel = minLevel;
    setvbuf(g_stream, nullptr, _IOLBF, 0);
}

void SetLevel(Level minLevel) { g_minLevel = minLevel; }
Level GetLevel()               { return g_minLevel; }

void LogRaw(Level level, const char* fmt, ...)
{
    if (static_cast<int>(level) < static_cast<int>(g_minLevel))
        return;

    char timeBuf[32];
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    std::tm tmv;
    localtime_r(&ts.tv_sec, &tmv);
    std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tmv);

    char userBuf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(userBuf, sizeof(userBuf), fmt, args);
    va_end(args);

    // tid 简化为主线程 pid 即可（单线程模型）
    long tid = static_cast<long>(syscall(SYS_gettid));

    std::lock_guard<std::mutex> lock(g_mutex);
    fprintf(g_stream, "[%s.%03ld] [%s] [tid=%ld] %s\n",
            timeBuf, ts.tv_nsec / 1000000, LevelTag(level), tid, userBuf);
    fflush(g_stream);
}

std::string HashToHex(const uint8_t hash[32])
{
    static const char* kHex = "0123456789abcdef";
    std::string s(64, '\0');
    for (int i = 0; i < 32; ++i)
    {
        s[i * 2]     = kHex[(hash[i] >> 4) & 0xF];
        s[i * 2 + 1] = kHex[hash[i] & 0xF];
    }
    return s;
}

std::string Utf16leToUtf8(const uint16_t* data, size_t count)
{
    // 简单实现：只处理 BMP 内的基本字符，遇到 0 终止
    std::string out;
    out.reserve(count * 2);
    for (size_t i = 0; i < count; ++i)
    {
        uint32_t cp = data[i];
        if (cp == 0) break;
        if (cp < 0x80)
            out.push_back(static_cast<char>(cp));
        else if (cp < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

} // namespace Logger
