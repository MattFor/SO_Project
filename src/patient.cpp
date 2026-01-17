//
// Created by MattFor on 21/12/2025.
//

#include <mutex>
#include <ctime>
#include <atomic>
#include <cerrno>
#include <thread>
#include <vector>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <mqueue.h>
#include <unistd.h>
#include <iostream>
#include <sys/mman.h>
#include <semaphore.h>
#include <sys/types.h>
#include <sys/resource.h>

#include "../include/Utilities.h"

static std::thread              g_ctrl_thread;
static std::vector<std::thread> g_child_threads;
static std::atomic_bool         g_running{true};
static PatientInfo g_self{};

// Registration MQ reused for this process
static mqd_t g_reg_mq = (mqd_t)-1;

// Local copies of shared memory handles (for child threads to increment waiting_to_register)
static int       g_shm_fd_local  = -1;
static ERShared* g_shm_local     = nullptr;
static sem_t*    g_shm_sem_local = nullptr;

// Ensure current_inside is decremented exactly once by this patient process
static bool       slot_released = false;
static std::mutex slot_mutex;

// Track how many successful registrations this process performed (adult + its children)
static std::atomic_int g_registered_count{0};

enum ExitReason
{
    NONE,
    TREATED,
    EVACUATED,
    DISMISSED_BY_TRIAGE,
    OTHER_SIG
};

static std::atomic g_exit_reason{NONE};

static void log_patient_local(const std::string& s)
{
    if constexpr (!LOGGING)
    {
        return;
    }

    const int pfd = open("../../logs/patients.log", O_CREAT | O_WRONLY | O_APPEND, IPC_MODE);
    if (pfd == -1)
    {
        perror("open patients.log (patient)");
        return;
    }

    FILE* pf = fdopen(pfd, "a");
    if (!pf)
    {
        perror("fdopen patients.log (patient)");
        close(pfd);
        return;
    }

    std::string t = timestamp() + " [patient pid=" + std::to_string(getpid()) + "] " + s + "\n";
    if (fwrite(t.c_str(), 1, t.size(), pf) < 0)
    {
        perror("fwrite patients.log (patient)");
    }

    fflush(pf);
    fclose(pf);
}

// Try to raise RLIMIT_NOFILE
static void try_raise_rlimit()
{
    rlimit rl{};
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0)
    {
        if (constexpr rlim_t want = 65536; rl.rlim_cur < want)
        {
            rl.rlim_cur = std::min(want, rl.rlim_max);
            if (setrlimit(RLIMIT_NOFILE, &rl) == -1)
            {
                perror("setrlimit RLIMIT_NOFILE (patient)");
            }
            else
            {
                log_patient_local("Increased RLIMIT_NOFILE to " + std::to_string(rl.rlim_cur));
            }
        }
    }
}

static void release_waiting_room_slot_once()
{
    std::lock_guard lk(slot_mutex);
    if (slot_released)
    {
        return;
    }

    slot_released = true;

    if (!g_shm_local || !g_shm_sem_local)
    {
        log_patient_local("release_waiting_room_slot_once: no shm/sem available (no-op)");
        return;
    }

    // Use loop to decrement exactly one owned slot if available
    if (sem_wait(g_shm_sem_local) == -1)
    {
        perror("sem_wait (patient release)");
        return;
    }

    if (const int owned = g_registered_count.load(); owned > 0 && g_shm_local->current_inside > 0)
    {
        g_shm_local->current_inside--;
        g_registered_count.fetch_sub(1);
        log_patient_local("Released waiting room slot; current_inside -> " + std::to_string(g_shm_local->current_inside));
    }
    else
    {
        log_patient_local("Release requested but no owned slot or current_inside==0 (no-op)");
    }

    if (sem_post(g_shm_sem_local) == -1)
    {
        perror("sem_post (patient release)");
    }
}

