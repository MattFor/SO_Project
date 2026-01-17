//
// Created by MattFor on 21/12/2025.
//

#include <atomic>
#include <cerrno>
#include <poll.h>
#include <random>
#include <thread>
#include <vector>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <iostream>
#include <mqueue.h>
#include <unistd.h>
#include <algorithm>
#include <sys/mman.h>
#include <sys/wait.h>
#include <semaphore.h>
#include <sys/types.h>
#include <sys/resource.h>

#include "include/Utilities.h"

static ERShared* g_shm        = nullptr;
static int       g_shm_fd     = -1;
static sem_t*    g_shm_sem    = nullptr;
static int       g_N          = 10;
static FILE*     master_log   = nullptr;
static FILE*     patients_log = nullptr;

static std::mutex       g_patient_mutex;
static bool             exit_on_no_patients = false;
static std::atomic_bool g_shutdown_requested{false};

static std::atomic_bool g_ipc_lost{false};        // Set by check_ipc_presence()
static std::atomic_bool g_signal_received{false}; // Set by real signal handler
static std::atomic_int  g_signal_number{0};       // Which signal was seen

static void log_master(const std::string& s)
{
    if constexpr (!LOGGING)
    {
        return;
    }

    if (master_log)
    {
        const std::string t = timestamp() + " " + s + "\n";
        if (fwrite(t.c_str(), 1, t.size(), master_log) < 0)
        {
            perror("fwrite(master_log)");
        }
        fflush(master_log);
    }
}

static bool is_pid_alive(const pid_t pid)
{
    return pid > 0 && ( kill(pid, 0) == 0 || errno == EPERM );
}

// Async signal safe handler, only sets atomics
static void shutdown_signal_handler(const int signum)
{
    g_signal_number.store(signum);
    g_signal_received.store(true);
}

static std::string strip_leading_slash(const char* name)
{
    if (!name)
    {
        return {};
    }

    std::string s(name);
    if (!s.empty() && s[0] == '/')
    {
        s.erase(0, 1);
    }

    return s;
}

// Return true if all monitored IPC objects are present on the filesystem
static bool check_ipc_presence_and_report()
{
    // Paths for POSIX IPC
    // - shared memory: /dev/shm/
    // - semaphores: /dev/shm/sem.
    // - message queues: /dev/mqueue/

    std::vector<std::string> missing;

    // SHM
    {
        std::string s    = strip_leading_slash(SHM_NAME);
        std::string path = std::string("/dev/shm/") + s;
        if (access(path.c_str(), F_OK) != 0)
        {
            missing.push_back(path);
        }
    }

    // SEM
    {
        std::string s    = strip_leading_slash(SEM_SHM_NAME);
        std::string path = std::string("/dev/shm/sem.") + s;
        if (access(path.c_str(), F_OK) != 0)
        {
            missing.push_back(path);
        }
    }

    // Message queues
    {
        std::string s    = strip_leading_slash(MQ_REG_NAME);
        std::string path = std::string("/dev/mqueue/") + s;
        if (access(path.c_str(), F_OK) != 0)
        {
            missing.push_back(path);
        }
    }
    {
        std::string s    = strip_leading_slash(MQ_TRIAGE_NAME);
        std::string path = std::string("/dev/mqueue/") + s;
        if (access(path.c_str(), F_OK) != 0)
        {
            missing.push_back(path);
        }
    }
    {
        std::string s    = strip_leading_slash(MQ_DOCTOR_NAME);
        std::string path = std::string("/dev/mqueue/") + s;
        if (access(path.c_str(), F_OK) != 0)
        {
            missing.push_back(path);
        }
    }

    if (!missing.empty())
    {
        std::ostringstream oss;
        oss << "check_ipc_presence: detected missing IPC objects:";
        for (auto& m : missing)
        {
            oss << " " << m;
        }
        log_master(oss.str());

        // Indicate loss (main loop will act on this)
        g_ipc_lost.store(true);
        return false;
    }

    return true;
}

// Get current message counts for MQs (best-effort). returns -1 on error.
static int mq_curmsgs_safe(const char* name)
{
    const mqd_t mq = mq_open(name, O_RDONLY);
    if (mq == (mqd_t)-1)
    {
        return -1;
    }

    mq_attr a{};
    if (mq_getattr(mq, &a) == -1)
    {
        mq_close(mq);
        return -1;
    }

    mq_close(mq);
    return static_cast<int>(a.mq_curmsgs);
}

