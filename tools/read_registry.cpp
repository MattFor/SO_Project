//
// Created by MattFor on 02.02.2026.
//

#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>

#include "../include/Utilities.h"

int main()
{
    int fd = shm_open(SHM_NAME, O_RDONLY, 0);
    if (fd == -1)
    {
        perror("shm_open");
        return 1;
    }

    size_t total = sizeof(ERShared) + sizeof(ControlRegistry);
    void*  p     = mmap(NULL, total, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED)
    {
        perror("mmap");
        return 1;
    }

    const ERShared*  shm = static_cast<ERShared*>(p);
    ControlRegistry* reg = (ControlRegistry*)( static_cast<char*>(p) + sizeof(ERShared) );
    printf("ERShared: N=%d current_inside=%d waiting=%d total_treated=%d doctors_online=%d\n", shm->N_waiting_room, shm->current_inside, shm->waiting_to_register, shm->total_treated, shm->doctors_online);

    for (size_t i = 0; i < CTRL_REGISTRY_SIZE; i++)
    {
        if (pid_t pid = reg->slots[i].pid; pid != 0)
        {
            printf("slot[%zu] pid=%d rdy=%u seq=%u\n", i, static_cast<int>(pid), static_cast<unsigned>(reg->slots[i].rdy.load()), static_cast<unsigned>(reg->slots[i].seq.load()));
        }
    }
}