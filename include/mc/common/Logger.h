#pragma once

#include <string>
#include <fstream>
#include <mutex>

namespace mc {

enum class LogLevel
{
    Info,
    Warn,
    Error
};

class Logger
{
public:
    static Logger& Instance();

    void Log(LogLevel level, const std::string& message);
    void LogInfo(const std::string& message)    { Log(LogLevel::Info, message); }
    void LogWarn(const std::string& message)    { Log(LogLevel::Warn, message); }
    void LogError(const std::string& message)   { Log(LogLevel::Error, message); }

    void SetLogFile(const std::string& filePath);

private:
    Logger();
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    std::string LevelToString(LogLevel level) const;
    std::string Timestamp() const;

    std::mutex      m_mutex;
    std::ofstream   m_file;
};

} // namespace mc