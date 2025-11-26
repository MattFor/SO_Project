//
// Created by MattFor on 26.11.2025.
//

#ifndef SO_PROJECT_LOGGER_H
#define SO_PROJECT_LOGGER_H

#include <mutex>
#include <atomic>
#include <chrono>
#include <string>
#include <format>
#include <fstream>
#include <filesystem>

class Logger
{
    std::mutex mtx;
    std::ofstream file;
    std::filesystem::path path;
    std::atomic<bool> open_flag{false};


    static std::string timestamp()
    {
        using namespace std::chrono;
        auto now = system_clock::now();
        return std::format("{:%Y-%m-%d %H:%M:%S}", now);
    }


public:
    explicit Logger(const std::filesystem::path& p) : path(p)
    {
        std::scoped_lock lk(this->mtx);
        this->file.open(std::filesystem::path("../") / this->path, std::ios::app | std::ios::binary);
        this->open_flag = this->file.is_open();
    }

    ~Logger()
    {
        close();
    }


    void log(std::string_view line);

    void flush();
    void close();

    bool is_open() const noexcept;
};


#endif //SO_PROJECT_LOGGER_H
