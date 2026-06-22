#include "mc/common/Logger.h"

#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace mc {

Logger& Logger::Instance()
{
    static Logger instance;
    return instance;
}

Logger::Logger()
{
    // Default log file
    SetLogFile("logs/runtime.log");
}

Logger::~Logger()
{
    if (m_file.is_open())
    {
        m_file.close();
    }
}

void Logger::SetLogFile(const std::string& filePath)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file.is_open())
    {
        m_file.close();
    }
    m_file.open(filePath, std::ios::out | std::ios::app);
}

void Logger::Log(LogLevel level, const std::string& message)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::string line = "[" + LevelToString(level) + "] " + message;
    std::string timestamped = Timestamp() + " " + line;

    // Console output
    std::cout << line << std::endl;

    // File output
    if (m_file.is_open())
    {
        m_file << timestamped << std::endl;
        m_file.flush();
    }
}

std::string Logger::LevelToString(LogLevel level) const
{
    switch (level)
    {
    case LogLevel::Info:  return "INFO";
    case LogLevel::Warn:  return "WARN";
    case LogLevel::Error: return "ERROR";
    }
    return "UNKNOWN";
}

std::string Logger::Timestamp() const
{
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;

    std::ostringstream oss;
    oss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
    oss << "." << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

} // namespace mc