//
// Created by MattFor on 26/11/2025.
//

#include <atomic>
#include <thread>
#include <string>
#include <cerrno>
#include <iostream>
#include <filesystem>

#include "include/patient/Patient.h"
#include "include/logger/LoggerService.h"

// Global flag set from signal/console handlers
static std::atomic_bool g_terminate_flag{false};

extern "C" void request_terminate() noexcept
{
	g_terminate_flag.store(true, std::memory_order_release);
}

#if defined(_WIN32)
#  include <windows.h>
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "Ws2_32.lib")

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
			return TRUE;
		default:
			return FALSE;
	}
}

#else
#  include <signal.h>
void posix_signal_handler(int /*sig*/)
{
	request_terminate();
}
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
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
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		perror("WSAStartup failed");
		return false;
	}
#endif
	return true;
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
	sock_t s = INVALID_SOCK;
	s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCK)
	{
		perror("socket() failed");
		set_errno_from_wsa();
		return INVALID_SOCK;
	}

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);

	if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
	{
		perror("inet_pton failed");
		close_sock(s);
		set_errno_from_wsa();
		return INVALID_SOCK;
	}

	if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
	{
		perror("connect() failed");
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
			perror("send() failed");
			set_errno_from_wsa();
			return false;
		}

		ptr += sent;
		left -= static_cast<size_t>(sent);
	}

	return true;
}

static bool send_log_line_to_logger(const std::string& host, unsigned short port, const std::string& line)
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


static void print_help(const char* progname)
{
	std::cout
			<< "Usage: " << progname << " [logfile] [port] [options]\n\n"
			<< "Positional:\n"
			<< "  logfile            path to logfile for LoggerService (default: main.log)\n"
			<< "  port               port for LoggerService (default: 7878)\n\n"
			<< "Options:\n"
			<< "  -h                 show this help and exit\n"
			<< "  -pc <N>            patient count (integer, default 1)\n"
			<< "  -cp <X|X%>         child count or percentage (e.g. 5 or 40%)\n"
			<< "  -vc <X|X%>         VIP count or percentage (e.g. 2 or 10%)\n"
			<< "  -rc <N>            registration window count (default 2, minimum 2)\n"
			<< "  -clear             truncate logfile (create/empty file) before starting LoggerService\n\n";
	//	<< "Examples:\n"
	//	<< "  " << progname << " main.log 7878 -pc 10 -cp 30% -vc 2 -rc 3\n"
	//	<< "  " << progname << " -pc 5 -cp 2 -vc 40%          # positional logfile/port stay defaults\n";
}

static bool parse_int(const std::string& s, int& out)
{
	try
	{
		size_t pos = 0;
		long v = std::stol(s, &pos);

		if (pos != s.size())
		{
			return false;
		}

		if (v < 0)
		{
			return false;
		}

		out = static_cast<int>(v);
		return true;
	}
	catch (...)
	{
		return false;
	}
}

static bool parse_percent_or_number(const std::string& token, int total, int& out_value)
{
	if (token.empty())
	{
		return false;
	}

	if (token.back() == '%')
	{
		int pct = 0;
		std::string num = token.substr(0, token.size() - 1);

		if (!parse_int(num, pct))
		{
			return false;
		}

		if (pct < 0)
		{
			pct = 0;
		}

		if (pct > 100)
		{
			pct = 100;
		}

		// Round to nearest integer (total * pct + 50) / 100
		out_value = static_cast<int>((static_cast<long long>(total) * pct + 50) / 100 );

		return true;
	}
	else
	{
		return parse_int(token, out_value);
	}
}

