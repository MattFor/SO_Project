

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

static std::atomic_bool g_accounted{false};

static std::vector<PatientInfo> g_active_children;
static std::mutex g_children_mutex;
// Registration MQ reused for this process
static mqd_t g_reg_mq = (mqd_t)-1;

// Local copies of shared memory handles (for child threads to increment waiting_to_register)
static int       g_shm_fd_local  = -1;
static ERShared* g_shm_local     = nullptr;
static sem_t*    g_shm_sem_local = nullptr;

// Ensure current_inside is decremented exactly once by this patient process
static bool       slot_released = false;
static std::mutex slot_mutex;

static int g_ctrl_slot = -1;
static ControlRegistry* g_ctrl_reg = nullptr;

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

static std::atomic<ExitReason> g_exit_reason{NONE};

enum class PatientState : uint8_t
{
    REGISTERED,
    WAITING_TRIAGE,
    SENT_TO_DOCTOR,
    TREATED,
    DISMISSED
};

static std::atomic<PatientState> g_state{PatientState::REGISTERED};
static std::atomic<uint64_t>     g_state_since_ns{0};

static inline uint64_t now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static inline void set_state(PatientState s)
{
    g_state.store(s, std::memory_order_release);
    g_state_since_ns.store(now_ns(), std::memory_order_relaxed);
}
/*
static inline bool should_count_as_processed()
{
    const auto state = g_state.load(std::memory_order_acquire);
    const auto exit  = g_exit_reason.load(std::memory_order_acquire);

    // Must have been admitted / visible to the system
if (g_exit_reason.load() == NONE)
    return false;

    // Only count terminal decisions
    switch (exit)
    {
        case TREATED:
        case DISMISSED_BY_TRIAGE:
        case EVACUATED:
            return true;
        default:
            return false;
    }
}*/
static inline bool should_count_as_processed()
{
    switch (g_exit_reason.load(std::memory_order_acquire))
    {
        case TREATED:
        case DISMISSED_BY_TRIAGE:
        case EVACUATED:
            return true;
        default:
            return false;
    }
}



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

static void commit_patient_exit()
{
    bool expected = false;
    if (!g_accounted.compare_exchange_strong(expected, true))
        return;

    if (!should_count_as_processed())
        return;

    if (!g_shm_local || !g_shm_sem_local)
        return;

    if (sem_wait(g_shm_sem_local) == -1)
        return;

    ++g_shm_local->total_treated; // now "patients_processed"

    sem_post(g_shm_sem_local);
}



static bool setup_control_registry()
{
    // Assume shm already mmapped by setup_shm_local; we need to set g_ctrl_reg pointer.
    // We used the layout: ERShared at start, followed by ControlRegistry.
    // Need to remap or compute pointer from g_shm_local (ERShared*)
    if (!g_shm_local)
        return false;

    g_ctrl_reg = reinterpret_cast<ControlRegistry*>(
        reinterpret_cast<char*>(g_shm_local) + sizeof(ERShared));
    if (!g_ctrl_reg)
        return false;

    // Try to claim a slot: look for an empty slot (pid==0).
    // Use simple linear search starting from alloc_cursor to spread allocations.
    uint32_t start = g_ctrl_reg->alloc_cursor.fetch_add(1) % CTRL_REGISTRY_SIZE;
    for (size_t i = 0; i < CTRL_REGISTRY_SIZE; ++i)
    {
        size_t idx = (start + i) % CTRL_REGISTRY_SIZE;
        pid_t expected = 0;
        if (std::atomic_compare_exchange_strong(
                reinterpret_cast<std::atomic<pid_t>*>(&g_ctrl_reg->slots[idx].pid),
                &expected,
                static_cast<pid_t>(getpid())))
        {
            g_ctrl_slot = static_cast<int>(idx);

            // mark seq = 0 (empty)
            g_ctrl_reg->slots[idx].seq.store(0, std::memory_order_relaxed);

            // ensure semaphore for bucket exists (create if necessary)
            const size_t bucket = slot_to_bucket(idx);
            const std::string sname = ctrl_sem_name(bucket);
            sem_t* sem = sem_open(sname.c_str(), O_CREAT, IPC_MODE, 0);
            if (sem == SEM_FAILED)
            {
                perror("sem_open patient register");
                // continue: patient can still poll
            }
            else
            {
                sem_close(sem);
            }

            return true;
        }
    }
    // failed to claim a slot
    return false;
}

