// link stage, sending single path LSA updates to neighbor

#include "ls_newShare.hpp"
//#include <iostream>
//#include <thread>
//#include <pthread.h>

void listenForNeighbors();
void* announceToNeighbors(void* unusedParam);
void* announceLSA(void* unusedParam);

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

    pthread_t announcerHello;
    pthread_create(&announcerHello, 0, announceToNeighbors, (void *)0);

    pthread_t announcerLSA;
    pthread_create(&announcerLSA, 0, announceLSA, (void *)0);

    listenForNeighbors();

    // testDij(6, 3);
}