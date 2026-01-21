//
// Created by MattFor on 21/12/2025.
//

#include <fcntl.h>
#include <fstream>
#include <cstring>
#include <unistd.h>
#include <iostream>

#include "../include/Utilities.h"

static ERShared* g_shm     = nullptr;
static int       g_shm_fd  = -1;
static sem_t*    g_shm_sem = nullptr;
static FILE*     reg_log   = nullptr;
static int       window_id = 1;

static void log_reg(const std::string& s)
{
    if constexpr (!LOGGING)
    {
        return;
    }

    if (reg_log)
    {
        const std::string t = timestamp() + " " + s + "\n";
        if (fwrite(t.c_str(), 1, t.size(), reg_log) < 0)
        {
            perror("fwrite reg");
        }
        fflush(reg_log);
    }
}

static void cleanup()
{
    if (reg_log)
    {
        fclose(reg_log);
    }

    if (g_shm)
    {
        munmap(g_shm, sizeof(ERShared));
        g_shm = nullptr;
    }

    if (g_shm_fd != -1)
    {
        close(g_shm_fd);
        g_shm_fd = -1;
    }

    if (g_shm_sem)
    {
        sem_close(g_shm_sem);
        g_shm_sem = nullptr;
    }
}

int main(const int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "registration <window_id>\n";
        return 1;
    }

    window_id = atoi(argv[1]);
    reg_log   = fopen("../../logs/registration.log", "a");
    if (!reg_log)
    {
        perror("fopen registration.log");
        return 1;
    }

    // Open shared memory
    g_shm_fd = shm_open(SHM_NAME, O_RDWR, 0);
    if (g_shm_fd == -1)
    {
        perror("shm_open reg");
        return 1;
    }

    g_shm = static_cast<ERShared*>(mmap(nullptr, sizeof(ERShared), PROT_READ | PROT_WRITE, MAP_SHARED, g_shm_fd, 0));

    if (g_shm == MAP_FAILED)
    {
        perror("mmap reg");
        return 1;
    }

    g_shm_sem = sem_open(SEM_SHM_NAME, 0);
    if (g_shm_sem == SEM_FAILED)
    {
        perror("sem_open reg");
        return 1;
    }

    // Open message queues
    const mqd_t mq_reg = mq_open(MQ_REG_NAME, O_RDONLY | O_CLOEXEC);
    if (mq_reg == (mqd_t)-1)
    {
        perror("mq_open reg read");
        cleanup();
        return 1;
    }

    const mqd_t mq_triage = mq_open(MQ_TRIAGE_NAME, O_WRONLY | O_CLOEXEC);
    if (mq_triage == (mqd_t)-1)
    {
        perror("mq_open triage write");
        cleanup();
        return 1;
    }

    log_reg("Registration window " + std::to_string(window_id) + " started");

    char         buf[MAX_MSG_SIZE];
    unsigned int prio;
    while (true)
    {
        // If evacuation flag set, exit
        if (sem_wait(g_shm_sem) == -1)
        {
            perror("sem_wait reg");
        }

        const bool evac = g_shm->evacuation;
        if (sem_post(g_shm_sem) == -1)
        {
            perror("sem_post reg");
        }

        if (evac)
        {
            log_reg("Evacuation detected; registration exiting.");
            break;
        }

        // Try to receive a patient (blocking)
        const ssize_t r = mq_receive(mq_reg, buf, MAX_MSG_SIZE, &prio);
        if (r == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }

            perror("mq_receive reg");
            break;
        }

        if (r < static_cast<ssize_t>(sizeof(PatientInfo)))
        {
            // Ignore malformed messages
            log_reg("Received short message (ignored).");
            continue;
        }

        PatientInfo p{};
        memcpy(&p, buf, sizeof(PatientInfo));
        if (sem_wait(g_shm_sem) == -1)
        {
            perror("sem_wait reg2");
        }

        if (g_shm->current_inside < g_shm->N_waiting_room)
        {
            g_shm->current_inside++;
            if (p.pid > 0)
            {
                ControlMessage cm{};
                cm.cmd        = CTRL_INSIDE;
                cm.target_pid = p.pid;

                const mqd_t mq_ctrl = mq_open(MQ_PATIENT_CTRL, O_WRONLY | O_CLOEXEC);

                if (mq_ctrl != (mqd_t)-1)
                {
                    if (mq_send(mq_ctrl, reinterpret_cast<const char*>(&cm), sizeof( cm ), 0) == -1)
                    {
                        log_reg("Failed to send CTRL_INSIDE to pid=" + std::to_string(p.pid) + " errno=" + std::to_string(errno));
                    }
                    else
                    {
                        log_reg("Sent CTRL_INSIDE to pid=" + std::to_string(p.pid));
                    }

                    mq_close(mq_ctrl);
                }
                else
                {
                    log_reg("Failed to open MQ_PATIENT_CTRL errno=" + std::to_string(errno));
                }
            }
        }
        else
        {
            // Waiting room full, patient remains outside, they still count as waiting_to_register
        }

        if (g_shm->waiting_to_register > 0)
        {
            g_shm->waiting_to_register--;
        }

        if (sem_post(g_shm_sem) == -1)
        {
            perror("sem_post reg2");
        }

        log_reg("Registered patient id=" + std::to_string(p.id) + " age=" + std::to_string(p.age) + ( p.is_vip ? " VIP" : "" ));

        // Forward to triage
        if (mq_send(mq_triage, buf, sizeof(PatientInfo), prio) == -1)
        {
            perror("mq_send to triage");
        }
        else
        {
            log_reg("Forwarded patient id=" + std::to_string(p.id) + " to triage");
        }

        // Self-exit policy: if this is window 2 it should check whether it should remain open
        if (window_id == 2)
        {
            if (sem_wait(g_shm_sem) == -1)
            {
                perror("sem_wait reg3");
            }

            if (const int waiting = g_shm->waiting_to_register; waiting < g_shm->N_waiting_room / 3)
            {
                // Close this window
                if (sem_post(g_shm_sem) == -1)
                {
                    perror("sem_post reg3");
                }

                log_reg("Window 2 closing due to waiting < N/3");
                break;
            }

            if (sem_post(g_shm_sem) == -1)
            {
                perror("sem_post reg3");
            }
        }
    }

    mq_close(mq_reg);
    mq_close(mq_triage);
    cleanup();
}