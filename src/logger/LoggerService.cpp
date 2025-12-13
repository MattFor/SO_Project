//
// Created by MattFor on 26.11.2025.
//

#include <iostream>

#include "../../include/logger/LoggerService.h"


void LoggerService::start()
{
    if (this->running.exchange(true))
    {
        return;
    }

    socket_startup();

    this->listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (this->listen_fd < 0)
    {
        std::cerr << "socket() failed\n";
        this->running = false;
        return;
    }

    int opt = 1;
    setsockopt(this->listen_fd, SOL_SOCKET, SO_REUSEADDR,
#ifdef _WIN32
               reinterpret_cast<const char*>(&opt), sizeof(opt)
#else
               &opt, sizeof(opt)
#endif
    );

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // Bind to localhost only
    addr.sin_port        = htons(this->port);

    if (bind(this->listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        std::cerr << "bind() failed on port " << this->port << "\n";
        socket_close(this->listen_fd);
        this->running = false;
        return;
    }

    if (listen(listen_fd, 8) < 0)
    {
        std::cerr << "listen() failed\n";
        socket_close(listen_fd);
        this->running = false;
        return;
    }

    this->accept_thread = std::jthread{[this](const std::stop_token& st) { accept_loop(st); }};
    std::cout << "LoggerService listening on 127.0.0.1:" << this->port << "\n";
}

void LoggerService::stop()
{
    if (!this->running.exchange(false))
    {
        return;
    }

    // Cause accept to break
    socket_close(this->listen_fd);
    if (this->accept_thread.joinable())
    {
        this->accept_thread.request_stop(), this->accept_thread.join();
    }

    this->logger.close();
    socket_cleanup();
    std::cout << "LoggerService stopped\n";
}

bool LoggerService::is_open() const noexcept
{
    return this->logger.is_open();
}

bool LoggerService::is_running() const noexcept
{
    return this->running.load(std::memory_order_acquire);
}

// Private //

void LoggerService::accept_loop(const std::stop_token& st)
{
    while (!st.stop_requested() && this->running.load())
    {
        sockaddr_in client_addr{};
        socklen_t   client_len = sizeof(client_addr);
        int         client_fd  = accept(this->listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0)
        {
            // Accept can fail when socket closed to stop service
            if (!this->running.load())
            {
                break;
            }

            continue;
        }

        // Handle each client in its own jthread
        std::jthread{[this, client_fd](const std::stop_token&) { handle_client(client_fd); }}.detach();
    }
}

void LoggerService::handle_client(const int client_fd)
{
    constexpr size_t BUF_SZ = 4096;
    std::string      buffer;
    buffer.reserve(1024);
    char tmp[BUF_SZ];

    while (true)
    {
        const ssize_t n = recv(client_fd, tmp, sizeof(tmp), 0);
        if (n <= 0)
        {
            break;
        }

        buffer.append(tmp, tmp + n);

        // Process lines
        size_t pos = 0;
        while (true)
        {
            const auto nl = buffer.find('\n', pos);
            if (nl == std::string::npos)
            {
                break;
            }

            std::string_view line{buffer.data() + pos, nl - pos};

            // Trim \r
            if (!line.empty() && line.back() == '\r')
            {
                line = line.substr(0, line.size() - 1);
            }

            if (line == "END")
            {
                // Special end signal: flush & close and stop service
                this->logger.log("<END signal received>");
                this->logger.flush();
                this->logger.close();

                // Stop the service
                this->running.store(false);

                socket_close(this->listen_fd);
                socket_close(client_fd);
                socket_cleanup();

                return;
            }

            this->logger.log(line);
            pos = nl + 1;
        }

        // Remove processed prefix
        if (pos > 0)
        {
            buffer.erase(0, pos);
        }
    }

    socket_close(client_fd);
}
