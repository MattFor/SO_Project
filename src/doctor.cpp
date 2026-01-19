#include <random>
#include <fstream>
#include <cstring>
#include <unistd.h>
#include <signal.h>

#include "../include/Utilities.h"

#include <sys/mman.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <semaphore.h>

static volatile sig_atomic_t evacuation       = 0;
static volatile sig_atomic_t leave_after_next = 0;
static FILE*                 doc_log          = nullptr;

static void sigusr2_handler(int)
{
    evacuation = 1;
}

static void sigusr1_handler(int)
{
    leave_after_next = 1;
}

static void log_doc(const std::string& s)
{
    if constexpr (!LOGGING)
    {
        return;
    }

    if (doc_log)
    {
        std::string t = timestamp() + " " + s + "\n";
        if (fwrite(t.c_str(), 1, t.size(), doc_log) < 0)
            perror("fwrite doc");
        fflush(doc_log);
    }
}

// shared control attachments
// --- shared control attachments (put near top of file, after includes) ----------
static sem_t*           shm_sem   = nullptr;
static ERShared*        ctrl      = nullptr;        // real shared header struct
static ControlRegistry* ctrl_reg  = nullptr;        // control registry after ERShared
static int              shm_fd    = -1;
static size_t           shm_size  = 0;

// Attach shared memory: map ERShared followed by ControlRegistry
static bool attach_shared_control()
{
    // open existing shared memory region (created by master)
    shm_fd = shm_open(SHM_NAME, O_RDWR, 0);
    if (shm_fd == -1)
    {
        perror("shm_open");
        return false;
    }

    // calculate expected size: ERShared + ControlRegistry
    shm_size = sizeof(ERShared) + sizeof(ControlRegistry);

    // map the entire region
    void* ptr = mmap(nullptr, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (ptr == MAP_FAILED)
    {
        perror("mmap");
        close(shm_fd);
        shm_fd = -1;
        return false;
    }

    // set pointers: ERShared at base, ControlRegistry immediately after
    ctrl = reinterpret_cast<ERShared*>(ptr);
    ctrl_reg = reinterpret_cast<ControlRegistry*>(
                   reinterpret_cast<char*>(ptr) + sizeof(ERShared));

    // open the named semaphore created by master
    shm_sem = sem_open(SEM_SHM_NAME, 0);
    if (shm_sem == SEM_FAILED)
    {
        perror("sem_open");
        munmap(ptr, shm_size);
        close(shm_fd);
        shm_fd = -1;
        shm_sem = nullptr;
        ctrl = nullptr;
        ctrl_reg = nullptr;
        return false;
    }

    return true;
}

static void detach_shared_control()
{
    if (shm_sem && shm_sem != SEM_FAILED)
    {
        sem_close(shm_sem);
        shm_sem = nullptr;
    }
    if (ctrl)
    {
        // unmap the same total size we mapped
        munmap(reinterpret_cast<void*>(ctrl), shm_size);
        ctrl = nullptr;
        ctrl_reg = nullptr;
    }
    if (shm_fd != -1)
    {
        close(shm_fd);
        shm_fd = -1;
    }
}


int main(const int argc, char** argv)
{
    int doc_id = 0;
    if (argc > 1)
    {
        doc_id = atoi(argv[1]);
    }

    doc_log = fopen("../../logs/doctor.log", "a");
    if (!doc_log)
    {
        perror("fopen doctor.log");
        return 1;
    }

    if (!attach_shared_control()) {
        fprintf(stderr, "Failed to attach shared control; exiting.\n");
        return 1;
    }

    struct sigaction sa1{}, sa2{};
    sa1.sa_handler = sigusr1_handler;
    sigemptyset(&sa1.sa_mask);
    sa1.sa_flags = 0;
    if (sigaction(SIGUSR1, &sa1, nullptr) == -1)
    {
        perror("sigaction SIGUSR1");
    }

    sa2.sa_handler = sigusr2_handler;
    sigemptyset(&sa2.sa_mask);
    sa2.sa_flags = 0;
    if (sigaction(SIGUSR2, &sa2, nullptr) == -1)
    {
        perror("sigaction SIGUSR2");
    }

    const mqd_t mq_doc = mq_open(MQ_DOCTOR_NAME, O_RDONLY);
    if (mq_doc == (mqd_t)-1)
    {
        perror("mq_open doctor read");
        detach_shared_control();
        return 1;
    }

    std::default_random_engine     rng(static_cast<unsigned>(time(nullptr)) + doc_id);
    std::uniform_real_distribution uni(0.0, 1.0);

    char         buf[MAX_MSG_SIZE];
    unsigned int prio;

    // announce online
    if (shm_sem && ctrl) {
        if (sem_wait(shm_sem) == -1)
        {
            perror("sem_wait");
        }
        else
        {
            ctrl->doctors_online++;
            if (sem_post(shm_sem) == -1)
                perror("sem_post");
        }
    }

    while (!evacuation)
    {
        const ssize_t r = mq_receive(mq_doc, buf, MAX_MSG_SIZE, &prio);
        if (r == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }

            perror("mq_receive doc");
            break;
        }

        if (r < static_cast<ssize_t>(sizeof(PatientInfo)))
        {
            log_doc("Short msg ignored");
            continue;
        }

        PatientInfo p{};
        memcpy(&p, buf, sizeof(PatientInfo));
        log_doc("Doctor " + std::to_string(doc_id) + " started treating patient id=" + std::to_string(p.id));
        // Simulate treatment time
        const int treat_ms = rng() % 800 + 200;
         usleep(treat_ms);

	if (p.age < 18)
        {
            if (shm_sem && ctrl)
            {
                if (sem_wait(shm_sem) == -1)
                {
                    perror("sem_wait (doctor account child)");
                }
                else
                {
                    ++ctrl->total_treated;
                    if (sem_post(shm_sem) == -1)
                        perror("sem_post (doctor account child)");
                }
            }


            log_doc("Doctor treated child id=" + std::to_string(p.id) + " (no signal to parent)");
            // Do NOT send SIGUSR1/SIGUSR2 to parent here — parent should stay alive.
            continue; // go to next patient
        }

        // Aftercare probabilities
        if (const double x = uni(rng); x < 0.005)
        {
            log_doc("Patient id=" + std::to_string(p.id) + " directed to another hospital");
            if (p.pid > 0)
            {
                kill(p.pid, SIGUSR2);
            }
        }
        else if (x < 0.005 + 0.145)
        {
            log_doc("Patient id=" + std::to_string(p.id) + " submitted for deeper hospital care");
            if (p.pid > 0)
            {
                kill(p.pid, SIGUSR1);
            }
        }
        else
        {
            log_doc("Patient id=" + std::to_string(p.id) + " dismissed to go home");
            if (p.pid > 0)
            {
                kill(p.pid, SIGUSR1);
            }
        }

        // If leave_after_next flag set, go out for random time then return
        if (leave_after_next)
        {
            leave_after_next = 0;
            log_doc("Doctor " + std::to_string(doc_id) + " going out (SIGUSR1) after finishing patient.");
            const int out_ms = rng() % 2000 + 500;
            usleep(out_ms * 1000);
            log_doc("Doctor " + std::to_string(doc_id) + " returned from break.");
        }
    }

    // announce offline
    if (shm_sem && ctrl) {
        if (sem_wait(shm_sem) == -1)
        {
            perror("sem_wait");
        }
        else
        {
            ctrl->doctors_online--;
            if (sem_post(shm_sem) == -1)
                perror("sem_post");
        }
    }

    detach_shared_control();

    log_doc("Doctor " + std::to_string(doc_id) + " exiting due to evacuation.");
    mq_close(mq_doc);
    fclose(doc_log);
}
