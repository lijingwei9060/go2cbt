#pragma once

#include <windows.h>

#include <string>
#include <fstream>
#include <mutex>


namespace BackupCommon
{

	enum class LogLevel
	{
		Debug,
		Info,
		Warning,
		Error
	};



	/*
		Logger: 全局日志管理器
		特点:
		1. 支持Unicode
		2. 支持文件输出
		3. 支持控制台输出
		4. 线程安全

	*/
	class Logger
	{

	public:

		/*
			获取单例对象
		*/
		static Logger& Instance();



		/*
			初始化日志
			logFile :日志文件路径
			console: 是否输出到控制台

		*/
		bool Initialize(
			const std::wstring& logFile,
			bool console = true
		);



		/*
			写日志
		*/
		void Write(
			LogLevel level,
			const std::wstring& message
		);



		/*
			关闭日志
		*/
		void Shutdown();



	private:


		Logger();
		~Logger();


		Logger(const Logger&) = delete;
		Logger& operator=(const Logger&) = delete;
		std::wstring LevelToString(LogLevel level);
		std::wstring GetTimeString();



	private:

		std::wofstream m_file;
		bool m_console;
		bool m_initialized;
		std::mutex m_mutex;

	};




}


// 快捷宏
#define LOG_DEBUG(msg) BackupCommon::Logger::Instance().Write(BackupCommon::LogLevel::Debug,msg)
#define LOG_INFO(msg)  BackupCommon::Logger::Instance().Write(BackupCommon::LogLevel::Info,msg)
#define LOG_WARNING(msg) BackupCommon::Logger::Instance().Write(BackupCommon::LogLevel::Warning,msg)
#define LOG_ERROR(msg) BackupCommon::Logger::Instance().Write(BackupCommon::LogLevel::Error,msg)