// Attempt to terminate a service pid nicely, wait a bit, then force a kill if necessary
static void terminate_service_graceful(pid_t& pid, const std::string& name)
{
    if (pid <= 0)
    {
        return;
    }

    if (!is_pid_alive(pid))
    {
        // Already dead
        pid = -1;
        return;
    }

    // Try SIGTERM
    if (kill(pid, SIGTERM) == -1)
    {
        perror(( "kill SIGTERM " + std::to_string(pid) ).c_str());
        log_master("terminate_service_graceful: failed to send SIGTERM to " + std::to_string(pid) + " (" + name + ")");
    }
    else
    {
        log_master("terminate_service_graceful: sent SIGTERM to " + std::to_string(pid) + " (" + name + ")");
    }

    // Wait up to 1 second in small steps for it to exit
    int st = 0;
    for (int i = 0; i < 10; i++)
    {
        if (const pid_t w = waitpid(pid, &st, WNOHANG); w == pid)
        {
            log_master("terminate_service_graceful: " + name + " pid=" + std::to_string(pid) + " exited (status=" + std::to_string(st) + ")");
            pid = -1;
            return;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Still alive -> SIGKILL
    if (is_pid_alive(pid))
    {
        if (kill(pid, SIGKILL) == -1)
        {
            perror(( "kill SIGKILL " + std::to_string(pid) ).c_str());
            log_master("terminate_service_graceful: failed to send SIGKILL to " + std::to_string(pid) + " (" + name + ")");
        }
        else
        {
            log_master("terminate_service_graceful: sent SIGKILL to " + std::to_string(pid) + " (" + name + ")");
        }

        // Final wait (blocking and short)
        if (const pid_t w2 = waitpid(pid, &st, 0); w2 == pid)
        {
            log_master("terminate_service_graceful: " + name + " pid=" + std::to_string(pid) + " reaped after SIGKILL");
        }
    }

    pid = -1;
}

static void truncate_log_file(const std::string& path)
{
    const int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, IPC_MODE);
    if (fd == -1)
    {
        perror(( "truncate_log_file: " + path ).c_str());
        return;
    }

    close(fd);
}

// Cleanup at exit
static void cleanup()
{
    log_master("cleanup: starting cleanup()");

    if (master_log)
    {
        fclose(master_log);
        master_log = nullptr;
    }

    if (patients_log)
    {
        fclose(patients_log);
        patients_log = nullptr;
    }

    if (g_shm)
    {
        munmap(g_shm, sizeof(ERShared));
        g_shm = nullptr;
    }

    if (g_shm_fd != -1)
    {
        close(g_shm_fd);
        shm_unlink(SHM_NAME);
        g_shm_fd = -1;
    }

    if (g_shm_sem)
    {
        sem_close(g_shm_sem);
        sem_unlink(SEM_SHM_NAME);
        g_shm_sem = nullptr;
    }

    mq_unlink(MQ_REG_NAME);
    mq_unlink(MQ_TRIAGE_NAME);
    mq_unlink(MQ_DOCTOR_NAME);

    /* === NEW === */
    if (mq_unlink(MQ_PATIENT_CTRL) == -1 && errno != ENOENT)
    {
        perror("mq_unlink patient_ctrl");
    }
    else
    {
        log_master("cleanup: mq_unlink patient_ctrl attempted");
    }
    /* === END NEW === */

    log_master("cleanup: finished cleanup()");
}


// SIGUSR2 => set evacuation flag in shared memory
static void sigusr2_handler(int)
{
    if (g_shm && g_shm_sem)
    {
        if (sem_wait(g_shm_sem) == -1)
        {
            perror("sem_wait");
        }

        g_shm->evacuation = true;
        if (sem_post(g_shm_sem) == -1)
        {
            perror("sem_post");
        }
    }

    log_master("SIGUSR2 received by master: evacuation flag set in shared memory.");
}

// Spawn process helper (fork + exec)
static pid_t spawn_process(const std::string& path, const std::vector<std::string>& args)
{
    log_master("spawn_process: forking to exec " + path);
    const pid_t pid = fork();
    if (pid == -1)
    {
        perror("fork");
        log_master(std::string("spawn_process: fork failed: ") + strerror(errno));
        return -1;
    }

    if (pid == 0)
    {
        std::vector<char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(const_cast<char*>(path.c_str()));
        for (auto& a : args)
        {
            argv.push_back(const_cast<char*>(a.c_str()));
        }

        argv.push_back(nullptr);
        execv(path.c_str(), argv.data());
        // If execv returns it's an error
        perror("execv");
        _exit(1);
    }

    log_master("spawn_process: forked pid=" + std::to_string(pid) + " for " + path);
    return pid;
}

// Create message queues and shared resources
static bool setup_ipc()
{
    log_master("setup_ipc: creating message queues and shared memory");

    mq_attr attr{};
    attr.mq_flags   = 0;
    attr.mq_maxmsg  = MAX_QUEUE_MESSAGES;
    attr.mq_msgsize = MAX_MSG_SIZE;
    attr.mq_curmsgs = 0;

    mqd_t mqd = mq_open(MQ_REG_NAME, O_CREAT | O_RDWR, IPC_MODE, &attr);
    if (mqd == (mqd_t)-1)
    {
        perror("mq_open reg");
        log_master(std::string("setup_ipc: mq_open reg failed: ") + strerror(errno));
        return false;
    }
    mq_close(mqd);
    log_master("setup_ipc: registration MQ created/validated");

    mqd = mq_open(MQ_TRIAGE_NAME, O_CREAT | O_RDWR, IPC_MODE, &attr);
    if (mqd == (mqd_t)-1)
    {
        perror("mq_open triage");
        log_master(std::string("setup_ipc: mq_open triage failed: ") + strerror(errno));
        return false;
    }
    mq_close(mqd);
    log_master("setup_ipc: triage MQ created/validated");

    mqd = mq_open(MQ_DOCTOR_NAME, O_CREAT | O_RDWR, IPC_MODE, &attr);
    if (mqd == (mqd_t)-1)
    {
        perror("mq_open doctor");
        log_master(std::string("setup_ipc: mq_open doctor failed: ") + strerror(errno));
        return false;
    }
    mq_close(mqd);
    log_master("setup_ipc: doctor MQ created/validated");

    /* === NEW: shared patient control MQ === */
    mq_attr ctrl_attr{};
    ctrl_attr.mq_flags   = 0;
    ctrl_attr.mq_maxmsg  = 1024;
    ctrl_attr.mq_msgsize = sizeof(ControlMessage);
    ctrl_attr.mq_curmsgs = 0;

    mqd = mq_open(
        MQ_PATIENT_CTRL,
        O_CREAT | O_RDWR | O_CLOEXEC,
        IPC_MODE,
        &ctrl_attr);

    if (mqd == (mqd_t)-1)
    {
        perror("mq_open patient_ctrl");
        log_master(std::string("setup_ipc: mq_open patient_ctrl failed: ") + strerror(errno));
        return false;
    }
    mq_close(mqd);
    log_master("setup_ipc: shared patient control MQ created/validated");
    /* === END NEW === */

    g_shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, IPC_MODE);
    if (g_shm_fd == -1)
    {
        perror("shm_open");
        return false;
    }

    if (ftruncate(g_shm_fd, sizeof(ERShared)) == -1)
    {
        perror("ftruncate");
        return false;
    }

    g_shm = static_cast<ERShared*>(
        mmap(nullptr, sizeof(ERShared), PROT_READ | PROT_WRITE, MAP_SHARED, g_shm_fd, 0));
    if (g_shm == MAP_FAILED)
    {
        perror("mmap");
        return false;
    }

    g_shm->N_waiting_room      = g_N;
    g_shm->current_inside      = 0;
    g_shm->waiting_to_register = 0;
    g_shm->total_treated       = 0;
    g_shm->second_reg_open     = false;
    g_shm->evacuation          = false;

    g_shm_sem = sem_open(SEM_SHM_NAME, O_CREAT, IPC_MODE, 1);
    if (g_shm_sem == SEM_FAILED)
    {
        perror("sem_open");
        return false;
    }

    return true;
}


