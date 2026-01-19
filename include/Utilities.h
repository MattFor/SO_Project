//
// Created by MattFor on 21/12/2025.
//

// Utilities.h

#ifndef ER_IPC_COMMON_H
#define ER_IPC_COMMON_H

#include <ctime>
#include <string>
#include <cstdio>
#include <mqueue.h>
#include <sys/mman.h>
#include <atomic>
#include <sys/wait.h>
#include <semaphore.h>

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
    CTRL_SPAWN_CHILD,
    CTRL_SHUTDOWN,
    CTRL_DISMISS,
    CTRL_INSIDE,
    CTRL_GOTO_DOCTOR
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


// --- add after ERShared in Utilities.h ---

// Control registry: fixed size array of slots (one per potential patient).
// Registry size should be >= max concurrent live patients (choose 131072 or similar).
static constexpr size_t CTRL_REGISTRY_SIZE = 131072; // tune as needed
static constexpr size_t CTRL_SEM_BUCKETS    = 256;   // small, power-of-two preferred

// in Utilities.h

struct ControlSlot
{
    std::atomic<uint32_t> seq;      // sequence number: 0 == empty, nonzero -> message available
    std::atomic<pid_t>    pid;      // owner pid (0 if empty)  <-- make atomic
    ControlMessage        msg;      // message storage
    char pad[32];
};

// Control region placed in shm alongside ERShared; address returned by shm_open(SHM_NAME)
struct ControlRegistry
{
    // A very small header to indicate initialization
    uint32_t initialized;
    // fixed-size slots
    ControlSlot slots[CTRL_REGISTRY_SIZE];
    // sequence counter used for slot allocation (simple round-robin starting point)
    std::atomic<uint32_t> alloc_cursor;
};

// Names for semaphore buckets: we will create named semaphores
// Use pattern "/er_ctrl_sem_<i>" (i in [0, CTRL_SEM_BUCKETS))
static inline std::string ctrl_sem_name(size_t bucket)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "/er_ctrl_sem_%zu", bucket);
    return std::string(buf);
}

// Hash function to map slot index -> semaphore bucket
static inline size_t slot_to_bucket(size_t slot_idx)
{
    return slot_idx & (CTRL_SEM_BUCKETS - 1); // CTRL_SEM_BUCKETS must be power of two
}


#endif //!ER_IPC_COMMON_H