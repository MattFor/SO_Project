//
// Created by MattFor on 21/12/2025.
//

#include <random>
#include <cerrno>
#include <fstream>
#include <cstring>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <semaphore.h>

#include "../include/Utilities.h"

static FILE* triage_log = nullptr;

static void log_tri(const std::string& s)
{
    if constexpr (!LOGGING)
    {
        return;
    }

    if (triage_log)
    {
        std::string t = timestamp() + " " + s + "\n";
        fwrite(t.c_str(), 1, t.size(), triage_log);
        fflush(triage_log);
    }
}

// --- shared control attachments (put near top of file, after includes) ----------
static sem_t*           shm_sem  = nullptr;
static ERShared*        ctrl     = nullptr; // real shared header struct
static ControlRegistry* ctrl_reg = nullptr; // control registry after ERShared
static int              shm_fd   = -1;
static size_t           shm_size = 0;

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
    ctrl     = static_cast<ERShared*>(ptr);
    ctrl_reg = reinterpret_cast<ControlRegistry*>(static_cast<char*>(ptr) + sizeof(ERShared));

    // open the named semaphore created by master
    shm_sem = sem_open(SEM_SHM_NAME, 0);
    if (shm_sem == SEM_FAILED)
    {
        perror("sem_open");
        munmap(ptr, shm_size);
        close(shm_fd);
        shm_fd   = -1;
        shm_sem  = nullptr;
        ctrl     = nullptr;
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
        ctrl     = nullptr;
        ctrl_reg = nullptr;
    }

    if (shm_fd != -1)
    {
        close(shm_fd);
        shm_fd = -1;
    }
}


int main()
{
    triage_log = fopen("../../logs/triage.log", "a");
    if (!triage_log)
    {
        perror("fopen triage.log");
        return 1;
    }

    if (!attach_shared_control())
    {
        fprintf(stderr, "Failed to attach shared control; exiting.\n");
        return 1;
    }

    const mqd_t mq_triage = mq_open(MQ_TRIAGE_NAME, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (mq_triage == (mqd_t)-1)
    {
        perror("mq_open triage");
        return 1;
    }

    std::default_random_engine             rng(static_cast<unsigned>(time(nullptr)));
    std::uniform_real_distribution uni(0.0, 1.0);

    char buf[MAX_MSG_SIZE];

    while (true)
    {
        sem_wait(shm_sem);
        const int docs = ctrl->doctors_online;
        sem_post(shm_sem);

        if (docs == 0)
        {
            // avoid busy-spin when no doctors: small sleep
            usleep(5 * 1000);
            continue;
        }

        // Now try to receive a triage message (non-blocking)
        unsigned int  prio = 0;
        const ssize_t r    = mq_receive(mq_triage, buf, MAX_MSG_SIZE, &prio);


        if (r == -1)
        {
            if (errno == EAGAIN || errno == EINTR)
            {
                usleep(5 * 1000);
                continue;
            }

            perror("mq_receive triage");
            break;
        }

        if (r < static_cast<ssize_t>(sizeof(PatientInfo)))
        {
            log_tri("Received malformed PatientInfo (ignored)");
            continue;
        }

        PatientInfo p{};
        memcpy(&p, buf, sizeof(PatientInfo));

        // 5% immediate dismiss
        if (uni(rng) < 0.05)
        {
            if (p.pid > 0)
            {
                ControlMessage cm{};
                cm.cmd        = CTRL_DISMISS;
                cm.target_pid = p.pid;
                cm.target_id  = p.id;
                cm.priority   = 0;

                if (mqd_t mq_ctrl = mq_open(MQ_PATIENT_CTRL, O_WRONLY | O_CLOEXEC); mq_ctrl != (mqd_t)-1)
                {
                    mq_send(mq_ctrl, reinterpret_cast<const char*>(&cm), sizeof( cm ), 0);
                    mq_close(mq_ctrl);
                    log_tri("Triaged patient id=" + std::to_string(p.id) + " sent CTRL_DISMISS to dispatcher");
                }

                log_tri("Dismissed patient id=" + std::to_string(p.id) + " pid=" + std::to_string(p.pid));
            }

            continue;
        }

        unsigned int send_prio;
        const double c = uni(rng);
        if (c < 0.10)
        {
            send_prio = 8;
        }
        else if (c < 0.45)
        {
            send_prio = 5;
        }
        else
        {
            send_prio = 1;
        }

        ControlMessage cm{};
        cm.cmd        = CTRL_GOTO_DOCTOR;
        cm.target_pid = p.pid;
        cm.target_id  = p.id;
        cm.priority   = send_prio;

        // 🔑 OPEN SHARED CONTROL MQ PER SEND (DO NOT CACHE)
        const mqd_t mq_ctrl = mq_open(MQ_PATIENT_CTRL, O_WRONLY | O_CLOEXEC);

        if (mq_ctrl == (mqd_t)-1)
        {
            log_tri("FAILED mq_open MQ_PATIENT_CTRL errno=" + std::to_string(errno));
            continue;
        }

        if (mq_send(mq_ctrl, reinterpret_cast<const char*>(&cm), sizeof( cm ), 0) == -1)
        {
            log_tri("FAILED CTRL_GOTO_DOCTOR id=" + std::to_string(p.id) + " pid=" + std::to_string(p.pid) + " errno=" + std::to_string(errno));
        }
        else
        {
            log_tri("Triaged patient id=" + std::to_string(p.id) + " pid=" + std::to_string(p.pid) + " prio=" + std::to_string(send_prio));
        }

        mq_close(mq_ctrl);
    }

    detach_shared_control();

    mq_close(mq_triage);
    fclose(triage_log);
    return 0;
}