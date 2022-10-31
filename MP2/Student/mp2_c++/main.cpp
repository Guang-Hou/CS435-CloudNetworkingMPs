#include "node.cpp"
#include <iostream>
#include <thread>

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

    std::thread threads[4];
    threads[0] = thread(sendHeartbeats);

    char fromAddr[100];
    struct sockaddr_in theirAddr;
    socklen_t theirAddrLen = sizeof(theirAddr);
    char recvBuf[BUFFERSIZE];
    int bytesRecvd;

    while (1)
    {
        memset(recvBuf, 0, sizeof(recvBuf));
        if ((bytesRecvd = recvfrom(mySocketUDP, recvBuf, BUFFERSIZE, 0,
                                   (struct sockaddr *)&theirAddr, &theirAddrLen)) == -1)
        {
            perror("connectivity listener: recvfrom failed");
            exit(1);
        }
        inet_ntop(AF_INET, &theirAddr.sin_addr, fromAddr, 100);

        string content(recvBuf, recvBuf + bytesRecvd + 1);

        short int heardFrom = -1;
        heardFrom = atoi(strchr(strchr(strchr(fromAddr, '.') + 1, '.') + 1, '.') + 1);

        if (strstr(fromAddr, "10.1.1."))
        {
            if (threads[1].joinable())
            {
                threads[1].join();
            }

            threads[1] = thread(checkNewAndLostNeighbor, heardFrom);       // check too often for lost neighbor?
        }

        if (!strncmp(recvBuf, "send", 4) || !strncmp(recvBuf, "fowd", 4))  // send/forward message
        {
            if (threads[2].joinable())
            {
                threads[2].join();
            }

            threads[2] = thread(sendOrFowdMessage, content, bytesRecvd);
        }
        else if (!strncmp(recvBuf, "LSAs", 4)) // LSA message
        {
            if (threads[3].joinable())
            {
                threads[3].join();
            }

            threads[3] = thread(processLSAMessage, content);
        }
        else {
            logMessage("Should Not Happen!!!!!1");
        }
    }

    if (threads[0].joinable())
    {
        threads[0].join();
    }

    //(should never reach here)
    close(mySocketUDP);
}