static void release_single_slot() {
    if (sem_wait(g_shm_sem_local) != -1) {
        if (g_shm_local->current_inside > 0) {
            g_shm_local->current_inside--;
        }
        sem_post(g_shm_sem_local);
    }
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
    if (slot_released) return;
    slot_released = true;

    if (!g_shm_local || !g_shm_sem_local) return;

    // Lock ONCE for the entire cleanup operation
    if (sem_wait(g_shm_sem_local) == -1) {
        perror("sem_wait (patient release)");
        return;
    }

    // Now it's safe to loop because we hold the lock
    while (g_registered_count.load() > 0)
    {
        if (g_shm_local->current_inside > 0) {
            g_shm_local->current_inside--;
            g_registered_count.fetch_sub(1);
            log_patient_local("Released slot. Remaining for this family: " +
                              std::to_string(g_registered_count.load()));
        } else {
            // Building is already empty, nothing left to decrement
            break;
        }
    }

    // Release ONCE after the loop finishes
    sem_post(g_shm_sem_local);
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

    if (p.pid != 0) {
    g_registered_count.fetch_add(1);
}

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
    p.pid    = getpid(); // 🔑 FIX: Use Parent PID so Dispatcher can find the slot
    p.age    = cm.child_age;
    p.is_vip = cm.child_vip;
    strncpy(p.symptoms, cm.symptoms, sizeof(p.symptoms) - 1);
    p.symptoms[sizeof(p.symptoms) - 1] = '\0';

    {
        std::lock_guard<std::mutex> lock(g_children_mutex);
        g_active_children.push_back(p);
    }

    log_patient_local("Spawned child-thread id=" + std::to_string(p.id) + " using Parent PID=" + std::to_string(p.pid));

    if (!send_registration(p)) {
        //g_registered_count.fetch_add(1);
        log_patient_local("child_thread: registration failed for child id=" + std::to_string(p.id));
    }
}

