//
// Created by MattFor on 21/12/2025.
//

#include <random>
#include <fstream>
#include <cstring>
#include <unistd.h>
#include <signal.h>

#include "../include/Utilities.h"

#define LOGGING 0

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
        return 1;
    }

    std::default_random_engine     rng(static_cast<unsigned>(time(nullptr)) + doc_id);
    std::uniform_real_distribution uni(0.0, 1.0);

    char         buf[MAX_MSG_SIZE];
    unsigned int prio;
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
        usleep(treat_ms * 1000);

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

    log_doc("Doctor " + std::to_string(doc_id) + " exiting due to evacuation.");
    mq_close(mq_doc);
    fclose(doc_log);
}