// Attempt to send a spawn-child control message to an adult patient (by pid)
// True if send succeeded
static bool send_spawn_child_to_patient(
    const pid_t target_pid,
    ControlMessage cm)
{
    if (target_pid <= 0)
    {
        log_master("send_spawn_child_to_patient: invalid target_pid");
        return false;
    }

    if (!is_pid_alive(target_pid))
    {
        log_master("send_spawn_child_to_patient: target pid " +
                   std::to_string(target_pid) + " not alive");
        return false;
    }

    // NEW: tag message instead of choosing MQ
    cm.target_pid = static_cast<int>(target_pid);

    const mqd_t mq = mq_open(
        MQ_PATIENT_CTRL,
        O_WRONLY | O_CLOEXEC);

    if (mq == (mqd_t)-1)
    {
        log_master("send_spawn_child_to_patient: mq_open(MQ_PATIENT_CTRL) failed: " +
                   std::string(strerror(errno)));
        return false;
    }

    if (mq_send(
            mq,
            reinterpret_cast<const char*>(&cm),
            sizeof(cm),
            0) == -1)
    {
        perror("mq_send patient_ctrl");
        mq_close(mq);
        return false;
    }

    mq_close(mq);
    log_master("send_spawn_child_to_patient: sent CTRL_SPAWN_CHILD id=" +
               std::to_string(cm.child_id) +
               " to target pid=" + std::to_string(target_pid));

    return true;
}


// Send SIGUSR2 to all services (registration windows, triage, doctors)
static void signal_all_services(const pid_t reg1, const pid_t reg2, const pid_t triage, const std::vector<pid_t>& doctors)
{
    auto send = [](const pid_t pid, const int sig, const char* name)
    {
        if (pid > 0)
        {
            if (kill(pid, sig) == -1)
            {
                perror(( "kill " + std::to_string(pid) ).c_str());
            }
            else
            {
                std::ostringstream oss;
                oss << "Sent signal " << sig << " to pid=" << pid << " (" << name << ")";
                std::cout << oss.str() << std::endl;
            }
        }
    };

    // Clean registration windows
    log_master("signal_all_services: broadcasting SIGUSR2 to services");
    send(reg1, SIGUSR2, "registration #1");
    if (reg2 > 0)
    {
        send(reg2, SIGUSR2, "registration #2");
    }

    // Clean triages, doctors
    send(triage, SIGUSR2, "triage");
    for (const pid_t d : doctors)
    {
        send(d, SIGUSR2, "doctor");
    }
}

