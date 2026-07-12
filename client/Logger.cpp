#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
#include "Logger.h"

#include <iostream>
#include <iomanip>
#include <codecvt>


namespace BackupCommon
{


    Logger::Logger()
        :
        m_minLevel(LogLevel::Debug),
        m_console(false),
        m_initialized(false)
    {

    }


    Logger::~Logger()
    {
        Shutdown();
    }



    Logger& Logger::Instance()
    {
        static Logger logger;

        return logger;
    }





    bool Logger::Initialize(
        const std::wstring& logFile,
        bool console
    )
    {
        std::lock_guard<std::mutex> lock(m_mutex);


        m_console = console;


        // 设置 UTF-8 codecvt，避免非 ASCII 字符（如 →）写入时触发 failbit 导致后续日志静默
        m_file.imbue(std::locale(m_file.getloc(), new std::codecvt_utf8<wchar_t>()));
        m_file.open(
            logFile,
            std::ios::out |
            std::ios::app
        );


        if (!m_file.is_open())
        {
            return false;
        }


        m_initialized = true;


        return true;
    }






    void Logger::Write(
        LogLevel level,
        const std::wstring& message
    )
    {

        // 过滤低于当前级别的日志
        if (level < m_minLevel)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);


        if (!m_initialized)
        {
            return;
        }


        std::wstring text;


        text += L"[";
        text += GetTimeString();
        text += L"]";


        text += L"[";
        text += LevelToString(level);
        text += L"] ";


        text += message;



        if (m_console)
        {
            std::wcout
                << text
                << std::endl;
        }



        if (m_file.is_open())
        {
            m_file
                << text
                << L"\n";
            // 如果流进入 failbit（codecvt 转换失败等），恢复状态以便后续日志继续写入
            if (m_file.fail())
            {
                m_file.clear();
            }

            // 不在每条日志后 flush——忙 I/O 时同步刷盘会阻塞整个进程。
            // 日志可靠性由 Shutdown() 中的最终 flush 保证。
        }

    }






    void Logger::Shutdown()
    {
        std::lock_guard<std::mutex> lock(m_mutex);


        if (m_file.is_open())
        {
            m_file.flush();  // 最终刷盘，保证所有缓冲日志落盘
            m_file.close();
        }


        m_initialized = false;
    }




    std::wstring Logger::LevelToString(
        LogLevel level
    )
    {

        switch (level)
        {

        case LogLevel::Debug:
            return L"DEBUG";


        case LogLevel::Info:
            return L"INFO ";


        case LogLevel::Warning:
            return L"WARN ";


        case LogLevel::Error:
            return L"ERROR";


        }


        return L"UNKN ";

    }




    std::wstring Logger::GetTimeString()
    {

        SYSTEMTIME st;

        GetLocalTime(&st);



        wchar_t buffer[64] = { 0 };



        swprintf_s(
            buffer,
            L"%04d-%02d-%02d %02d:%02d:%02d",
            st.wYear,
            st.wMonth,
            st.wDay,
            st.wHour,
            st.wMinute,
            st.wSecond
        );


        return buffer;

    }


}