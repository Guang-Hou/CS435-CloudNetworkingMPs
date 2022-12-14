/* Path vector routing, sending single LSA updates to neighbors periodically. 
   Share my newer paths to new neighbor. */

#include "pv_periodicNewShare.hpp"
#include <stdlib.h>
#include <pthread.h>

int main(int argc, char** argv)
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

    pthread_t announcerHeartbeat;
    pthread_create(&announcerHeartbeat, 0, announceHeartbeat, (void*)0);

    //pthread_t announcerLSA;
    //pthread_create(&announcerLSA, 0, announceLSA, (void*)0);

    listenForNeighbors();
}