int main(const int argc, char** argv)
{
	std::filesystem::path logfile = "main.log";
	unsigned short port = 7878;

	int pc = 1;      // patient count
	int cp_value = 0; // children count
	int vc_value = 0; // vip count
	int rc = 2;      // registration windows
	bool do_clear_logfile = false;

	int argi = 1;
	if (argc > argi)
	{
		std::string a1 = argv[argi];
		if (!a1.empty() && a1[0] != '-')
		{
			logfile = a1;
			++argi;
		}
	}

	if (argc > argi)
	{
		std::string a2 = argv[argi];
		if (!a2.empty() && a2[0] != '-')
		{
			try
			{
				port = static_cast<unsigned short>(std::stoi(a2));
				++argi;
			}
			catch (...)
			{
				// Nothing
			}
		}
	}

	for (int i = argi; i < argc; ++i)
	{
		std::string opt = argv[i];
		if (opt == "-h")
		{
			print_help(argv[0]);
			return 0;
		}
		else if (opt == "-pc")
		{
			if (i + 1 >= argc)
			{
				std::cerr << "Option -pc requires a value\n";
				return 2;
			}

			int val = 0;
			if (!parse_int(argv[i + 1], val))
			{
				std::cerr << "Invalid -pc value: " << argv[i + 1] << "\n";
				return 2;
			}

			if (val < 0)
			{
				val = 0;
			}

			pc = val;
			i++;
		}
		else if (opt == "-cp")
		{
			if (i + 1 >= argc)
			{
				std::cerr << "Option -cp requires a value\n";
				return 2;
			}

			std::string token = argv[i + 1];
			int computed = 0;

			if (!parse_percent_or_number(token, pc, computed))
			{
				std::cerr << "Invalid -cp value: " << token << "\n";
				return 2;
			}

			cp_value = computed;
			i++;
		}
		else if (opt == "-vc")
		{
			if (i + 1 >= argc)
			{
				std::cerr << "Option -vc requires a value\n";
				return 2;
			}

			int computed = 0;
			std::string token = argv[i + 1];

			if (!parse_percent_or_number(token, pc, computed))
			{
				std::cerr << "Invalid -vc value: " << token << "\n";
				return 2;
			}

			vc_value = computed;
			i++;
		}
		else if (opt == "-rc")
		{
			if (i + 1 >= argc)
			{
				std::cerr << "Option -rc requires a value\n";
				return 2;
			}

			int val = 0;
			if (!parse_int(argv[i + 1], val))
			{
				std::cerr << "Invalid -rc value: " << argv[i + 1] << "\n";
				return 2;
			}
			if (val < 2)
			{
				std::cerr << "Registration window count must be at least 2; using 2\n";
				rc = 2;
			}
			else
			{
				rc = val;
			}
			i++;
		}
		else if (opt == "-clear")
		{
			do_clear_logfile = true;
		}
		else
		{
			std::cerr << "Unknown option: " << opt << "\n";
			print_help(argv[0]);

			return 2;
		}
	}

	if (pc < 0)
	{
		pc = 0;
	}

	if (cp_value < 0)
	{
		cp_value = 0;
	}

	if (vc_value < 0)
	{
		vc_value = 0;
	}

	if (cp_value > pc)
	{
		std::cerr << "Child count (" << cp_value << ") > patient count (" << pc << "); capping to " << pc << "\n";
		cp_value = pc;
	}

	if (vc_value > pc)
	{
		std::cerr << "VIP count (" << vc_value << ") > patient count (" << pc << "); capping to " << pc << "\n";
		vc_value = pc;
	}

	if (rc < 2)
	{
		std::cerr << "Registration windows < 2; setting to 2\n";
		rc = 2;
	}

	if (do_clear_logfile)
	{
		std::filesystem::path path = std::filesystem::path("..") / logfile;
		const std::string path_str = path.string();

#if defined(_WIN32)
		const std::wstring wpath = path.wstring();

		if (DeleteFileW(wpath.c_str()) != 0)
		{
			std::cerr << "Deleted logfile: " << path_str << '\n';
		}
		else
		{
			const DWORD gle = GetLastError();
			if (gle == ERROR_FILE_NOT_FOUND)
			{
				std::cerr << "No logfile to delete: " << path_str << '\n';
			}
			else
			{
				std::cerr << "DeleteFileW failed (err=" << gle << "), attempting truncate fallback...\n";
#if defined(_MSC_VER)
				FILE* f = _wfopen(wpath.c_str(), L"w");
#else
				FILE* f = std::fopen(path_str.c_str(), "w");
#endif
				if (f)
				{
					std::fclose(f);
					std::cerr << "Truncated logfile (fallback): " << path_str << '\n';
				}
				else
				{
					std::perror(("Failed to clear logfile: " + path_str).c_str());
				}
			}
		}
#else
	std::error_code ec;
	if (std::filesystem::remove(path, ec))
	{
		std::cerr << "Deleted logfile: " << path_str << '\n';
	}
	else
	{
		if (ec)
		{
			std::cerr << "remove() failed: " << ec.message() << ", attempting truncate fallback...\n";
			FILE* f = std::fopen(path_str.c_str(), "w");
			if (f)
			{
				std::fclose(f);
				std::cerr << "Truncated logfile (fallback): " << path_str << '\n';
			}
			else
			{
				std::perror(("Failed to clear logfile: " + path_str).c_str());
			}
		}
		else
		{
			std::cerr << "No logfile to remove: " << path_str << '\n';
		}
	}
#endif
	}

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

	std::cout << "LoggerService started on 127.0.0.1:" << port << " (logfile: " << logfile << ")\n";
	std::cout << "Press Ctrl+C or send 'END' (with newline) from a client to stop.\n";

	auto tcpLogger = [port](const std::string& msg)
	{
		const std::string host = "127.0.0.1";
		bool ok = send_log_line_to_logger(host, port, msg);
		if (!ok)
		{
			std::cerr << "[LOGGER FALLBACK] " << msg << '\n';
		}
	};

	// Create patients vector
	std::vector <Patient> patients;
	patients.reserve(static_cast<size_t>(std::max(0, pc)));

	int cp_remaining = cp_value;
	int vc_remaining = vc_value;

	for (int idx = 0; idx < pc; ++idx)
	{
		PatientData pd;
		std::ostringstream idss;
		idss << "p_" << std::setw(4) << std::setfill('0') << (idx + 1);
		pd.id = idss.str();

		std::ostringstream namess;
		namess << "Patient_" << (idx + 1);
		pd.name = namess.str();

		if (cp_remaining > 0)
		{
			pd.age = static_cast<unsigned int>((idx % 17) + 1);
			--cp_remaining;
		}
		else
		{
			pd.age = static_cast<unsigned int>(18 + (idx % 63));
		}

		if (vc_remaining > 0)
		{
			pd.vip = true;
			--vc_remaining;
		}
		else
		{
			pd.vip = false;
		}

		pd.symptoms = (pd.age < 18) ? "fever, cough (child)" : "cough and mild fever";

		try
		{
			patients.emplace_back(pd, nullptr, tcpLogger);
		}
		catch (...)
		{
			std::cerr << "Failed to construct Patient object for id=" << pd.id << "\n";
		}
	}

	{
		std::ostringstream ss;
		ss << "CONFIG: patients=" << pc << " children=" << cp_value << " vip=" << vc_value << " reg_windows=" << rc;
		std::cout << ss.str() << "\n";
		tcpLogger(ss.str());
	}

	while (!g_terminate_flag.load(std::memory_order_acquire) && svc.is_open())
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}

	svc.stop();
}