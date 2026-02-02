//
// Created by MattFor on 02.02.2026.
//

#include <stdio.h>
#include <mqueue.h>
#include <string.h>
#include <stdlib.h>

#include "../include/Utilities.h"

int main(const int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "Usage: send_ctrl <target_pid> <cmd> [child_id] [child_age] [vip]\n");
        return 1;
    }

    mqd_t mq = mq_open(MQ_PATIENT_CTRL, O_WRONLY | O_CLOEXEC);
    if (mq == ( mqd_t ) - 1)
    {
        perror("mq_open");
        return 1;
    }

    ControlMessage cm = {};
    cm.target_pid     = atoi(argv[1]);
    cm.cmd            = atoi(argv[2]);

    if (argc > 3)
    {
        cm.child_id = atoi(argv[3]);
    }

    if (argc > 4)
    {
        cm.child_age = atoi(argv[4]);
    }

    if (argc > 5)
    {
        cm.child_vip = atoi(argv[5]) != 0;
    }

    strncpy(cm.symptoms, "send_ctrl test", sizeof( cm.symptoms ) - 1);

    if (mq_send(mq, (char*)&cm, sizeof( cm ), 0) == -1)
    {
        perror("mq_send");
        mq_close(mq);
        return 1;
    }

    mq_close(mq);
}