// Control thread that listens for control messages on per-patient MQ
// It opens the MQ by name and uses mq_timedreceive with a short timeout so it can
// Check the g_running flag regularly and exit quickly when requested
static void control_thread_fn()
{
	 if (!g_ctrl_reg || g_ctrl_slot < 0)
    {
        log_patient_local("control_thread: no control registry available; exiting");
        return;
    }

    const size_t bucket = slot_to_bucket(static_cast<size_t>(g_ctrl_slot));
    const std::string sname = ctrl_sem_name(bucket);

    // Open the semaphore once and reuse
    sem_t* sem = sem_open(sname.c_str(), 0);
    if (sem == SEM_FAILED)
    {
        log_patient_local("control_thread: sem_open failed, will fallback to polling");
    }

    while (g_running)
    {
        bool woke = false;

        if (sem != SEM_FAILED)
        {
            // Wait until dispatcher posts the bucket
            if (sem_wait(sem) == -1)
            {
                if (errno == EINTR) continue;
                perror("sem_wait patient ctrl");
                break;
            }
            woke = true;
        }
        else
        {
            // fallback: simple sleep/poll
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            woke = true;
        }

        if (!g_running) break;

        // check our slot
        ControlSlot& slot = g_ctrl_reg->slots[static_cast<size_t>(g_ctrl_slot)];
        const uint32_t seq = slot.seq.load(std::memory_order_acquire);
        if (seq == 0)
        {
            // spurious wake or no msg
            continue;
        }

        // copy message locally
        ControlMessage cm;
        cm = slot.msg;

        // clear slot (set seq to 0)
        slot.seq.store(0, std::memory_order_release);

        // Handle message (saqme handling as before)
        if (cm.cmd == CTRL_SPAWN_CHILD)
        {
            // create child thread
           // g_child_threads.emplace_back(child_thread_fn, cm);
        }
       else if (cm.cmd == CTRL_GOTO_DOCTOR)
        {
            // If this control message targets a child (target_id != 0 and not the adult id),
            // locate the child in g_active_children and forward the child's PatientInfo to doctor MQ.
if (cm.target_id != 0 && cm.target_id != g_self.id)
    {
        PatientInfo child{};
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(g_children_mutex);
            for (auto it = g_active_children.begin(); it != g_active_children.end(); ++it)
            {
                if (it->id == cm.target_id)
                {
                    child = *it;
                    g_active_children.erase(it);
                    found = true;
                    break;
                }
            }
        }

        if (!found)
        {
            log_patient_local("CTRL_GOTO_DOCTOR: target child id not found id=" + std::to_string(cm.target_id));
        }
        else
        {
            // Bookkeeping: decrement local registered count and shared current_inside
            g_registered_count.fetch_sub(1);

            if (g_shm_local && g_shm_sem_local)
            {
                if (sem_wait(g_shm_sem_local) != -1)
                {
                    if (g_shm_local->current_inside > 0)
                        g_shm_local->current_inside--;
                    if (g_shm_local->waiting_to_register > 0)
                        g_shm_local->waiting_to_register--; // optional: if you incremented earlier
                    sem_post(g_shm_sem_local);
                }
                else
                {
                    perror("sem_wait (patient forward child)");
                }
            }

            // Forward child to doctor MQ
            mqd_t mq_doctor = mq_open(MQ_DOCTOR_NAME, O_WRONLY | O_CLOEXEC);
            if (mq_doctor != (mqd_t)-1)
            {
                if (mq_send(mq_doctor, reinterpret_cast<char*>(&child), sizeof(PatientInfo), cm.priority) == -1)
                {
                    log_patient_local("CTRL_GOTO_DOCTOR: mq_send child to doctor failed errno=" + std::to_string(errno));
                }
                else
                {
                    log_patient_local("CTRL_GOTO_DOCTOR queued for child id=" + std::to_string(child.id) + " (priority=" + std::to_string(cm.priority) + ")");
                }
                mq_close(mq_doctor);
            }
            else
            {
                log_patient_local("CTRL_GOTO_DOCTOR: mq_open doctor failed errno=" + std::to_string(errno));
            }
        }
    }
            else
            {
                // Default: this targets the adult patient — unchanged behavior
                set_state(PatientState::SENT_TO_DOCTOR);
                mqd_t mq_doctor = mq_open(MQ_DOCTOR_NAME, O_WRONLY | O_CLOEXEC);
                if (mq_doctor != (mqd_t)-1)
                {
                    if (mq_send(mq_doctor, reinterpret_cast<char*>(&g_self), sizeof(PatientInfo), cm.priority) == -1)
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
        }
        else if (cm.cmd == CTRL_DISMISS)
        {
set_state(PatientState::DISMISSED);
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

    if (sem != SEM_FAILED)
        sem_close(sem);
}


static void sig_treated_handler(int)
{
set_state(PatientState::TREATED);
    g_exit_reason.store(TREATED);
    g_running = false;
}

static void sigusr2_handler(int)
{
 set_state(PatientState::DISMISSED);
    g_exit_reason.store(EVACUATED);
    g_running = false;
}

static void sig_dismissed_handler(int)
{
 set_state(PatientState::DISMISSED);
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

    const size_t total_shm_sz = sizeof(ERShared) + sizeof(ControlRegistry);

    void* p = mmap(nullptr, total_shm_sz, PROT_READ | PROT_WRITE, MAP_SHARED, g_shm_fd_local, 0);
    if (p == MAP_FAILED)
    {
        perror("mmap (patient)");
        g_shm_local = nullptr;
        return false;
    }

    // ERShared is at the start, ControlRegistry follows immediately
    g_shm_local = reinterpret_cast<ERShared*>(p);
    g_ctrl_reg  = reinterpret_cast<ControlRegistry*>(
                      reinterpret_cast<char*>(p) + sizeof(ERShared));

    g_shm_sem_local = sem_open(SEM_SHM_NAME, 0);
    if (g_shm_sem_local == SEM_FAILED)
    {
        perror("sem_open (patient)");
        g_shm_sem_local = nullptr;
        // still return true because shared memory mapping succeeded
    }

    return true;
}


static void cleanup_local()
{
release_waiting_room_slot_once();
	commit_patient_exit();
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

if (g_ctrl_reg && g_ctrl_slot >= 0)
    {
        // set pid back to 0 atomically
        g_ctrl_reg->slots[static_cast<size_t>(g_ctrl_slot)].pid.store(0, std::memory_order_release);

        // Optionally bump seq to zero (already should be 0 after processing)
        g_ctrl_reg->slots[static_cast<size_t>(g_ctrl_slot)].seq.store(0, std::memory_order_release);

        // post the bucket semaphore once in case anyone is waiting for this slot to appear/disappear
        const size_t bucket = slot_to_bucket(static_cast<size_t>(g_ctrl_slot));
        const std::string sname = ctrl_sem_name(bucket);
        sem_t* sem = sem_open(sname.c_str(), 0);
        if (sem != SEM_FAILED)
        {
            sem_post(sem);
            sem_close(sem);
        }

        g_ctrl_slot = -1;
    }

    if (g_shm_local)
    {
        const size_t total_shm_sz = sizeof(ERShared) + sizeof(ControlRegistry);
if (munmap(g_shm_local, total_shm_sz) == -1)
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
atexit(cleanup_local);
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

	if (g_shm_local)
{
    if (!setup_control_registry())
    {
        log_patient_local("setup_control_registry: failed to claim a slot (continuing, but will not receive control messages)");
        // We still proceed, but control thread will notice no slot and exit
    }
    else
    {
        log_patient_local("setup_control_registry: claimed slot=" + std::to_string(g_ctrl_slot));
    }
}


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

	set_state(PatientState::WAITING_TRIAGE);

constexpr uint64_t STUCK_TIMEOUT_NS =
    3ull * 1000 * 1000 * 1000; // 2 seconds

while (g_running)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const auto     state = g_state.load(std::memory_order_acquire);
    const uint64_t idle  = now_ns() - g_state_since_ns.load();

    // Stuck waiting for triage decision
    if ((state == PatientState::REGISTERED ||
         state == PatientState::WAITING_TRIAGE) &&
        idle > STUCK_TIMEOUT_NS)
    {
        log_patient_local("WATCHDOG: stuck before triage → self-dismiss");
        set_state(PatientState::DISMISSED);
        g_exit_reason.store(DISMISSED_BY_TRIAGE);
        break;
    }

    // Stuck after being sent to doctor
    if (state == PatientState::SENT_TO_DOCTOR &&
        idle > STUCK_TIMEOUT_NS)
    {
        log_patient_local("WATCHDOG: doctor never treated → self-dismiss");
        set_state(PatientState::DISMISSED);
        g_exit_reason.store(DISMISSED_BY_TRIAGE);
        break;
    }
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

    //release_waiting_room_slot_once();
/*
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
*/
	//commit_patient_exit();

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