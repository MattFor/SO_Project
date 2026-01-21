//
// Created by MattFor on 21/12/2025.
//

// Utilities.h

#ifndef ER_IPC_COMMON_H
#define ER_IPC_COMMON_H

#include <ctime>
#include <string>
#include <cstdio>
#include <atomic>
#include <mqueue.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <condition_variable>

#define LOGGING 0

static constexpr auto MQ_REG_NAME    = "/er_registration_mq";
static constexpr auto MQ_TRIAGE_NAME = "/er_triage_mq";
static constexpr auto MQ_DOCTOR_NAME = "/er_doctor_mq";

static constexpr auto SHM_NAME     = "/er_shm_ctrl";
static constexpr auto SEM_SHM_NAME = "/er_shm_sem";

static constexpr mode_t IPC_MODE = 0600; // Minimal perms

static constexpr auto MQ_PATIENT_CTRL = "/er_patient_ctrl";

// Control commands for per-patient control MQ
enum CtrlCmd : int
{
    CTRL_CHILD_TREATED,
    CTRL_SPAWN_CHILD,
    CTRL_GOTO_DOCTOR,
    CTRL_SHUTDOWN,
    CTRL_DISMISS,
    CTRL_INSIDE,
};

struct ControlMessage
{
    int  cmd; // CtrlCmd
    int  child_id;
    int  child_age;
    bool child_vip;
    char symptoms[128];

    int priority;
    int target_id;
    int target_pid;
};

// Shared control structure in shared memory
struct ERShared
{
    int N_waiting_room;      // Total capacity N
    int current_inside;      // How many inside waiting room
    int waiting_to_register; // Waiting to register

    // Number of patients that doctors have finished processing
    // Incremented by doctor processes under semaphore protection.
    int total_treated;

    bool second_reg_open; // Whether second registration window is open
    bool evacuation;      // Set by SIGUSR2

    int doctors_online;

    // Padding
    char pad[24];
};

static constexpr size_t MAX_QUEUE_MESSAGES = 32;
static constexpr size_t MAX_MSG_SIZE       = 2048;

struct PatientInfo
{
    int  id;
    int  pid;
    int  age;
    bool is_vip;
    char symptoms[128];
};

inline std::string timestamp()
{
    timespec ts{};
    if (clock_gettime(CLOCK_REALTIME, &ts) == -1)
    {
        perror("clock_gettime");
        return "0000-00-00 00:00:00.000";
    }

    tm tm{};
    localtime_r(&ts.tv_sec, &tm);

    char buf[64];
    strftime(buf, sizeof( buf ), "%Y-%m-%d %H:%M:%S", &tm);

    char final_buf[80];
    std::snprintf(final_buf, sizeof( final_buf ), "%s.%03ld", buf, ts.tv_nsec / 1000000L);

    return std::string(final_buf);
}

static constexpr size_t CTRL_SEM_BUCKETS   = 1024;
static constexpr size_t CTRL_REGISTRY_SIZE = 262144;

struct ControlSlot
{
    std::atomic<uint32_t> seq;
    std::atomic<pid_t>    pid;
    std::atomic<uint8_t>  rdy;
    ControlMessage        msg;
    char                  pad[32];
};

struct ControlRegistry
{
    uint32_t              initialized;
    ControlSlot           slots[CTRL_REGISTRY_SIZE];
    std::atomic<uint32_t> alloc_cursor;
};

static std::string ctrl_sem_name(const size_t bucket)
{
    char buf[64];
    std::snprintf(buf, sizeof( buf ), "/er_ctrl_sem_%zu", bucket);
    return std::string(buf);
}

static size_t slot_to_bucket(const size_t slot_idx)
{
    return slot_idx & CTRL_SEM_BUCKETS - 1;
}


#endif //!ER_IPC_COMMON_H