static void input_thread_fn(std::atomic_bool& stop_flag, pid_t& reg1_pid, pid_t& reg2_pid, pid_t& triage_pid, std::vector<pid_t>& doctor_pids, std::vector<pid_t>& adult_patient_pids, std::vector<ControlMessage>& pending_children, std::atomic_int& next_id)
{
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    log_master("input_thread: started");
    std::default_random_engine rng(static_cast<unsigned>(time(nullptr)) ^ pthread_self());

    char        buf[1024];
    std::string inbuf; // Accumulate partial reads
    std::string line;

    pollfd pfd{};
    pfd.fd     = STDIN_FILENO;
    pfd.events = POLLIN;

    while (!stop_flag)
    {
        if (int ret = poll(&pfd, 1, 50); ret > 0)
        {
            if (ssize_t n = read(STDIN_FILENO, buf, sizeof( buf ) - 1); n > 0)
            {
                buf[n] = '\0';
                inbuf.append(buf, n);
            }
            // n <= 0 -> nothing to append (EAGAIN/EINTR handled by poll)
        }

        // Quick exit check (exit_on_no_patients)
        if (exit_on_no_patients)
        {
            std::lock_guard lk(g_patient_mutex);
            if (adult_patient_pids.empty() && pending_children.empty())
            {
                log_master("input_thread: exit_on_no_patients triggered, exiting input thread");
                break;
            }
        }

        // Extract full lines from inbuf
        size_t pos;
        while (( pos = inbuf.find_first_of("\r\n") ) != std::string::npos)
        {
            // Isolate single line (trim any additional consecutive newlines)
            line = inbuf.substr(0, pos);
            // Remove the processed line and all contiguous newline chars
            size_t skip = pos;

            while (skip < inbuf.size() && ( inbuf[skip] == '\n' || inbuf[skip] == '\r' ))
            {
                ++skip;
            }

            inbuf.erase(0, skip);

            // Trim trailing spaces
            while (!line.empty() && ( line.back() == '\r' || line.back() == '\n' ))
            {
                line.pop_back();
            }

            // If empty line, continue
            if (line.empty())
            {
                continue;
            }

            // Parse and sanitize command tokens
            std::istringstream iss(line);
            std::string        cmd;
            iss >> cmd;

            if (cmd == "help")
            {
                std::cout << "Supported commands:\n" << "  help                     - show this help\n" << "  signal <SIG...> [pid]    - send a POSIX signal (e.g. SIGUSR2)\n" << "  list                     - show simulation status and queue lengths\n" << "  add N                    - add N new patients now\n" << "  quit | shutdown | exit   - request shutdown\n";
                log_master("input_thread: help requested");
            }
            else if (cmd == "doctor")
            {
                std::string token;
                if (!( iss >> token ))
                {
                    std::cout << "Usage: add N (positive to add, negative to remove)\n";
                    continue;
                }

                int N = 0;
                try
                {
                    N = std::stoi(token);
                }
                catch (...)
                {
                    std::cout << "Invalid number: " << token << "\n";
                    continue;
                }

                if (N == 0)
                {
                    std::cout << "Specify a non-zero number of doctors to add or remove\n";
                    continue;
                }

                if (N > 0)
                {
                    for (int i = 0; i < N; i++)
                    {
                        if (pid_t pid = spawn_process("./doctor", {std::to_string(next_id++)}); pid > 0)
                        {
                            std::lock_guard lk(g_patient_mutex);
                            doctor_pids.push_back(pid);
                            std::cout << "Added doctor pid=" << pid << "\n";
                            log_master("input_thread: added doctor pid=" + std::to_string(pid));
                        }
                    }
                }
                else
                {
                    int             to_remove = -N;
                    std::lock_guard lk(g_patient_mutex);
                    while (to_remove-- > 0 && !doctor_pids.empty())
                    {
                        pid_t pid = doctor_pids.back();
                        doctor_pids.pop_back();

                        std::cout << "Removing doctor pid=" << pid << "...\n";
                        log_master("input_thread: removing doctor pid=" + std::to_string(pid));

                        terminate_service_graceful(pid, "doctor");
                    }
                }
            }
            else if (cmd == "add")
            {
                int N = 0;
                if (!( iss >> N ) || N <= 0)
                {
                    std::cout << "Usage: add N   (N must be positive integer)\n";
                    continue;
                }

                log_master("input_thread: add requested N=" + std::to_string(N));

                bool no_log = false;
                if (std::string next_token; iss >> next_token)
                {
                    no_log = next_token == "no_log";
                }

                // Spawn a thread to handle the bulk addition
                std::thread([N, &rng, &next_id, &adult_patient_pids, &pending_children, no_log]
                {
                    if (setpriority(PRIO_PROCESS, 0, -20) == -1)
                    {
                        perror("setpriority");
                        log_master("input_thread/add: failed to increase priority of bulk add");
                    }

                    for (int i = 0; i < N && !g_shutdown_requested.load(); i++)
                    {
                        int  age      = rng() % 80 + 1;
                        bool is_child = age < 18;
                        bool is_vip   = rng() % 100 < 5;
                        int  my_id    = next_id.fetch_add(1);

                        if (is_child)
                        {
                            ControlMessage cm{};
                            cm.cmd       = CTRL_SPAWN_CHILD;
                            cm.child_id  = my_id;
                            cm.child_age = age;
                            cm.child_vip = is_vip;
                            strncpy(cm.symptoms, "child symptoms", sizeof( cm.symptoms ) - 1);
                            cm.symptoms[sizeof( cm.symptoms ) - 1] = '\0';

                            {
                                bool            dispatched = false;
                                std::lock_guard lk(g_patient_mutex);
                                std::erase_if(adult_patient_pids, [](const pid_t p)
                                {
                                    return !is_pid_alive(p);
                                });

                                if (!adult_patient_pids.empty())
                                {
                                    std::uniform_int_distribution<size_t> di(0, adult_patient_pids.size() - 1);
                                    size_t                                tries = adult_patient_pids.size();
                                    while (tries--)
                                    {
                                        size_t idx = di(rng);
                                        if (pid_t target = adult_patient_pids[idx]; send_spawn_child_to_patient(target, cm))
                                        {
                                            dispatched = true;
                                            break;
                                        }
                                    }
                                }

                                if (!dispatched)
                                {
                                    pending_children.push_back(cm);
                                }
                            }

                            std::ostringstream out;
                            out << "add_thread: created patient id=" << my_id << " age=" << age << " type=child" << ( is_vip ? " VIP" : "" );
                            if (!no_log)
                            {
                                std::cout << out.str() << '\n';
                            }
                            log_master(out.str());
                        }
                        else
                        {
                            if (pid_t pid = spawn_process("./patient", {std::to_string(my_id), std::to_string(age), is_vip ? "1" : "0"}); pid > 0)
                            {
                                {
                                    std::lock_guard lk(g_patient_mutex);
                                    adult_patient_pids.push_back(pid);
                                }

                                std::ostringstream out;
                                out << "add_thread: forked adult patient pid=" << pid << " id=" << my_id << ( is_vip ? " VIP" : "" );
                                if (!no_log)
                                {
                                    std::cout << out.str() << '\n';
                                }
                                log_master(out.str());
                            }
                            else
                            {
                                std::ostringstream out;
                                out << "add_thread: failed to fork adult patient id=" << my_id;
                                if (!no_log)
                                {
                                    std::cout << out.str() << '\n';
                                }
                                log_master(out.str());
                            }
                        }

                        // std::this_thread::sleep_for(std::chrono::microseconds(50));
                    }

                    log_master("add_thread: finished spawning " + std::to_string(N) + " patients");
                }).detach();
            }
            else if (cmd == "signal" || cmd == "sig")
            {
                std::string signame;
                iss >> signame;
                if (signame.empty())
                {
                    std::cout << "Usage: signal <SIGUSR1|SIGUSR2|SIGTERM|SIGINT> [pid]\n";
                    continue;
                }

                if (signame.rfind("SIG", 0) != 0)
                {
                    signame = "SIG" + signame;
                }

                int sig = 0;
                if (signame == "SIGUSR1")
                {
                    sig = SIGUSR1;
                }
                else if (signame == "SIGUSR2")
                {
                    sig = SIGUSR2;
                }
                else if (signame == "SIGTERM")
                {
                    sig = SIGTERM;
                }
                else if (signame == "SIGINT")
                {
                    sig = SIGINT;
                }
                else
                {
                    std::cout << "Unknown signal: " << signame << "\n";
                    continue;
                }

                if (pid_t target = 0; iss >> target)
                {
                    if (kill(target, sig) == -1)
                    {
                        perror("kill (user target)");
                        log_master("input_thread: failed to send " + signame + " to pid=" + std::to_string(target));
                    }
                    else
                    {
                        std::cout << "Sent " << signame << " to pid=" << target << "\n";
                        log_master("input_thread: sent " + signame + " to pid=" + std::to_string(target));
                    }
                }
                else if (sig == SIGUSR2)
                {
                    // Ensure signals are sent exactly once
                    bool do_send = false;
                    if (bool expected = false; g_shutdown_requested.compare_exchange_strong(expected, true))
                    {
                        do_send = true;
                    }

                    if (do_send)
                    {
                        signal_all_services(reg1_pid, reg2_pid, triage_pid, doctor_pids);
                        if (g_shm && g_shm_sem)
                        {
                            sem_wait(g_shm_sem);
                            g_shm->evacuation = true;
                            sem_post(g_shm_sem);
                        }

                        log_master("input_thread: broadcast SIGUSR2 requested by user");
                        // Drain any remaining stdin so same data doesn't cause re-trigger
                        while (read(STDIN_FILENO, buf, sizeof( buf )) > 0)
                        {
                            // Nothing...
                        }
                    }
                    else
                    {
                        std::cout << "Shutdown already requested\n";
                    }
                }
                else
                {
                    std::cout << "Specify pid for signals other than SIGUSR2\n";
                }
            }
            else if (cmd == "list")
            {
                int reg_q    = mq_curmsgs_safe(MQ_REG_NAME);
                int triage_q = mq_curmsgs_safe(MQ_TRIAGE_NAME);
                int doc_q    = mq_curmsgs_safe(MQ_DOCTOR_NAME);

                int waiting = -1, inside = -1, treated = -1;
                if (g_shm && g_shm_sem)
                {
                    sem_wait(g_shm_sem);
                    waiting = g_shm->waiting_to_register;
                    inside  = g_shm->current_inside;
                    treated = g_shm->total_treated;
                    sem_post(g_shm_sem);
                }

                size_t adults_snapshot = 0;
                {
                    std::lock_guard lk(g_patient_mutex);
                    adults_snapshot = adult_patient_pids.size();
                }

                std::cout << "--- simulation status ---\n";
                std::cout << "registration #1 pid=" << reg1_pid << "\n";
                std::cout << "registration #2 pid=" << reg2_pid << "\n";
                std::cout << "triage pid=" << triage_pid << "\n";
                std::cout << "doctors: ";
                for (auto d : doctor_pids)
                {
                    std::cout << d << " ";
                }
                std::cout << "\n";
                std::cout << "active adult patients (tracked): " << adults_snapshot << "\n";
                std::cout << "waiting_to_register (shared): " << waiting << "\n";
                std::cout << "current_inside (shared): " << inside << "\n";
                std::cout << "registration MQ messages: " << ( reg_q >= 0 ? std::to_string(reg_q) : "err" ) << "\n";
                std::cout << "triage MQ messages: " << ( triage_q >= 0 ? std::to_string(triage_q) : "err" ) << "\n";
                std::cout << "doctor MQ messages: " << ( doc_q >= 0 ? std::to_string(doc_q) : "err" ) << "\n";
                std::cout << "pending children (master queue): " << pending_children.size() << "\n";
                std::cout << "total treated (shared): " << treated << "\n";
                log_master("input_thread: list command executed");
            }
            else if (cmd == "quit" || cmd == "shutdown" || cmd == "exit")
            {
                std::cout << "Interactive requested shutdown\n";

                if (bool expected = false; g_shutdown_requested.compare_exchange_strong(expected, true))
                {
                    if (g_shm && g_shm_sem)
                    {
                        sem_wait(g_shm_sem);
                        g_shm->evacuation = true;
                        sem_post(g_shm_sem);
                    }

                    signal_all_services(reg1_pid, reg2_pid, triage_pid, doctor_pids);
                    log_master("input_thread: interactive shutdown requested");

                    // Drain any remaining stdin so duplicate text won't be re-processed
                    while (read(STDIN_FILENO, buf, sizeof( buf )) > 0)
                    {
                        // Nothing...
                    }
                }
                else
                {
                    std::cout << "Shutdown already requested\n";
                }

                stop_flag = true; // Tell caller thread we're done
                log_master("input_thread: exiting after shutdown request");
                return;
            }
            else
            {
                std::cout << "Unknown command. Supported: help, add, signal, list, quit\n";
                log_master("input_thread: unknown command '" + cmd + "'");
            }
        }

        // If no full line available, sleep briefly (keeps thread responsive)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    log_master("input_thread: exiting");
    stop_flag = true;
}

int main(int argc, char** argv)
{
    if constexpr (!LOGGING)
    {
        std::cout << "Logging is disabled" << '\n';
    }

    if (argc < 4)
    {
        std::cerr << "Usage: master <N_waiting> <num_doctors> <num_patients_total>\n";
        return 1;
    }

    g_N              = atoi(argv[1]);
    int num_doctors  = atoi(argv[2]);
    int num_patients = atoi(argv[3]);

    for (int ai = 4; ai < argc; ++ai)
    {
        if (std::string a = argv[ai]; a == "-exit_on_no_patients" || a == "--exit_on_no_patients")
        {
            exit_on_no_patients = true;
            std::ostringstream oss;
            oss << "main: exit_on_no_patients enabled by argv";
            log_master(oss.str());
        }
    }

    truncate_log_file("../../logs/master.log");
    truncate_log_file("../../logs/patients.log");
    truncate_log_file("../../logs/registration.log");
    truncate_log_file("../../logs/triage.log");
    truncate_log_file("../../logs/doctor.log");

    int mfd = open("../../logs/master.log", O_CREAT | O_WRONLY | O_APPEND, IPC_MODE);
    if (mfd == -1)
    {
        perror("open master.log");
        return 1;
    }

    master_log = fdopen(mfd, "a");
    if (!master_log)
    {
        perror("fdopen master.log");
        close(mfd);
        return 1;
    }

    if (int pfd = open("../../logs/patients.log", O_CREAT | O_WRONLY | O_APPEND, IPC_MODE); pfd != -1)
    {
        patients_log = fdopen(pfd, "a");
        if (!patients_log)
        {
            perror("fdopen patients.log");
            close(pfd);
        }
    }

    log_master("main: logs opened. Master starting.");

    struct sigaction sa{};
    sa.sa_handler = sigusr2_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGUSR2, &sa, nullptr) == -1)
    {
        perror("sigaction SIGUSR2");
        log_master("main: sigaction failed");
    }

    struct sigaction sa2{};
    sa2.sa_handler = shutdown_signal_handler;
    sigemptyset(&sa2.sa_mask);
    sa2.sa_flags = 0;

    if (sigaction(SIGINT, &sa2, nullptr) == -1 ||  // Ctrl-c
        sigaction(SIGTERM, &sa2, nullptr) == -1 || // Kill
        sigaction(SIGHUP, &sa2, nullptr) == -1 ||  // Terminal close / hangup
        sigaction(SIGQUIT, &sa2, nullptr) == -1    // Ctrl-\ -> quit
    )
    {
        perror("sigaction KILL ACTIONS");
        log_master("main: sigaction failed");
    }

    if (!setup_ipc())
    {
        log_master("main: setup_ipc failed");
        cleanup();
        return 1;
    }
    log_master("IPC initialized.");

    // Spawn core services immediately and concurrently
    pid_t triage_pid = spawn_process("./triage", {});
    pid_t reg1_pid   = spawn_process("./registration", {"1"});
    pid_t reg2_pid   = -1;

    std::vector<pid_t> doctor_pids;
    for (int i = 0; i < num_doctors; i++)
    {
        doctor_pids.push_back(spawn_process("./doctor", {std::to_string(i + 1)}));
    }

    // Initialize patient tracking structures
    static std::atomic_int      next_id{1};
    std::vector<pid_t>          adult_patient_pids;
    std::vector<ControlMessage> pending_children;

    // Start input thread immediately so commands can be used anytime
    std::atomic_bool input_stop{false};
    std::thread      input_thread(input_thread_fn, std::ref(input_stop), std::ref(reg1_pid), std::ref(reg2_pid), std::ref(triage_pid), std::ref(doctor_pids), std::ref(adult_patient_pids), std::ref(pending_children), std::ref(next_id));
    input_thread.detach();
    log_master("main: input thread launched before spawning patients");

    // Spawn initial patients in BACKGROUND thread to avoid blocking monitor/services
    std::atomic_bool spawn_done{false};
    std::thread      spawn_thread([&]
    {
        if (setpriority(PRIO_PROCESS, 0, -20) == -1)
        {
            perror("setpriority");
            log_master("main: failed to increase priority of bulk spawn");
        }

        std::default_random_engine rng(static_cast<unsigned>(time(nullptr)));
        log_master("spawn_thread: started, will create " + std::to_string(num_patients) + " patients");

        for (int i = 0; i < num_patients && !g_shutdown_requested.load(); i++)
        {
            int  age      = rng() % 80 + 1;
            bool is_child = age < 18;
            bool is_vip   = rng() % 100 < 5;

            int my_id = next_id.fetch_add(1);

            if (is_child)
            {
                ControlMessage cm{};
                cm.cmd       = CTRL_SPAWN_CHILD;
                cm.child_id  = my_id;
                cm.child_age = age;
                cm.child_vip = is_vip;
                strncpy(cm.symptoms, "child symptoms", sizeof( cm.symptoms ) - 1);
                cm.symptoms[sizeof( cm.symptoms ) - 1] = '\0';

                {
                    bool            dispatched = false;
                    std::lock_guard lk(g_patient_mutex);
                    std::erase_if(adult_patient_pids, [](const pid_t p)
                    {
                        return !is_pid_alive(p);
                    });

                    if (!adult_patient_pids.empty())
                    {
                        std::uniform_int_distribution<size_t> di(0, adult_patient_pids.size() - 1);
                        size_t                                tries = adult_patient_pids.size();
                        while (tries--)
                        {
                            size_t idx = di(rng);
                            if (pid_t target = adult_patient_pids[idx]; send_spawn_child_to_patient(target, cm))
                            {
                                dispatched = true;
                                break;
                            }
                        }
                    }

                    if (!dispatched)
                    {
                        pending_children.push_back(cm);
                    }
                }

                std::ostringstream out;
                out << "spawn_thread: created patient id=" << my_id << " age=" << age << " type=child" << ( is_vip ? " VIP" : "" );
                // std::cout << out.str() << "\n";
                log_master(out.str());
            }
            else
            {
                if (pid_t pid = spawn_process("./patient", {std::to_string(my_id), std::to_string(age), is_vip ? "1" : "0"}); pid > 0)
                {
                    {
                        std::lock_guard lk(g_patient_mutex);
                        adult_patient_pids.push_back(pid);
                    }

                    std::ostringstream out;
                    out << "spawn_thread: forked adult patient pid=" << pid << " id=" << my_id << ( is_vip ? " VIP" : "" );
                    // std::cout << out.str() << "\n";
                    log_master(out.str());
                }
                else
                {
                    std::ostringstream out;
                    out << "spawn_thread: failed to fork adult patient id=" << my_id;
                    // std::cout << out.str() << "\n";
                    log_master(out.str());
                }
            }

            // std::this_thread::sleep_for(std::chrono::microseconds(50));
        }

        spawn_done.store(true);
        log_master("spawn_thread: finished spawning (spawn_done = true)");
    });

    log_master("main: spawn_thread launched; entering monitor loop");

    // Monitoring loop
    bool second_open = false;
    while (!g_shutdown_requested)
    {
        if (g_signal_received.load())
        {
            // Only the main thread performs the shutdown actions, attempt to set shutdown flag once
            bool do_send = false;
            if (bool expected = false; g_shutdown_requested.compare_exchange_strong(expected, true))
            {
                do_send = true;
            }

            int                signo = g_signal_number.load();
            std::ostringstream oss;
            oss << "Main: received signal " << signo << " -> initiating evacuation";
            log_master(oss.str());

            // Try to set evacuation in shared memory if possible
            if (g_shm && g_shm_sem)
            {
                if (sem_wait(g_shm_sem) == -1)
                {
                    perror("sem_wait");
                }
                else
                {
                    g_shm->evacuation = true;
                    if (sem_post(g_shm_sem) == -1)
                    {
                        perror("sem_post");
                    }
                }
            }

            if (do_send)
            {
                signal_all_services(reg1_pid, reg2_pid, triage_pid, doctor_pids);
            }

            // Clear the flag so it's no rehandled
            g_signal_received.store(false);
        }

        // Monitor FS for external IPC unlinks
        if (!g_shutdown_requested.load())
        {
            if (!check_ipc_presence_and_report())
            {
                bool did_set = false;
                if (bool expected = false; g_shutdown_requested.compare_exchange_strong(expected, true))
                {
                    did_set = true;
                }
                log_master("Main: IPC loss detected -> triggering evacuation/cleanup (did_set=" + std::to_string(did_set) + ")");

                // Attempt to set evacuation in shared memory
                if (g_shm_sem && sem_wait(g_shm_sem) != -1)
                {
                    if (g_shm)
                        g_shm->evacuation = true;
                    if (sem_post(g_shm_sem) == -1)
                        perror("sem_post");
                }

                // Broadcast to services
                signal_all_services(reg1_pid, reg2_pid, triage_pid, doctor_pids);
                // Set g_shutdown_requested already done by compare_exchange above
            }
        }

        if (exit_on_no_patients)
        {
            int treated = -1;
            if (sem_wait(g_shm_sem) == -1)
            {
                perror("sem_wait");
            }
            else
            {
                treated = g_shm->total_treated;
                sem_post(g_shm_sem);
            }

            if (treated >= num_patients && pending_children.empty() && spawn_done.load())
            {
                break;
            }
        }

        if (sem_wait(g_shm_sem) == -1)
        {
            perror("sem_wait");
        }

        int  waiting = g_shm->waiting_to_register;
        bool evac    = g_shm->evacuation;
        sem_post(g_shm_sem);

        if (evac)
        {
            break;
        }

        int reg_q    = mq_curmsgs_safe(MQ_REG_NAME);
        int triage_q = mq_curmsgs_safe(MQ_TRIAGE_NAME);
        int doc_q    = mq_curmsgs_safe(MQ_DOCTOR_NAME);

        long total_queued = waiting;
        if (reg_q > 0)
        {
            total_queued += reg_q;
        }

        if (triage_q > 0)
        {
            total_queued += triage_q;
        }

        log_master("monitor: queue snapshot waiting=" + std::to_string(waiting) + " reg_q=" + std::to_string(reg_q) + " triage_q=" + std::to_string(triage_q) + " doc_q=" + std::to_string(doc_q) + " total_queued=" + std::to_string(total_queued) + " spawn_done=" + ( spawn_done.load() ? "1" : "0" ));

        if (!second_open && total_queued * 2 >= g_N)
        {
            reg2_pid = spawn_process("./registration", {"2"});
            if (reg2_pid > 0)
            {
                log_master("Opened second registration window pid=" + std::to_string(reg2_pid));
                second_open = true;
            }
            else
            {
                log_master("Attempted to open second registration window but spawn failed.");
            }
        }

        else if (second_open && waiting < g_N / 3 && reg_q <= 0)
        {
            log_master("monitor: deciding to close second registration (waiting < N/3 and reg MQ empty)");
            if (reg2_pid > 0)
            {
                terminate_service_graceful(reg2_pid, "registration #2");
            }
            else
            {
                log_master("monitor: reg2_pid not set, just marking second_open=false");
            }

            second_open = false;
            reg2_pid    = -1;
            log_master("monitor: second registration window closed");
        }

        {
            int   status;
            pid_t w;
            while (( w = waitpid(-1, &status, WNOHANG) ) > 0)
            {
                if (w == reg2_pid)
                {
                    log_master("Reaped registration #2 pid=" + std::to_string(w));
                    reg2_pid    = -1;
                    second_open = false;
                    continue;
                }

                if (w == reg1_pid)
                {
                    log_master("Reaped registration #1 pid=" + std::to_string(w));
                    reg1_pid = -1;
                    continue;
                }

                if (w == triage_pid)
                {
                    log_master("Reaped triage pid=" + std::to_string(w));
                    triage_pid = -1;
                    continue;
                }

                bool doctor_handled = false;
                for (size_t di = 0; di < doctor_pids.size(); ++di)
                {
                    if (doctor_pids[di] == w)
                    {
                        log_master("Reaped doctor pid=" + std::to_string(w));
                        doctor_pids.erase(doctor_pids.begin() + di);
                        doctor_handled = true;
                        break;
                    }
                }

                if (doctor_handled)
                {
                    continue;
                }

                {
                    std::lock_guard lk(g_patient_mutex);
                    std::erase(adult_patient_pids, w);
                }

                log_master("Reaped unknown/child process pid=" + std::to_string(w));
            }
        }

        if (!pending_children.empty())
        {
            std::vector<pid_t> live_adults;
            {
                std::lock_guard lk(g_patient_mutex);
                for (pid_t p : adult_patient_pids)
                {
                    if (is_pid_alive(p))
                    {
                        live_adults.push_back(p);
                    }
                }
            }

            if (!live_adults.empty())
            {
                auto it = pending_children.begin();
                while (it != pending_children.end())
                {
                    bool sent = false;
                    for (pid_t target : live_adults)
                    {
                        if (send_spawn_child_to_patient(target, *it))
                        {
                            sent = true;
                            break;
                        }
                    }

                    if (sent)
                    {
                        it = pending_children.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }
        }

        if (exit_on_no_patients)
        {
            bool all_adults_done = true;
            {
                std::lock_guard lk(g_patient_mutex);
                for (pid_t p : adult_patient_pids)
                {
                    int st = 0;
                    if (p > 0 && waitpid(p, &st, WNOHANG) == 0)
                    {
                        all_adults_done = false;
                        break;
                    }
                }
            }

            if (all_adults_done && pending_children.empty() && spawn_done.load())
            {
                break;
            }
        }

        usleep(20000);
    }

    if (!spawn_done.load())
    {
        g_shutdown_requested = true;
    }

    if (spawn_thread.joinable())
    {
        spawn_thread.join();
        log_master("main: spawn_thread joined");
    }

    log_master("main: shutting down - final shutdown signal wave, for safety");
    signal_all_services(reg1_pid, reg2_pid, triage_pid, doctor_pids);

    if (reg2_pid > 0)
    {
        terminate_service_graceful(reg2_pid, "registration #2");
    }

    if (reg1_pid > 0)
    {
        terminate_service_graceful(reg1_pid, "registration #1");
    }

    if (triage_pid > 0)
    {
        terminate_service_graceful(triage_pid, "triage");
    }

    for (pid_t d : doctor_pids)
    {
        if (d > 0)
        {
            terminate_service_graceful(d, "doctor");
        }
    }

    cleanup();
    g_shutdown_requested = true;
}