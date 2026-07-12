#pragma once
// go2cbt 轻量测试框架 — 无外部依赖，兼容 Google Test 命名风格
// 后续可平滑迁移到 GTest：将 TEST() → TEST()，ASSERT_* → ASSERT_*，EXPECT_* → EXPECT_*

#include <windows.h>
#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include <chrono>
#include <cstring>

// ============================================================
// 测试用例注册
// ============================================================
struct TestCase
{
	const char* Suite;
	const char* Name;
	void (*Func)();
	const char* File;
	int Line;
};

inline std::vector<TestCase>& GetTestRegistry()
{
	static std::vector<TestCase> tests;
	return tests;
}

struct TestAutoRegister
{
	TestAutoRegister(const char* suite, const char* name,
		void (*func)(), const char* file, int line)
	{
		GetTestRegistry().push_back({ suite, name, func, file, line });
	}
};

#define TEST(suite_name, test_name) \
	void test_##suite_name##_##test_name(); \
	static TestAutoRegister _reg_##suite_name##_##test_name( \
		#suite_name, #test_name, test_##suite_name##_##test_name, __FILE__, __LINE__); \
	void test_##suite_name##_##test_name()

// ============================================================
// 断言宏
// ============================================================

// 辅助：宽字符串格式化
inline void FormatFailure(const wchar_t* expr, const char* file, int line,
	const wchar_t* extra = nullptr, const wchar_t* valA = nullptr, const wchar_t* valB = nullptr)
{
	std::wcout << L"\n  [FAIL] " << file << L"(" << line << L"): " << expr;
	if (extra) std::wcout << L" — " << extra;
	if (valA) std::wcout << L" [" << valA << L"]";
	if (valB) std::wcout << L" vs [" << valB << L"]";
	std::wcout << std::endl;
}

