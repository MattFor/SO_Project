//
// Created by MattFor on 26.11.2025.
//

#include <atomic>
#include <thread>
#include <chrono>
#include <iostream>
#include <filesystem>

#include "include/logger/LoggerService.h"


// Global flag set from signal/console handlers
static std::atomic_bool g_terminate_flag{false};

extern "C" void request_terminate() noexcept
{
    g_terminate_flag.store(true, std::memory_order_release);
}

#if defined(_WIN32)
#include <windows.h>

BOOL WINAPI console_ctrl_handler(const DWORD ctrl_type)
{
    switch (ctrl_type)
    {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
        case CTRL_LOGOFF_EVENT:
            request_terminate();
            // Return TRUE to indicate we handled it (prevents default)
            return TRUE;
        default:
            return FALSE;
    }
}
#else
#include <signal.h>
void posix_signal_handler(int /*sig*/)
{
    request_terminate();
}
#endif

int main(const int argc, char** argv)
{
    std::filesystem::path logfile = "logger_service.log";
    unsigned short        port    = 7878;

    if (argc > 1)
    {
        logfile = argv[1];
    }

    if (argc > 2)
    {
        port = static_cast<unsigned short>(std::stoi(argv[2]));
    }

    // Install handlers BEFORE starting service
#if defined(_WIN32)
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
    struct sigaction sa{};
    sa.sa_handler = posix_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);  // Ctrl+C
    sigaction(SIGTERM, &sa, nullptr); // Kill
    sigaction(SIGHUP, &sa, nullptr);  // Terminal closed / hangup
    sigaction(SIGQUIT, &sa, nullptr);
#endif

    LoggerService svc(logfile, port);
    svc.start();

    std::cout << "Press Ctrl+C or send 'END' (with newline) from a client to stop.\n";

    // Wait loop: break when service stops or termination requested
    while (!g_terminate_flag.load(std::memory_order_acquire) && svc.is_open())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    svc.stop();
}
