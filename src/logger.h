#pragma once
#include <fstream>
#include <string>
#include <filesystem>
#include <chrono>
#include <iomanip>

class Logger {
public:
    explicit Logger(const std::string& path) : path_(path) {
        std::filesystem::create_directories(std::filesystem::path(path_).parent_path());
    }

    void info(const std::string& msg) { write("INFO", msg); }
    void warning(const std::string& msg) { write("WARNING", msg); }
    void error(const std::string& msg) { write("ERROR", msg); }

private:
    std::string path_;

    void write(const std::string& level, const std::string& msg) {
        std::ofstream out(path_, std::ios::app);
        if (!out) return;

        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);

        std::tm tm{};
    #ifdef _WIN32
        localtime_s(&tm, &tt);
    #else
        localtime_r(&tt, &tm);
    #endif

        out << "[" << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "] "
            << "[" << level << "] "
            << msg << "\n";
    }
};
