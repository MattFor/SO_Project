//
// Created by MattFor on 26.11.2025.
//

#ifndef SO_PROJECT_LOGGERSERVICE_H
#define SO_PROJECT_LOGGERSERVICE_H

#include <thread>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
using socklen_t = int;
#else
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif


static void socket_startup()
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

static void socket_cleanup()
{
#ifdef _WIN32
    WSACleanup();
#endif
}

static void socket_close(int fd)
{
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}


#include "Logger.h"


class LoggerService
{
    unsigned short    port;
    Logger            logger;
    int               listen_fd{-1};
    std::jthread      accept_thread;
    std::atomic<bool> running{false};

public:
    explicit LoggerService(std::filesystem::path logfile, const unsigned short port_ = 7878)
        : port(port_), logger(std::move(logfile))
    {
    }

    ~LoggerService()
    {
        stop();
    }

    void start();
    void stop();

    bool is_open() const noexcept;
    bool is_running() const noexcept;

private:
    void handle_client(int client_fd);
    void accept_loop(const std::stop_token& st);
};

#endif //SO_PROJECT_LOGGERSERVICE_H
