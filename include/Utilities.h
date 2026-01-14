//
// Created by MattFor on 21/12/2025.
//

#ifndef ER_IPC_COMMON_H
#define ER_IPC_COMMON_H

#include <ctime>
#include <string>
#include <cstdio>
#include <mqueue.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <semaphore.h>

#define LOGGING 0

static constexpr auto MQ_REG_NAME    = "/er_registration_mq";
static constexpr auto MQ_TRIAGE_NAME = "/er_triage_mq";
static constexpr auto MQ_DOCTOR_NAME = "/er_doctor_mq";

static constexpr auto SHM_NAME     = "/er_shm_ctrl";
static constexpr auto SEM_SHM_NAME = "/er_shm_sem";

static constexpr mode_t IPC_MODE = 0600; // Minimal perms

static constexpr auto PATIENT_CTRL_MQ_PREFIX = "/er_patient_ctrl_";

// Control commands for per-patient control MQ
enum CtrlCmd : int
{
    CTRL_SPAWN_CHILD,
    CTRL_SHUTDOWN,
    CTRL_DISMISS,
    CTRL_INSIDE
};

struct ControlMessage
{
    int  cmd; // CtrlCmd
    int  child_id;
    int  child_age;
    bool child_vip;
    char symptoms[128];
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
    // Padding
    char pad[24];
};

static constexpr size_t MAX_QUEUE_MESSAGES = 50;
static constexpr size_t MAX_MSG_SIZE       = 512;

// Patient struct info marshaled as string
struct PatientInfo
{
    int  id;
    int  pid; // Process id of patient process
    int  age;
    bool is_vip;
    // Symptoms string (short)
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

    // Append milliseconds
    char final_buf[80];
    std::snprintf(final_buf, sizeof( final_buf ), "%s.%03ld", buf, ts.tv_nsec / 1000000L);

    return std::string(final_buf);
}


#endif //!ER_IPC_COMMON_H