// Open registration MQ once (writer) with a small retry/backoff thing
static void setup_reg_mq_once()
{
    if (g_reg_mq != (mqd_t)-1)
    {
        return;
    }

    int tries = 10;
    while (tries--)
    {
        g_reg_mq = mq_open(MQ_REG_NAME, O_WRONLY | O_CLOEXEC);
        if (g_reg_mq != (mqd_t)-1)
        {
            break;
        }

        if (errno == ENOENT)
        {
            // Maybe master hasn't created it yet, time to wait a bit
            usleep(50 * 1000);
            continue;
        }

        if (errno == EMFILE || errno == ENFILE)
        {
            // Resource limit, try to raise and wait
            try_raise_rlimit();
            usleep(100 * 1000);
            continue;
        }

        perror("mq_open (patient setup) reg");
        break;
    }
}

// Send registration to registration MQ (used by adult and child threads)
//  - ensure MQ descriptor exists (try cached g_reg_mq, otherwise open tmp)
//  - increment shared waiting_to_register BEFORE mq_send (if semaphore available)
//  - if mq_send fails, rollback the increment
//  - increment g_registered_count on *success* and keep log
static bool send_registration(const PatientInfo& p)
{
    auto mq_to_use = (mqd_t)-1;
    bool used_tmp  = false;

    // Try cached descriptor first
    if (g_reg_mq != (mqd_t)-1)
    {
        mq_to_use = g_reg_mq;
    }
    else
    {
        // Attempt to lazy-open cached descriptor
        setup_reg_mq_once();
        if (g_reg_mq != (mqd_t)-1)
        {
            mq_to_use = g_reg_mq;
        }
    }

    // If still not opened, try one-shot open
    if (mq_to_use == (mqd_t)-1)
    {
        mq_to_use = mq_open(MQ_REG_NAME, O_WRONLY | O_CLOEXEC);
        if (mq_to_use == (mqd_t)-1)
        {
            // Cannot open registration MQ, time to give up
            perror("mq_open patient->reg (final)");
            log_patient_local("send_registration: failed to open reg MQ, aborting registration for id=" + std::to_string(p.id));
            return false;
        }

        used_tmp = true;
    }

    // Prepare message buffer
    char buf[MAX_MSG_SIZE] = {};
    memcpy(buf, &p, sizeof(PatientInfo));
    const unsigned int prio = p.is_vip ? 10u : 1u;

    // Increment shared waiting_to_register BEFORE enqueue so master / monitor sees accurate pending count
    bool incremented = false;
    if (g_shm_sem_local && g_shm_local)
    {
        if (sem_wait(g_shm_sem_local) == -1)
        {
            perror("sem_wait (patient send_registration pre)");
        }
        else
        {
            g_shm_local->waiting_to_register++;
            incremented = true;
            if (sem_post(g_shm_sem_local) == -1)
            {
                perror("sem_post (patient send_registration pre)");
            }
        }
    }

    // Attempt to send
    if (mq_send(mq_to_use, buf, sizeof(PatientInfo), prio) == -1)
    {
        perror("mq_send patient");
        log_patient_local("send_registration: mq_send failed for id=" + std::to_string(p.id) + " errno=" + std::to_string(errno));

        // Rollback increment if able to update it
        if (incremented && g_shm_sem_local && g_shm_local)
        {
            if (sem_wait(g_shm_sem_local) == -1)
            {
                perror("sem_wait (patient send_registration rollback)");
            }
            else
            {
                if (g_shm_local->waiting_to_register > 0)
                {
                    g_shm_local->waiting_to_register--;
                }
                else
                {
                    log_patient_local("send_registration rollback: waiting_to_register already zero");
                }

                if (sem_post(g_shm_sem_local) == -1)
                {
                    perror("sem_post (patient send_registration rollback)");
                }
            }
        }

        if (used_tmp && mq_to_use != (mqd_t)-1)
        {
            mq_close(mq_to_use);
        }

        return false;
    }

    // Success
    log_patient_local("Registered patient id=" + std::to_string(p.id) + " age=" + std::to_string(p.age) + ( p.is_vip ? " VIP" : "" ));
    g_registered_count.fetch_add(1);

    if (used_tmp && mq_to_use != (mqd_t)-1)
    {
        mq_close(mq_to_use);
    }

    return true;
}

