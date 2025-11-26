//
// Created by MattFor on 26.11.2025.
//

#include "Logger.h"


void Logger::log(const std::string_view line)
{
    if (!this->open_flag.load(std::memory_order_acquire))
    {
        return;
    }

    std::scoped_lock lk(this->mtx);
    if (!this->file.is_open())
    {
        return;
    }

    this->file << '[' << timestamp() << "] " << line << '\n';
    this->file.flush();
}

void Logger::flush()
{
    std::scoped_lock lk(this->mtx);
    if (this->file.is_open())
    {
        this->file.flush();
    }
}

void Logger::close()
{
    std::scoped_lock lk(this->mtx);
    if (this->file.is_open())
    {
        this->file.flush();
        this->file.close();
        this->open_flag.store(false, std::memory_order_release);
    }
}

bool Logger::is_open() const noexcept
{
    return this->open_flag.load(std::memory_order_acquire);
}
