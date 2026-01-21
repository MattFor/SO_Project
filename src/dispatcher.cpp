//
// Created by MattFor on 19.01.2026.
//

#include <cstdio>
#include <string>
#include <fcntl.h>
#include <cstring>
#include <mqueue.h>
#include <unistd.h>
#include <iostream>
#include <sys/mman.h>
#include <semaphore.h>

#include "../include/Utilities.h"

static int              g_shm_fd       = -1;
static ControlRegistry* g_ctrl_reg     = nullptr;
static FILE*            dispatcher_log = nullptr;

static void log_dispatcher(const std::string& s)
{
    if constexpr (!LOGGING)
    {
        return;
    }

    if (!dispatcher_log)
    {
        if (const int dfd = open("../../logs/dispatcher.log", O_CREAT | O_WRONLY | O_APPEND, IPC_MODE); dfd != -1)
        {
            dispatcher_log = fdopen(dfd, "a");
        }
    }

    if (dispatcher_log)
    {
        std::string t = timestamp() + " " + s + "\n";
        if (fwrite(t.c_str(), 1, t.size(), dispatcher_log) < 0)
        {
            perror("fwrite dispatcher.log");
        }

        fflush(dispatcher_log);
    }
}

static bool open_shm_registry()
{
    g_shm_fd = shm_open(SHM_NAME, O_RDWR, 0);
    if (g_shm_fd == -1)
    {
        perror("shm_open dispatcher");
        if (dispatcher_log)
        {
            log_dispatcher(std::string("shm_open failed errno=") + std::to_string(errno));
        }

        return false;
    }

    void* p = mmap(nullptr, sizeof(ERShared) + sizeof(ControlRegistry), PROT_READ | PROT_WRITE, MAP_SHARED, g_shm_fd, 0);
    if (p == MAP_FAILED)
    {
        perror("mmap dispatcher");
        if (dispatcher_log)
        {
            log_dispatcher(std::string("mmap failed errno=") + std::to_string(errno));
        }

        return false;
    }

    g_ctrl_reg = reinterpret_cast<ControlRegistry*>(static_cast<char*>(p) + sizeof(ERShared));

    if (dispatcher_log)
    {
        log_dispatcher("open_shm_registry: success");
    }

    return true;
}

static ssize_t find_slot_for_pid(pid_t pid)
{
    for (size_t i = 0; i < CTRL_REGISTRY_SIZE; ++i)
    {
        if (const pid_t owner = g_ctrl_reg->slots[i].pid; owner == pid)
        {
            return static_cast<ssize_t>(i);
        }
    }
    return -1;
}

int main()
{
    if constexpr (LOGGING)
    {
        if (const int dfd = open("../../logs/dispatcher.log", O_CREAT | O_WRONLY | O_APPEND, IPC_MODE); dfd == -1)
        {
            perror("open dispatcher.log");
        }
        else
        {
            dispatcher_log = fdopen(dfd, "a");
            if (!dispatcher_log)
            {
                perror("fdopen dispatcher.log");
                close(dfd);
            }
            else
            {
                log_dispatcher("Dispatcher starting up");
            }
        }
    }

    if (!open_shm_registry())
    {
        log_dispatcher("open_shm_registry failed; exiting");
        return 1;
    }

    const mqd_t mq_ctrl = mq_open(MQ_PATIENT_CTRL, O_RDONLY | O_CLOEXEC);
    if (mq_ctrl == ( mqd_t ) - 1)
    {
        perror("mq_open MQ_PATIENT_CTRL");
        log_dispatcher(std::string("mq_open(MQ_PATIENT_CTRL) failed errno=") + std::to_string(errno));
        if (dispatcher_log)
        {
            fclose(dispatcher_log);
        }
        return 1;
    }

    log_dispatcher("mq_open(MQ_PATIENT_CTRL) success; entering receive loop");

    char         buf[sizeof(ControlMessage)];
    unsigned int prio;

    while (true)
    {
        const ssize_t r = mq_receive(mq_ctrl, buf, sizeof( buf ), &prio);
        if (r == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }
            perror("mq_receive dispatcher");
            log_dispatcher(std::string("mq_receive failed errno=") + std::to_string(errno));
            break;
        }

        if (r < static_cast<ssize_t>(sizeof(ControlMessage)))
        {
            log_dispatcher("mq_receive: short/malformed message ignored");
            continue;
        }

        ControlMessage cm{};
        memcpy(&cm, buf, sizeof( cm ));

        if (cm.target_pid == 0)
        {
            log_dispatcher("mq_receive: msg with target_pid==0 (broadcast) ignored");
            continue;
        }

        if (const ssize_t slot_idx = find_slot_for_pid(cm.target_pid); slot_idx >= 0)
        {
            ControlSlot& slot = g_ctrl_reg->slots[slot_idx];

            if (!slot.rdy.load(std::memory_order_acquire))
            {
                log_dispatcher("Slot found for pid=" + std::to_string(cm.target_pid) + " but slot not ready — deferring message");
                continue;
            }

            // Write message and publish via seq increment (non-zero => available)
            const uint32_t nextseq = slot.seq.load(std::memory_order_relaxed) + 1;
            slot.msg               = cm;
            std::atomic_thread_fence(std::memory_order_release);
            slot.seq.store(nextseq ? nextseq : 1, std::memory_order_release);

            const size_t      bucket = slot_to_bucket(static_cast<size_t>(slot_idx));
            const std::string sname  = ctrl_sem_name(bucket);
            if (sem_t* sem = sem_open(sname.c_str(), 0); sem != SEM_FAILED)
            {
                if (sem_post(sem) == -1)
                {
                    log_dispatcher(std::string("sem_post failed for ") + sname + " errno=" + std::to_string(errno));
                }
                else
                {
                    log_dispatcher(std::string("Posted bucket semaphore ") + sname + " for slot " + std::to_string(slot_idx));
                }
                sem_close(sem);
            }
            else
            {
                log_dispatcher(std::string("sem_open failed for ") + sname + " errno=" + std::to_string(errno));
            }
        }
        else
        {
            log_dispatcher(std::string("No slot found for pid=") + std::to_string(cm.target_pid) + " — dropping message");
        }
    }

    mq_close(mq_ctrl);

    if (dispatcher_log)
    {
        log_dispatcher("Dispatcher exiting");
        fclose(dispatcher_log);
        dispatcher_log = nullptr;
    }
}