// Child thread function (runs inside patient process)
// Path will ensure any outstanding current_inside counts are reclaimed if needed
static void child_thread_fn(const ControlMessage& cm)
{
    PatientInfo p{};
    p.id     = cm.child_id;
    p.pid    = 0; // Child is not a persistent process
    p.age    = cm.child_age;
    p.is_vip = cm.child_vip;
    strncpy(p.symptoms, cm.symptoms, sizeof( p.symptoms ) - 1);
    p.symptoms[sizeof( p.symptoms ) - 1] = '\0';

    log_patient_local("Spawned child-thread id=" + std::to_string(p.id) + " age=" + std::to_string(p.age) + ( p.is_vip ? " VIP" : "" ));

    if (const bool ok = send_registration(p); !ok)
    {
        log_patient_local("child_thread: registration failed for child id=" + std::to_string(p.id));
    }
    else
    {
        log_patient_local("child_thread: registration SUCCESS for child id=" + std::to_string(p.id));
    }
}

// Control thread that listens for control messages on per-patient MQ
// It opens the MQ by name and uses mq_timedreceive with a short timeout so it can
// Check the g_running flag regularly and exit quickly when requested
static void control_thread_fn()
{
    // Reader descriptor
    const mqd_t ctrl_mq_rd =
        mq_open(MQ_PATIENT_CTRL, O_RDONLY | O_CLOEXEC);

    if (ctrl_mq_rd == (mqd_t)-1)
    {
        log_patient_local("control_thread: failed to open shared ctrl MQ (reader)");
        return;
    }

    // Writer descriptor used only when we need to requeue messages not meant for us
    mqd_t ctrl_mq_wr = mq_open(MQ_PATIENT_CTRL, O_WRONLY | O_CLOEXEC);
    if (ctrl_mq_wr == (mqd_t)-1)
    {
        // Not fatal — we'll attempt to reopen lazily later if needed
        log_patient_local("control_thread: could not open shared ctrl MQ (writer) at start, will retry on demand errno=" + std::to_string(errno));
        ctrl_mq_wr = (mqd_t)-1;
    }

    ControlMessage cm{};
    // buffer must be large enough for arbitrary control message payloads
    char buf[sizeof(ControlMessage)];
    timespec ts{};

    while (g_running)
    {
        // prepare timeout (500ms)
        if (clock_gettime(CLOCK_REALTIME, &ts) == -1)
        {
            perror("clock_gettime");
        }
        ts.tv_sec  += 0;
        ts.tv_nsec += 500 * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000L)
        {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }

        unsigned int recv_prio = 0;
        const ssize_t r = mq_timedreceive(
            ctrl_mq_rd,
            buf,
            sizeof(buf),
            &recv_prio,
            &ts);

        if (r == -1)
        {
            if (errno == ETIMEDOUT || errno == EINTR)
                continue;

            perror("mq_timedreceive patient ctrl");
            break;
        }

        if (r < static_cast<ssize_t>(sizeof(ControlMessage)))
        {
            // malformed/short message — log and skip
            log_patient_local("control_thread: received short/malformed control message (len=" + std::to_string(r) + ")");
            continue;
        }

        // copy into typed structure
        memcpy(&cm, buf, sizeof(cm));

        // If the message is NOT for this process, requeue it (so other patients can consume)
        if (cm.target_pid != static_cast<int>(getpid()))
        {
            // ensure we have a writer descriptor
            if (ctrl_mq_wr == (mqd_t)-1)
            {
                ctrl_mq_wr = mq_open(MQ_PATIENT_CTRL, O_WRONLY | O_CLOEXEC);
                if (ctrl_mq_wr == (mqd_t)-1)
                {
                    log_patient_local("control_thread: re-open writer failed errno=" + std::to_string(errno));
                    // Avoid busy looping re-trying; push the message back later
                    // Sleep briefly to avoid tight loop and hope other consumer will handle it
                    usleep(1000);
                    // NOTE: message is already removed by mq_timedreceive; we couldn't requeue it.
                    // This is unfortunate — but we'll continue trying for future messages.
                    continue;
                }
            }

            // Attempt to requeue with the same priority and original length 'r'
            if (mq_send(ctrl_mq_wr, buf, static_cast<size_t>(r), recv_prio) == -1)
            {
                log_patient_local("control_thread: mq_send (requeue) failed errno=" + std::to_string(errno));
                // If requeue fails, we still continue — message is lost but we logged it.
            }
            else
            {
                log_patient_local("control_thread: requeued control msg for pid=" + std::to_string(cm.target_pid) + " with prio=" + std::to_string(recv_prio));
            }

            continue; // next message
        }

        if (cm.cmd == CTRL_SPAWN_CHILD)
        {
            // g_child_threads.emplace_back(child_thread_fn, cm);
        }
        else if (cm.cmd == CTRL_GOTO_DOCTOR)
        {
            mqd_t mq_doctor = mq_open(MQ_DOCTOR_NAME, O_WRONLY | O_CLOEXEC);
            if (mq_doctor != (mqd_t)-1)
            {
                if (mq_send(
                        mq_doctor,
                        reinterpret_cast<char*>(&g_self),
                        sizeof(PatientInfo),
                        cm.priority) == -1)
                {
                    log_patient_local("CTRL_GOTO_DOCTOR: mq_send to doctor failed errno=" + std::to_string(errno));
                }
                else
                {
                    log_patient_local("CTRL_GOTO_DOCTOR queued (priority=" + std::to_string(cm.priority) + ")");
                }
                mq_close(mq_doctor);
            }
            else
            {
                log_patient_local("CTRL_GOTO_DOCTOR: mq_open doctor failed errno=" + std::to_string(errno));
            }
        }
        else if (cm.cmd == CTRL_DISMISS)
        {
            g_exit_reason.store(DISMISSED_BY_TRIAGE);
            g_running = false;
        }
        else if (cm.cmd == CTRL_SHUTDOWN)
        {
            g_running = false;
        }
        else
        {
            log_patient_local("control_thread: unknown cmd=" + std::to_string(static_cast<int>(cm.cmd)));
        }
    }

    if (ctrl_mq_wr != (mqd_t)-1)
        mq_close(ctrl_mq_wr);
    mq_close(ctrl_mq_rd);
}


