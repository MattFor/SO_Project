//
// Created by MattFor on 21/12/2025.
//

#include <random>
#include <fstream>
#include <cstring>

#include "../include/Utilities.h"

static FILE* triage_log = nullptr;

static void log_tri(const std::string& s)
{
	if (triage_log)
	{
		std::string t = timestamp() + " " + s + "\n";
		if (fwrite(t.c_str(), 1, t.size(), triage_log) < 0)
			perror("fwrite triage");
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

	mqd_t mq_triage = mq_open(MQ_TRIAGE_NAME, O_RDONLY);
	if (mq_triage == (mqd_t) - 1)
	{
		perror("mq_open triage read");
		return 1;
	}
	mqd_t mq_doctor = mq_open(MQ_DOCTOR_NAME, O_WRONLY);
	if (mq_doctor == (mqd_t) - 1)
	{
		perror("mq_open doctor write");
		return 1;
	}

	std::default_random_engine     rng(static_cast<unsigned>(time(nullptr)));
	std::uniform_real_distribution uni(0.0, 1.0);

	char         buf[MAX_MSG_SIZE];
	unsigned int prio;
	while (true)
	{
		ssize_t r = mq_receive(mq_triage, buf, MAX_MSG_SIZE, &prio);
		if (r == -1)
		{
			if (errno == EINTR)
			{
				continue;
			}

			perror("mq_receive triage");
			break;
		}

		if (r < static_cast<ssize_t>(sizeof(PatientInfo)))
		{
			log_tri("Short msg ignored");
			continue;
		}
		PatientInfo p;
		memcpy(&p, buf, sizeof(PatientInfo));

		// 5% immediate dismiss
		if (const double x = uni(rng); x < 0.05)
		{
			log_tri("Patient id=" + std::to_string(p.id) + " dismissed immediately after triage");
			if (p.pid > 0)
			{
				kill(p.pid, SIGRTMIN + 3);
			}
			// Not forwarded
			continue;
		}

		// Otherwise determine color
		const double c = uni(rng);
		std::string  color;
		if (c < 0.10)
		{
			color = "RED";
		} // 10%
		else if (c < 0.10 + 0.35)
		{
			color = "YELLOW";
		} // 35%
		else
		{
			color = "GREEN";
		} // 55% (approx)

		log_tri("Patient id=" + std::to_string(p.id) + " triaged as " + color + (p.is_vip ? " VIP" : ""));

		// Send to doctor queue. Use priority: VIP -> 10, red->8, yellow->5, green->1
		unsigned int send_prio = p.is_vip ? 12 : (color == "RED" ? 8 : (color == "YELLOW" ? 5 : 1));
		if (mq_send(mq_doctor, buf, sizeof(PatientInfo), send_prio) == -1)
		{
			perror("mq_send doctor");
		}
		else
		{
			log_tri("Forwarded patient id=" + std::to_string(p.id) + " to doctors with prio " + std::to_string(send_prio));
		}
	}

	mq_close(mq_triage);
	mq_close(mq_doctor);
	fclose(triage_log);
}