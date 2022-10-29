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

    // initialization: get this process's node ID, record what time it is,
    // and set up our sockaddr_in's for sending to the other nodes.
    int inputId = atoi(argv[1]);
    string costFile = argv[2];
    string logFile = argv[3];

    Node node(inputId, costFile, logFile);

    thread th1(&Node::sendHeartbeats, &node);

    // node.listenForNeighbors();
    std::thread th2(&Node::listenForNeighbors, &node);

    /*
    char fromAddr[100];
    struct sockaddr_in theirAddr;
    socklen_t theirAddrLen = sizeof(theirAddr);
    char recvBuf[BUFFERSIZE];
    int bytesRecvd;

    while (1)
    {
        memset(recvBuf, 0, sizeof(recvBuf));
        if ((bytesRecvd = recvfrom(node.mySocketUDP, recvBuf, BUFFERSIZE, 0,
                                   (struct sockaddr *)&theirAddr, &theirAddrLen)) == -1)
        {
            perror("connectivity listener: recvfrom failed");
            exit(1);
        }

        inet_ntop(AF_INET, &theirAddr.sin_addr, fromAddr, 100);

        // string s(recvBuf);
        // std::cout << "Received number of bytes: " << bytesRecvd << std::endl;

        struct timeval now;
        gettimeofday(&now, 0);
        // printf("Now in seconds : %ld\tmicro seconds : %ld\n", now.tv_sec, now.tv_usec);

        // heartbeat message to identify new neighbors
        short int heardFrom = -1;
        if (strstr(fromAddr, "10.1.1."))
        {
            heardFrom = atoi(
                strchr(strchr(strchr(fromAddr, '.') + 1, '.') + 1, '.') + 1);

            long usecondDifference = (now.tv_sec - node.previousHeartbeat[heardFrom].tv_sec) * 1000000L + now.tv_usec - node.previousHeartbeat[heardFrom].tv_usec;

            if (node.previousHeartbeat[heardFrom].tv_sec == 0 || usecondDifference > 600000) // timeout 0.6 second
            {
                string logContent = "Saw new neighbor ";
                logContent += to_string(heardFrom);
                node.logMessage(logContent.c_str());

                // string timeStr = "Time difference in usecond: ";
                // timeStr += to_string(usecondDifference);
                // logMessage(timeStr.c_str());

                // node.handleNewNeighbor(heardFrom, heardFrom, 0, heardFrom, set<int>{heardFrom});
                thread *hn = new thread(&Node::handleNewNeighbor, &node, heardFrom, heardFrom, 0, heardFrom, set<int>{heardFrom});
            }

            //  record that we heard from heardFrom just now.
            gettimeofday(&node.previousHeartbeat[heardFrom], 0);
        }

        // check if there is any neighbor link is broken, if so update  pathRecords and broadcast LSA
        for (int i = 0; i < 256; i += 1)
        {
            if (i != node.myNodeId)
            {
                long timeDifference = (now.tv_sec - node.previousHeartbeat[i].tv_sec) * 1000000L + now.tv_usec - node.previousHeartbeat[i].tv_usec;
                if (node.previousHeartbeat[i].tv_sec != 0 && timeDifference > 600000) // missed two hearbeats
                {
                    string logContent = "Link broken to node: ";
                    logContent += to_string(i);
                    node.logMessage(logContent.c_str());

                    thread *bl = new thread(&Node::handleBrokenLink, &node, i);
                }
            }
        }

        if (!strncmp(recvBuf, "send", 4) || !strncmp(recvBuf, "fowd", 4)) // send message
        {
            short int destNodeId;
            memcpy(&destNodeId, recvBuf + 4, 2);
            destNodeId = ntohs(destNodeId);
            recvBuf[bytesRecvd] = 0;

            string logContent = "Just received send or forward message.";
            node.logMessage(logContent.c_str());
            node.logTime();
            string content;
            content.assign(recvBuf, bytesRecvd + 1);

            thread *dm = new thread(&Node::directMessage, &node, destNodeId, content, bytesRecvd + 1);
        }
        else if (!strncmp(recvBuf, "LSAs", 4)) // LSA message
        {
            // cout << "Received LSAs message." << endl;
            string strLSA(recvBuf + 4);
            json LSA = json::parse(strLSA);

            int neighborId = LSA["fromID"];
            int destId = LSA["destID"];
            int distance = LSA["dist"];
            int nextHop = LSA["nextHop"];
            set<int> nodesInPath = LSA["nodesInPath"];

            // node.processNeighborLSA(neighborId, destId, distance, nextHop, nodesInPath);
            thread *pn = new thread(&Node::processNeighborLSA, &node, neighborId, destId, distance, nextHop, nodesInPath);
        }
    }
    */

    th1.join();
    th2.join();

    //(should never reach here)
    close(node.mySocketUDP);
}