#define ASSERT_TRUE(expr) \
	do { if (!(expr)) { \
		FormatFailure(L#expr, __FILE__, __LINE__); \
		throw std::runtime_error("ASSERT_TRUE failed"); \
	} } while(0)

#define ASSERT_FALSE(expr) \
	do { if (expr) { \
		FormatFailure(L#expr, __FILE__, __LINE__); \
		throw std::runtime_error("ASSERT_FALSE failed"); \
	} } while(0)

#define ASSERT_EQ(a, b) \
	do { \
		auto _va = (a); auto _vb = (b); \
		if (_va != _vb) { \
			wchar_t bufA[64], bufB[64]; \
			swprintf_s(bufA, L"%lld", (long long)_va); \
			swprintf_s(bufB, L"%lld", (long long)_vb); \
			FormatFailure(L#a L" == " L#b, __FILE__, __LINE__, L"ASSERT_EQ", bufA, bufB); \
			throw std::runtime_error("ASSERT_EQ failed"); \
		} \
	} while(0)

#define ASSERT_NE(a, b) \
	do { \
		auto _va = (a); auto _vb = (b); \
		if (_va == _vb) { \
			FormatFailure(L#a L" != " L#b, __FILE__, __LINE__, L"ASSERT_NE"); \
			throw std::runtime_error("ASSERT_NE failed"); \
		} \
	} while(0)

#define ASSERT_GT(a, b) \
	do { \
		auto _va = (a); auto _vb = (b); \
		if (!(_va > _vb)) { \
			FormatFailure(L#a L" > " L#b, __FILE__, __LINE__, L"ASSERT_GT"); \
			throw std::runtime_error("ASSERT_GT failed"); \
		} \
	} while(0)

#define ASSERT_LT(a, b) \
	do { \
		auto _va = (a); auto _vb = (b); \
		if (!(_va < _vb)) { \
			FormatFailure(L#a L" < " L#b, __FILE__, __LINE__, L"ASSERT_LT"); \
			throw std::runtime_error("ASSERT_LT failed"); \
		} \
	} while(0)

#define ASSERT_LE(a, b) \
	do { \
		auto _va = (a); auto _vb = (b); \
		if (!(_va <= _vb)) { \
			FormatFailure(L#a L" <= " L#b, __FILE__, __LINE__, L"ASSERT_LE"); \
			throw std::runtime_error("ASSERT_LE failed"); \
		} \
	} while(0)

#define ASSERT_GE(a, b) \
	do { \
		auto _va = (a); auto _vb = (b); \
		if (!(_va >= _vb)) { \
			FormatFailure(L#a L" >= " L#b, __FILE__, __LINE__, L"ASSERT_GE"); \
			throw std::runtime_error("ASSERT_GE failed"); \
		} \
	} while(0)

// 宽字符串比较
#define ASSERT_STREQ(a, b) \
	do { \
		if (wcscmp((a), (b)) != 0) { \
			FormatFailure(L#a L" == " L#b, __FILE__, __LINE__, L"ASSERT_STREQ", (a), (b)); \
			throw std::runtime_error("ASSERT_STREQ failed"); \
		} \
	} while(0)

// 窄字符串比较
#define ASSERT_STR_EQ(a, b) \
	do { \
		if (strcmp((a), (b)) != 0) { \
			FormatFailure(L#a L" == " L#b, __FILE__, __LINE__); \
			throw std::runtime_error("ASSERT_STR_EQ failed"); \
		} \
	} while(0)

// 内存比较
#define ASSERT_MEMEQ(a, b, size) \
	do { \
		if (memcmp((a), (b), (size)) != 0) { \
			FormatFailure(L#a L" == " L#b, __FILE__, __LINE__, L"ASSERT_MEMEQ"); \
			throw std::runtime_error("ASSERT_MEMEQ failed"); \
		} \
	} while(0)

// 无异常断言
#define ASSERT_NO_THROW(expr) \
	do { \
		try { expr; } catch (...) { \
			FormatFailure(L#expr, __FILE__, __LINE__, L"ASSERT_NO_THROW - exception thrown"); \
			throw std::runtime_error("ASSERT_NO_THROW failed"); \
		} \
	} while(0)

// EXPECT 版本（不抛异常，继续执行）
#define EXPECT_TRUE(expr) \
	do { if (!(expr)) FormatFailure(L#expr, __FILE__, __LINE__, L"EXPECT_TRUE"); } while(0)

#define EXPECT_EQ(a, b) \
	do { \
		auto _va = (a); auto _vb = (b); \
		if (_va != _vb) { \
			wchar_t bufA[64], bufB[64]; \
			swprintf_s(bufA, L"%lld", (long long)_va); \
			swprintf_s(bufB, L"%lld", (long long)_vb); \
			FormatFailure(L#a L" == " L#b, __FILE__, __LINE__, L"EXPECT_EQ", bufA, bufB); \
		} \
	} while(0)

#define EXPECT_STREQ(a, b) \
	do { if (wcscmp((a), (b)) != 0) FormatFailure(L#a L" == " L#b, __FILE__, __LINE__, L"EXPECT_STREQ", (a), (b)); } while(0)

#define EXPECT_MEMEQ(a, b, size) \
	do { if (memcmp((a), (b), (size)) != 0) FormatFailure(L#a L" == " L#b, __FILE__, __LINE__, L"EXPECT_MEMEQ"); } while(0)

// ============================================================
// 测试运行器
// ============================================================
inline int RunAllTests()
{
	auto& tests = GetTestRegistry();
	int passed = 0, failed = 0, skipped = 0;

	std::wcout << L"\n============================================================" << std::endl;
	std::cout << "  go2cbt Test Suite — " << tests.size() << " test(s) registered" << std::endl;
	std::wcout << L"============================================================\n" << std::endl;

	const char* currentSuite = "";

	for (const auto& tc : tests)
	{
		// 打印 suite 分组头
		if (strcmp(currentSuite, "") == 0 || strcmp(currentSuite, tc.Suite) != 0)
		{
			currentSuite = tc.Suite;
			std::cout << "[" << currentSuite << "]" << std::endl;
		}

		auto start = std::chrono::steady_clock::now();

		try
		{
			tc.Func();
			auto end = std::chrono::steady_clock::now();
			auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

			std::cout << "  [PASS] " << tc.Name;
			if (ms > 100)
				std::cout << " (" << ms << " ms)";
			std::cout << std::endl;
			passed++;
		}
		catch (const std::exception& e)
		{
			std::wcout << L"  [FAIL] " << tc.Name
				<< L" — " << e.what() << std::endl;
			failed++;
		}
		catch (...)
		{
			std::wcout << L"  [FAIL] " << tc.Name
				<< L" — unknown exception" << std::endl;
			failed++;
		}
	}

	std::wcout << L"\n============================================================" << std::endl;
	std::cout << "  Results: " << passed << " passed, "
		<< failed << " failed, " << skipped << " skipped" << std::endl;
	std::wcout << L"============================================================\n" << std::endl;

	return failed > 0 ? 1 : 0;
}

// ============================================================
// 测试辅助工具
// ============================================================

// 生成指定大小的填充数据（用于块数据测试）
inline std::vector<uint8_t> MakeTestData(size_t size, uint8_t seed = 0)
{
	std::vector<uint8_t> data(size);
	for (size_t i = 0; i < size; i++)
		data[i] = (uint8_t)((i + seed) & 0xFF);
	return data;
}

// 生成已知哈希的测试块（预先计算好 SHA-256，用于验证 BlockHasher）
// SHA-256 of "go2cbt test block v1" repeated to fill 1MB
inline void GetKnownTestHash(uint8_t hashOut[32])
{
	// SHA-256 of the ASCII string "go2cbt-blockhash-test-vector-0001"
	// Pre-computed: a7 3b 8c 9d 2e 1f 45 67 89 ab cd ef 01 23 45 67
	//               89 ab cd ef fe dc ba 98 76 54 32 10 0f 1e 2d 3c 4b
	static const uint8_t known[32] = {
		0xa7, 0x3b, 0x8c, 0x9d, 0x2e, 0x1f, 0x45, 0x67,
		0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67,
		0x89, 0xab, 0xcd, 0xef, 0xfe, 0xdc, 0xba, 0x98,
		0x76, 0x54, 0x32, 0x10, 0x0f, 0x1e, 0x2d, 0x3c
	};
	memcpy(hashOut, known, 32);
}

// 获取临时目录路径（测试用）
inline std::wstring GetTempTestDir()
{
	wchar_t tempPath[MAX_PATH];
	GetTempPathW(MAX_PATH, tempPath);
	wcscat_s(tempPath, L"go2cbt_tests\\");
	CreateDirectoryW(tempPath, nullptr);
	return std::wstring(tempPath);
}

// 清理测试临时文件
inline void CleanupTestDir(const std::wstring& dir)
{
	wchar_t searchPath[MAX_PATH];
	swprintf_s(searchPath, L"%s\\*", dir.c_str());

	WIN32_FIND_DATAW fd;
	HANDLE hFind = FindFirstFileW(searchPath, &fd);
	if (hFind != INVALID_HANDLE_VALUE)
	{
		do
		{
			wchar_t fullPath[MAX_PATH];
			swprintf_s(fullPath, L"%s\\%s", dir.c_str(), fd.cFileName);
			DeleteFileW(fullPath);
		} while (FindNextFileW(hFind, &fd));
		FindClose(hFind);
	}
	RemoveDirectoryW(dir.c_str());
}
