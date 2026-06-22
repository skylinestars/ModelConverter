#include <gtest/gtest.h>
#include <fstream>
#include <cstdio>
#include <filesystem>
#include "mc/common/Logger.h"

class LoggerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Use a temp file so test is independent of working directory
        testLogPath = (std::filesystem::temp_directory_path() / "mc_test_runtime.log").string();
        mc::Logger::Instance().SetLogFile(testLogPath);
    }

    void TearDown() override
    {
        // Restore default log file relative to project root
        mc::Logger::Instance().SetLogFile("logs/runtime.log");
        std::error_code ec;
        std::filesystem::remove(testLogPath, ec);
    }

    std::string ReadLogFile()
    {
        std::ifstream file(testLogPath);
        if (!file.is_open())
        {
            return "";
        }
        std::string content((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
        return content;
    }

    std::string testLogPath;
};

TEST_F(LoggerTest, LogInfo_WritesToFile)
{
    mc::Logger::Instance().LogInfo("Test Info Message");
    std::string content = ReadLogFile();
    EXPECT_NE(content.find("[INFO]"), std::string::npos);
    EXPECT_NE(content.find("Test Info Message"), std::string::npos);
}

TEST_F(LoggerTest, LogWarn_WritesToFile)
{
    mc::Logger::Instance().LogWarn("Test Warn Message");
    std::string content = ReadLogFile();
    EXPECT_NE(content.find("[WARN]"), std::string::npos);
    EXPECT_NE(content.find("Test Warn Message"), std::string::npos);
}

TEST_F(LoggerTest, LogError_WritesToFile)
{
    mc::Logger::Instance().LogError("Test Error Message");
    std::string content = ReadLogFile();
    EXPECT_NE(content.find("[ERROR]"), std::string::npos);
    EXPECT_NE(content.find("Test Error Message"), std::string::npos);
}

TEST_F(LoggerTest, MultipleLogs_AllWritten)
{
    mc::Logger::Instance().LogInfo("First");
    mc::Logger::Instance().LogWarn("Second");
    mc::Logger::Instance().LogError("Third");

    std::string content = ReadLogFile();
    EXPECT_NE(content.find("First"), std::string::npos);
    EXPECT_NE(content.find("Second"), std::string::npos);
    EXPECT_NE(content.find("Third"), std::string::npos);
}