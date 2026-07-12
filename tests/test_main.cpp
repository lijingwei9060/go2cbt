// 测试入口 — 初始化日志系统，运行所有已注册测试
#include "test_framework.h"
#include "../client/Logger.h"

int main()
{
	// 初始化日志到文件（测试环境不输出到控制台，避免干扰测试报告）
	BackupCommon::Logger::Instance().Initialize(L"test_output.log", false);

	int result = RunAllTests();

	BackupCommon::Logger::Instance().Shutdown();
	return result;
}
