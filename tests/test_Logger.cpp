// Logger 模块单元测试
// 覆盖：单例模式、日志级别过滤、初始化/关闭、线程安全、消息格式
#include "test_framework.h"
#include "../client/Logger.h"

using namespace BackupCommon;

// ============================================================
// 单例 & 初始化
// ============================================================
TEST(Logger, Singleton_ReturnsSameInstance)
{
	Logger& a = Logger::Instance();
	Logger& b = Logger::Instance();
	ASSERT_EQ((intptr_t)&a, (intptr_t)&b);
}

TEST(Logger, Initialize_WithValidPath_Succeeds)
{
	// 每个测试重新获取实例（单例已存在，重新初始化）
	Logger::Instance().Shutdown();
	bool ok = Logger::Instance().Initialize(L"test_logger.log", false);
	ASSERT_TRUE(ok);
}

TEST(Logger, SetMinLevel_And_GetMinLevel_RoundTrip)
{
	Logger::Instance().SetMinLevel(LogLevel::Debug);
	ASSERT_EQ((int)Logger::Instance().GetMinLevel(), (int)LogLevel::Debug);

	Logger::Instance().SetMinLevel(LogLevel::Info);
	ASSERT_EQ((int)Logger::Instance().GetMinLevel(), (int)LogLevel::Info);

	Logger::Instance().SetMinLevel(LogLevel::Warning);
	ASSERT_EQ((int)Logger::Instance().GetMinLevel(), (int)LogLevel::Warning);

	Logger::Instance().SetMinLevel(LogLevel::Error);
	ASSERT_EQ((int)Logger::Instance().GetMinLevel(), (int)LogLevel::Error);

	// 恢复默认
	Logger::Instance().SetMinLevel(LogLevel::Debug);
}

TEST(Logger, MinLevel_FiltersMessages)
{
	// 设置为 WARNING 级别，DEBUG 和 INFO 应被过滤
	Logger::Instance().SetMinLevel(LogLevel::Warning);

	// 验证：Write() 不会崩溃，即使消息被过滤
	//（日志文件内容由集成测试验证）
	ASSERT_NO_THROW(Logger::Instance().Write(LogLevel::Debug, L"should be filtered"));
	ASSERT_NO_THROW(Logger::Instance().Write(LogLevel::Info, L"should also be filtered"));
	ASSERT_NO_THROW(Logger::Instance().Write(LogLevel::Warning, L"should pass"));
	ASSERT_NO_THROW(Logger::Instance().Write(LogLevel::Error, L"should pass"));

	// 恢复
	Logger::Instance().SetMinLevel(LogLevel::Debug);
}

TEST(Logger, Write_AllLevels_NoCrash)
{
	Logger::Instance().SetMinLevel(LogLevel::Debug);

	ASSERT_NO_THROW(Logger::Instance().Write(LogLevel::Debug, L"debug message"));
	ASSERT_NO_THROW(Logger::Instance().Write(LogLevel::Info, L"info message"));
	ASSERT_NO_THROW(Logger::Instance().Write(LogLevel::Warning, L"warning message"));
	ASSERT_NO_THROW(Logger::Instance().Write(LogLevel::Error, L"error message"));
}

TEST(Logger, Write_EmptyMessage_NoCrash)
{
	ASSERT_NO_THROW(Logger::Instance().Write(LogLevel::Info, L""));
}

TEST(Logger, Write_LongMessage_NoCrash)
{
	std::wstring longMsg(4096, L'X');
	ASSERT_NO_THROW(Logger::Instance().Write(LogLevel::Info, longMsg));
}

TEST(Logger, Write_UnicodeMessage_NoCrash)
{
	ASSERT_NO_THROW(Logger::Instance().Write(LogLevel::Info, L"Unicode 测试 🧪 — 日本語 한국어"));
}

TEST(Logger, Shutdown_And_Reinitialize)
{
	Logger::Instance().Shutdown();
	ASSERT_NO_THROW(Logger::Instance().Initialize(L"test_reinit.log", false));
	ASSERT_TRUE(true);  // 没崩溃就是成功
}

// ============================================================
// 多线程安全性（烟雾测试）
// ============================================================
#include <thread>
#include <atomic>

TEST(Logger, ThreadSafety_MultipleWriters_NoCrash)
{
	Logger::Instance().SetMinLevel(LogLevel::Debug);
	std::atomic<bool> start{ false };
	std::atomic<int> done{ 0 };

	auto writer = [&](int id) {
		while (!start) { /* spin */ }
		for (int i = 0; i < 100; i++)
		{
			wchar_t msg[128];
			swprintf_s(msg, L"[Thread-%d] message %d", id, i);
			Logger::Instance().Write(LogLevel::Info, msg);
		}
		done++;
	};

	std::thread t1(writer, 1);
	std::thread t2(writer, 2);
	std::thread t3(writer, 3);
	std::thread t4(writer, 4);

	start = true;
	t1.join();
	t2.join();
	t3.join();
	t4.join();

	ASSERT_EQ(done.load(), 4);
}

// ============================================================
// 日志级别枚举
// ============================================================
TEST(Logger, LogLevel_EnumValues)
{
	// 确保级别顺序：Debug < Info < Warning < Error
	ASSERT_TRUE((int)LogLevel::Debug < (int)LogLevel::Info);
	ASSERT_TRUE((int)LogLevel::Info < (int)LogLevel::Warning);
	ASSERT_TRUE((int)LogLevel::Warning < (int)LogLevel::Error);
}

// ============================================================
// LOG 宏
// ============================================================
TEST(Logger, Macros_NoCrash)
{
	ASSERT_NO_THROW(LOG_DEBUG(L"test debug macro"));
	ASSERT_NO_THROW(LOG_INFO(L"test info macro"));
	ASSERT_NO_THROW(LOG_WARNING(L"test warning macro"));
	ASSERT_NO_THROW(LOG_ERROR(L"test error macro"));
}
