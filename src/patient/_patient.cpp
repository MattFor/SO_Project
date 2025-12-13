//
// Created by MattFor on 13/12/2025.
//

#include <string>
#include <thread>
#include <atomic>
#include <sstream>
#include <cstring>
#include <iostream>

#include "patient/Patient.h"

#if defined(_WIN32)

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "Ws2_32.lib")
#else
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

static std::atomic_bool g_terminate_flag{false};

extern "C" void request_terminate() noexcept
{
	g_terminate_flag.store(true, std::memory_order_release);
}

#if !defined(_WIN32)
void posix_signal_handler(int /*sig*/) {
	request_terminate();
}
#endif

#if defined(_WIN32)
using sock_t = SOCKET;
static constexpr sock_t INVALID_SOCK = INVALID_SOCKET;
#else
using sock_t = int;
static constexpr sock_t INVALID_SOCK = -1;
#endif

static bool sockets_startup()
{
#if defined(_WIN32)
	WSADATA wsa{};
	return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
	return true;
#endif
}

static void sockets_cleanup()
{
#if defined(_WIN32)
	WSACleanup();
#endif
}

static void close_sock(sock_t s)
{
#if defined(_WIN32)
	closesocket(s);
#else
	close(s);
#endif
}

static void set_errno_from_wsa()
{
#if defined(_WIN32)
	errno = WSAGetLastError();
#endif
}

static sock_t connect_to_logger(const std::string& host, unsigned short port)
{
	sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCK)
	{
		set_errno_from_wsa();
		return INVALID_SOCK;
	}

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);

	if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
	{
		close_sock(s);
		set_errno_from_wsa();
		return INVALID_SOCK;
	}

	if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
	{
		set_errno_from_wsa();
		close_sock(s);
		return INVALID_SOCK;
	}

	return s;
}

static bool send_all(sock_t s, const std::string& data)
{
	size_t left = data.size();
	const char* ptr = data.data();

	while (left > 0)
	{
		int sent = static_cast<int>(send(s, ptr, static_cast<int>(left), 0));
		if (sent <= 0)
		{
			set_errno_from_wsa();
			return false;
		}

		ptr += sent;
		left -= static_cast<size_t>(sent);
	}

	return true;
}

static bool send_log_line(const std::string& host, unsigned short port, const std::string& line)
{
	if (!sockets_startup())
	{
		return false;
	}

	sock_t s = connect_to_logger(host, port);
	if (s == INVALID_SOCK)
	{
		sockets_cleanup();
		return false;
	}

	std::string data = line;
	if (data.empty() || data.back() != '\n')
	{
		data.push_back('\n');
	}

	bool ok = send_all(s, data);
	close_sock(s);
	sockets_cleanup();

	return ok;
}

int main(int argc, char** argv)
{
	if (argc < 8)
	{
		std::cerr << "Usage: _patient <id> <name> <age> <vip> <symptoms> <logger_host> <logger_port>\n";
		return 1;
	}

	PatientData pd;
	pd.id = argv[1];
	pd.name = argv[2];
	pd.age = static_cast<unsigned int>(std::stoi(argv[3]));
	pd.vip = std::string(argv[4]) == "1";
	pd.symptoms = argv[5];

	std::string logger_host = argv[6];
	unsigned short logger_port = static_cast<unsigned short>(std::stoi(argv[7]));

	auto logger = [logger_host, logger_port](const std::string& msg)
	{
		if (!send_log_line(logger_host, logger_port, msg))
		{
			std::cerr << "[LOGGER FALLBACK] " << msg << std::endl;
		}
	};

	Patient patient(pd, nullptr, logger);

	// Signal handlers
#if defined(_WIN32)
	auto ctrlHandler = [](DWORD /*ctrlType*/) -> BOOL
	{
		request_terminate();
		return TRUE;
	};

	SetConsoleCtrlHandler(ctrlHandler, TRUE);
#else
	struct sigaction sa{};
	sa.sa_handler = posix_signal_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGTERM, &sa, nullptr);
	sigaction(SIGINT, &sa, nullptr);
#endif

	logger("Patient process started: " + pd.id);

	while (!g_terminate_flag.load(std::memory_order_acquire))
	{
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}

	logger("Patient process terminating: " + pd.id);
}