static void sig_treated_handler(int)
{
    g_exit_reason.store(TREATED);
    g_running = false;
}

static void sigusr2_handler(int)
{
    g_exit_reason.store(EVACUATED);
    g_running = false;
}

static void sig_dismissed_handler(int)
{
    g_exit_reason.store(DISMISSED_BY_TRIAGE);
    g_running = false;
}

static bool setup_shm_local()
{
    g_shm_fd_local = shm_open(SHM_NAME, O_RDWR, 0);
    if (g_shm_fd_local == -1)
    {
        perror("shm_open (patient)");
        return false;
    }

    g_shm_local = static_cast<ERShared*>(mmap(nullptr, sizeof(ERShared), PROT_READ | PROT_WRITE, MAP_SHARED, g_shm_fd_local, 0));
    if (g_shm_local == MAP_FAILED)
    {
        perror("mmap (patient)");
        g_shm_local = nullptr;
        return false;
    }

    g_shm_sem_local = sem_open(SEM_SHM_NAME, 0);
    if (g_shm_sem_local == SEM_FAILED)
    {
        perror("sem_open (patient)");
        g_shm_sem_local = nullptr;
        return true;
    }

    return true;
}

static void cleanup_local()
{
    g_running = false;

    if (g_ctrl_thread.joinable())
    {
        g_ctrl_thread.join();
    }

    for (auto& t : g_child_threads)
    {
        if (t.joinable())
        {
            t.join();
        }
    }

    if (g_reg_mq != (mqd_t)-1)
    {
        mq_close(g_reg_mq);
        g_reg_mq = (mqd_t)-1;
    }

    if (g_shm_local)
    {
        if (munmap(g_shm_local, sizeof(ERShared)) == -1)
        {
            perror("munmap (patient)");
        }

        g_shm_local = nullptr;
    }

    if (g_shm_fd_local != -1)
    {
        if (close(g_shm_fd_local) == -1)
        {
            perror("close(shm_fd local)");
        }

        g_shm_fd_local = -1;
    }

    if (g_shm_sem_local && g_shm_sem_local != SEM_FAILED)
    {
        if (sem_close(g_shm_sem_local) == -1)
        {
            perror("sem_close (patient)");
        }

        g_shm_sem_local = nullptr;
    }

    // if (!g_ctrl_mq_name.empty())
    // {
    //     if (mq_unlink(g_ctrl_mq_name.c_str()) == -1 && errno != ENOENT)
    //     {
    //         perror("mq_unlink (patient ctrl)");
    //     }
    //     else
    //     {
    //         log_patient_local("cleanup_local: mq_unlink attempted for " + g_ctrl_mq_name);
    //     }
    // }
}

