// path vector, sending single LSA updates to neighbor for every update

#include "pv_single.hpp"
#include <iostream>
#include <thread>
#include <pthread.h>

void listenForNeighbors();
void *announceToNeighbors(void *unusedParam);

int main(int argc, char **argv)
{
    if (argc != 4)
    {
        fprintf(stderr, "Usage: %s mynodeid initialcostsfile logfile\n\n", argv[0]);
        exit(1);
    }

    int inputId = atoi(argv[1]);
    string costFile = argv[2];
    string logFile = argv[3];

    init(inputId, costFile, logFile);

    pthread_t announcerThread;
    pthread_create(&announcerThread, 0, announceToNeighbors, (void *)0);

    listenForNeighbors();
}