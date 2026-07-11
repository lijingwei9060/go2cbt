#include "Logger.h"

#include <iostream>
#include <iomanip>


namespace BackupCommon
{


    Logger::Logger()
        :
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
                << std::endl;


            m_file.flush();
        }

    }





    void Logger::Shutdown()
    {
        std::lock_guard<std::mutex> lock(m_mutex);


        if (m_file.is_open())
        {
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