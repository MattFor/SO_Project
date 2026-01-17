//
// Created by MattFor on 21/12/2025.
//

#include <random>
#include <fstream>
#include <cstring>
#include <unistd.h>
#include <cerrno>
#include <csignal>

#include "../include/Utilities.h"

static FILE* triage_log = nullptr;

static void log_tri(const std::string& s)
{
    if constexpr (!LOGGING)
        return;

    if (triage_log)
    {
        std::string t = timestamp() + " " + s + "\n";
        fwrite(t.c_str(), 1, t.size(), triage_log);
        fflush(triage_log);
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

    const mqd_t mq_triage =
        mq_open(MQ_TRIAGE_NAME, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (mq_triage == (mqd_t)-1)
    {
        perror("mq_open triage");
        return 1;
    }

    std::default_random_engine rng(static_cast<unsigned>(time(nullptr)));
    std::uniform_real_distribution<double> uni(0.0, 1.0);

    char buf[MAX_MSG_SIZE];

    while (true)
    {
        unsigned int prio = 0;
        const ssize_t r =
            mq_receive(mq_triage, buf, MAX_MSG_SIZE, &prio);

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
                kill(p.pid, SIGRTMIN + 3);
                log_tri("Dismissed patient id=" +
                        std::to_string(p.id) +
                        " pid=" + std::to_string(p.pid));
            }
            continue;
        }

        unsigned int send_prio;
        const double c = uni(rng);
        if (c < 0.10)
            send_prio = 8;
        else if (c < 0.45)
            send_prio = 5;
        else
            send_prio = 1;

        ControlMessage cm{};
        cm.cmd        = CTRL_GOTO_DOCTOR;
        cm.target_pid = p.pid;
        cm.priority   = send_prio;

        // 🔑 OPEN SHARED CONTROL MQ PER SEND (DO NOT CACHE)
        const mqd_t mq_ctrl =
            mq_open(MQ_PATIENT_CTRL, O_WRONLY | O_CLOEXEC);

        if (mq_ctrl == (mqd_t)-1)
        {
            log_tri("FAILED mq_open MQ_PATIENT_CTRL errno=" +
                    std::to_string(errno));
            continue;
        }

        if (mq_send(
                mq_ctrl,
                reinterpret_cast<const char*>(&cm),
                sizeof(cm),
                0) == -1)
        {
            log_tri("FAILED CTRL_GOTO_DOCTOR id=" +
                    std::to_string(p.id) +
                    " pid=" + std::to_string(p.pid) +
                    " errno=" + std::to_string(errno));
        }
        else
        {
            log_tri("Triaged patient id=" +
                    std::to_string(p.id) +
                    " pid=" + std::to_string(p.pid) +
                    " prio=" + std::to_string(send_prio));
        }

        mq_close(mq_ctrl);
    }

    mq_close(mq_triage);
    fclose(triage_log);
    return 0;
}