int main(const int argc, char** argv)
{
    try_raise_rlimit();

    if (argc < 4)
    {
        std::cerr << "patient <id> <age> <vip>\n";
        return 1;
    }

    const int  id     = atoi(argv[1]);
    const int  age    = atoi(argv[2]);
    const bool is_vip = atoi(argv[3]) != 0;

    if (!setup_shm_local())
    {
        log_patient_local("setup_shm_local: failed - continuing without shm/sem handles");
    }

    const pid_t mypid = getpid();

    // Start control thread
    g_ctrl_thread = std::thread(control_thread_fn);

    // Signal handlers
    struct sigaction sa1{}, sa2{}, sa3{};
    sa1.sa_handler = sig_treated_handler;
    sigemptyset(&sa1.sa_mask);
    sa1.sa_flags = 0;
    if (sigaction(SIGUSR1, &sa1, nullptr) == -1)
    {
        perror("sigaction SIGUSR1 (patient)");
    }

    sa2.sa_handler = sigusr2_handler; // Evacuation
    sigemptyset(&sa2.sa_mask);
    sa2.sa_flags = 0;
    if (sigaction(SIGUSR2, &sa2, nullptr) == -1)
    {
        perror("sigaction SIGUSR2 (patient)");
    }

    sa3.sa_handler = sig_dismissed_handler;
    sigemptyset(&sa3.sa_mask);
    sa3.sa_flags = 0;
    if (sigaction(SIGRTMIN + 3, &sa3, nullptr) == -1)
    {
        // Nothing...
    }

    setup_reg_mq_once();

    // Prepare our own PatientInfo and send registration (adult)
    g_self = {};
    g_self.id          = id;
    g_self.pid         = static_cast<int>(mypid);
    g_self.age         = age;
    g_self.is_vip      = is_vip;
    strncpy(g_self.symptoms, "adult symptoms", sizeof(g_self.symptoms) - 1);
    g_self.symptoms[sizeof(g_self.symptoms) - 1] = '\0';


    // Send registration
    if (const bool ok = send_registration(g_self); !ok)
    {
        log_patient_local("main: send_registration failed for adult id=" + std::to_string(g_self.id));
    }

    while (g_running)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    g_running = false;

    if (g_ctrl_thread.joinable())
    {
        g_ctrl_thread.join();
    }

    for (auto& t : g_child_threads)
    {
        if (t.joinable())
        {
            t.join();
        }
    }

    release_waiting_room_slot_once();

    if (g_shm_local && g_shm_sem_local)
    {
        if (const int remaining = g_registered_count.load(); remaining > 0)
        {
            log_patient_local("Final reclaim: attempting to release up to " + std::to_string(remaining) + " outstanding registrations");
            for (int i = 0; i < remaining; i++)
            {
                if (sem_wait(g_shm_sem_local) == -1)
                {
                    perror("sem_wait (patient final reclaim)");
                    break;
                }

                if (g_shm_local->current_inside > 0)
                {
                    g_shm_local->current_inside--;
                    log_patient_local("Final reclaim: decremented current_inside -> " + std::to_string(g_shm_local->current_inside));
                    g_registered_count.fetch_sub(1);
                }
                else
                {
                    if (sem_post(g_shm_sem_local) == -1)
                    {
                        perror("sem_post (patient final reclaim)");
                    }

                    break;
                }

                if (sem_post(g_shm_sem_local) == -1)
                {
                    perror("sem_post (patient final reclaim)");
                    break;
                }
            }
        }
    }

    cleanup_local();

    switch (g_exit_reason.load())
    {
        case TREATED:
        {
            log_patient_local("Exiting: treated by doctor");
        }
        break;
        case EVACUATED:
        {
            log_patient_local("Exiting: evacuated");
        }
        break;
        case DISMISSED_BY_TRIAGE:
        {
            log_patient_local("Exiting: dismissed by triage");
        }
        break;
        default:
        {
            log_patient_local("Exiting: other/unknown reason");
        }
        break;
    }
}