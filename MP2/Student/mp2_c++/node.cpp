#include "json.hpp"
#include "node.hpp"
#include <iostream>
#include <string>
#include <sstream>
#include <fstream>
#include <stdio.h>
#include <netdb.h>
#include <sys/time.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <vector>
//#include <queue>
//#include <errno.h>
//#include <sys/types.h>
//#include <netinet/in.h>
//#include <sys/socket.h>

using json = nlohmann::json;

int BUFFERSIZE = 1500;

Node::Node(int inputId, string costFile, string logFile)
{
    myNodeId = inputId;
    // initailize linkCost and distanceAndPath
    for (int i = 0; i < 256; i += 1)
    {
        previousHeartbeat[i].tv_sec = 0;
        previousHeartbeat[i].tv_usec = 0;

        if (i == myNodeId)
        {
            vector<int> path{i};
            dp distancePathPair{0, path};
            // distancePathPair.first = 0;
            // distancePathPair.second = path;
            // pqOfPaths allPaths = pathVector[i];
            // allPaths.push(distancePathPair);
            pathVector[i].push(distancePathPair);
            linkCost[i] = 0;
        }
        else
        {
            linkCost[i] = 1;
        }
    }

    setupNodeSockets();

    fcost = fopen(costFile.c_str(), "r");
    flog = fopen(logFile.c_str(), "a");

    readCostFile();
};

void Node::readCostFile()
{
    if (fcost == NULL)
    {
        return;
    }

    int destNodeId, cost;

    while (fscanf(fcost, "%d %d", &(destNodeId), &(cost)) != EOF)
    {
        linkCost[destNodeId] = cost;
    }
    fclose(fcost); // close the file object.

    /*
        json j_vector(linkCost);
        string s = j_vector.dump();
        cout << s << endl;
        */
}

void Node::sendHeartbeats()
{
    struct timespec sleepFor;
    sleepFor.tv_sec = 0;
    sleepFor.tv_nsec = 300 * 1000 * 1000; // 300 ms

    const char *heartBeats = "HEREIAM";

    while (1)
    {
        broadcastMessage(heartBeats, myNodeId);  // myNodeId just for filling the parameter
        nanosleep(&sleepFor, 0);
    }
}

void Node::broadcastMessage(const char *message, int skipId)
{
    for (int i = 0; i < 256; i += 1)
    {
        if (i != myNodeId && i != skipId)
        {
            sendto(mySocketUDP, message, BUFFERSIZE, 0,
                   (struct sockaddr *)&allNodeSocketAddrs[i], sizeof(allNodeSocketAddrs[i]));
        }
    }
    // cout << "End of broadcastMessage." << endl;
}

string Node::generateStrLSA(int fromNodeId, int destNodeId, int distance, const vector<int> *path)
{
    string sPath = "";
    if (path != NULL)
    {
        json j_vector(*path);
        sPath = j_vector.dump();
        // cout << "The received path: " << sPath << endl;
    }

    // cout << "path size: " << sPath.length() << endl;

    char payload[BUFFERSIZE];
    memset(payload, 0, BUFFERSIZE);

    strcpy(payload, "LSAs");

    uint32_t from = htons(static_cast<uint32_t>(fromNodeId));
    uint32_t dest = htons(static_cast<uint32_t>(destNodeId));
    uint32_t dist = htons(static_cast<uint32_t>(distance));
    // cout << "from value: " << from << " length of from: " << sizeof(from) << endl;
    memcpy(payload + 4, &from, 4);
    memcpy(payload + 8, &dest, 4);
    memcpy(payload + 12, &dist, 4);
    memcpy(payload + 16, sPath.c_str(), sPath.length());
    // int byteSize = 16 + sPath.length() + 1;
    //  payload[16 + sPath.length()] = 0;

    // string content(payload + 16);
    // cout << "payload length: " << content.length() << endl;

    // cout << "The path in payload: " << content << endl;

    // json j_vector = json::parse(content);
    // auto path = j_vector.get<vector<int>>();
    //  string outStr;
    //  outStr.assign(payload + 16, payload + 32);
    //  cout << "Node " << myNodeId << " is broadcasting LSA Message: " << outStr << endl;

    // cout << "End of broadcastLSA." << endl;
    string payloadStr(payload, payload + 16 + sPath.length() + 1);

    // cout << "payloadStr: " << payloadStr << endl;

    return payloadStr;
}

void Node::broadcastLSA(int fromNodeId, int destNodeId, int distance, const vector<int> *path, int skipId)
{
    string strLSA = generateStrLSA(fromNodeId, destNodeId, distance, path);
    broadcastMessage(strLSA.c_str(), skipId);
}

void Node::listenForNeighbors()
{
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

        // string s(recvBuf);
        //  std::cout << "Received number of bytes: " << bytesRecvd << std::endl;

        struct timeval now;
        gettimeofday(&now, 0);
        // printf("Now in seconds : %ld\tmicro seconds : %ld\n", now.tv_sec, now.tv_usec);

        // heartbeat message to identify new neighbors
        short int heardFrom = -1;
        if (strstr(fromAddr, "10.1.1."))
        {
            heardFrom = atoi(
                strchr(strchr(strchr(fromAddr, '.') + 1, '.') + 1, '.') + 1);

            // std::cout << "Received from Node: " << heardFrom << std::endl;
            //  std::cout << "Content received: " << s << std::endl;

            // TODO: this node can consider heardFrom to be directly connected to it; do any such logic now.
            // printf("HeardFrom Node previous heartbeat in seconds : %ld\tmicro seconds : %ld\n", previousHeartbeat[heardFrom].tv_sec, previousHeartbeat[heardFrom].tv_usec);
            // long timeGap = (now.tv_sec - previousHeartbeat[heardFrom].tv_sec) * 1000000L + now.tv_usec - previousHeartbeat[heardFrom].tv_usec;
            // printf("Time gap from last time seen this node: %ld\n", timeGap);

            if (previousHeartbeat[heardFrom].tv_sec == 0 ||
                ((now.tv_sec - previousHeartbeat[heardFrom].tv_sec) * 1000000L + now.tv_usec - previousHeartbeat[heardFrom].tv_usec) > 600000) // timeout 0.6 second
            {
                vector<int> fromToDestPath{heardFrom};
                // dp distPath = pair<0, fromToDestPath>;
                updateDistanceAndPath(heardFrom, heardFrom, 0, &fromToDestPath);
                // myNeighbors.add(heardFrom)
                // broadcastLSA();
                // cout << "After updating, is the path empty? " << pathVector[heardFrom].empty() << endl;
                // json j_vector(pathVector[heardFrom].top().second);
                // string s = j_vector.dump();
                // cout << "Latest Path to the node " << heardFrom << " : " << s << endl;

                sharePathsToNewNeighbor(heardFrom);
            }

            // neighbors.insert(heardFrom);
            //  record that we heard from heardFrom just now.
            gettimeofday(&previousHeartbeat[heardFrom], 0);
        }

        // check if there is any neighbor link is broken, if so update distanceAndPath and broadcast LSA
        for (int i = 0; i < 256; i += 1)
        {
            if (i != myNodeId)
            {
                if (previousHeartbeat[i].tv_sec != 0 &&
                    ((now.tv_sec - previousHeartbeat[i].tv_sec) * 1000000L + now.tv_usec - previousHeartbeat[i].tv_usec) > 600000) // missed two hearbeats
                {
                    updateDistanceAndPath(myNodeId, i, -1, NULL);
                    // std::cout << "Link broken to node: " << i << std::endl;
                }
            }
        }

        if (!strncmp(recvBuf, "send", 4) || !strncmp(recvBuf, "fowd", 4)) // send message
        {
            short int destNodeId;
            memcpy(&destNodeId, recvBuf + 4, 2);
            destNodeId = ntohs(destNodeId);
            recvBuf[bytesRecvd] = 0;

            /*
            std::cout << "Dest Node ID: " << destNodeId << std::endl;

            char message[100];
            strcpy(message, "fowd");
            memcpy(message + 4, recvBuf + 4, bytesRecvd - 3);

            string rmessage(message + 6);
            std::cout << "Message received: " << (message + 6) << std::endl;
            */

            directMessage(destNodeId, recvBuf, bytesRecvd + 1);
        }
        /*
        else if (!strncmp(recvBuf, "fowd", 4)) // forward message
        {
            short int destNodeId;

            memcpy(&destNodeId, recvBuf + 4, 2);
            destNodeId = ntohs(destNodeId);

            directMessage(destNodeId, recvBuf, bytesRecvd);
        }
        */

        else if (!strncmp(recvBuf, "LSAs", 4)) // LSA message
        {

            // broadcastLSA(int fromNodeId, int destNodeId, dp distancePathPair)

            uint32_t from, dest, dist;
            memcpy(&from, recvBuf + 4, sizeof(from));
            memcpy(&dest, recvBuf + 8, sizeof(dest));
            memcpy(&dist, recvBuf + 12, sizeof(dest));
            string content(recvBuf + 16);

            from = ntohs(from);
            dest = ntohs(dest);
            dist = ntohs(dist);

            // std::cout << "fromId: " << from << " destId: " << dest << " dist: " << dist << std::endl;
            //  std::cout << "message_type: " << message_type << std::endl;
            //  std::cout << "contentLength: " << contentLength << std::endl;
            // std::cout << "content of received LSA message: " << content << std::endl;

            json j_vector = json::parse(content);
            auto path = j_vector.get<vector<int>>();

            updateDistanceAndPath(from, dest, dist, &path);
        }
    }

    //(should never reach here)
    close(mySocketUDP);
}

void Node::sharePathsToNewNeighbor(int newNeighborId)
{
    for (int i = 0; i < 256; i += 1)
    {
        if (i != myNodeId && i != newNeighborId && !pathVector[i].empty())
        {
            int distance = pathVector[i].top().first;
            const vector<int> *path = &(pathVector[i].top().second);
            string strLSA = generateStrLSA(myNodeId, i, distance, path);
            sendto(mySocketUDP, strLSA.c_str(), BUFFERSIZE, 0,
                   (struct sockaddr *)&allNodeSocketAddrs[newNeighborId], sizeof(allNodeSocketAddrs[newNeighborId]));
        }
    }
}

void Node::updateDistanceAndPath(int fromNodeId, int destNodeId, int fromToDestDistance, const vector<int> *fromToDestPath)
{
    // json j_vector(fromToDestPath);
    // string s1 = j_vector.dump();
    // cout << "Received fromNode: " << fromNodeId << " "  << "destNode: " << destNodeId << "Distance: " << fromToDestDistance << endl;
    // cout << "Received fromToDestPath: " << s1 << endl;

    if (destNodeId == myNodeId)
    {
        return;
    }

    pqOfPaths *currPaths = &(pathVector[destNodeId]);
    int thisToFromDistance = linkCost[fromNodeId];

    int newDist = linkCost[fromNodeId] + fromToDestDistance;

    // bool isEmpty = currPaths->empty();
    // cout << "Is the currPaths empty: " << isEmpty << endl;

    if (currPaths->empty())
    {
        if (fromToDestDistance != -1)
        {
            vector<int> newPath = *fromToDestPath;
            if (fromNodeId != destNodeId)
            {
                newPath.insert(newPath.begin(), fromNodeId);
            }
            currPaths->push(make_pair(newDist, newPath));
            // cout << "Adding new path." << endl;
            // cout << "is the path empty? " << currPaths->empty() << endl;
            // if (!currPaths->empty())
            //{
            // json j_vector(currPaths->top().second);
            // string s1 = j_vector.dump();
            // cout << "Received fromNode: " << fromNodeId << " "
            //     << "destNode: " << destNodeId << "Distance: " << fromToDestDistance << endl;
            // cout << "Inside update function, updated path: " << s1 << endl;
            //}

            broadcastLSA(myNodeId, destNodeId, newDist, &(currPaths->top().second), fromNodeId);
        }
    }
    else
    {
        int currThisToDestDistance = currPaths->top().first;
        int currNextHop = currPaths->top().second[0];

        // link broken
        if (fromToDestDistance == -1)
        {
            bool firstPathAffected = currNextHop == fromNodeId;
            pqOfPaths newPaths;

            // remvoe all links starting with fromNodeId
            while (!currPaths->empty())
            {
                dp distancePath = currPaths->top();
                currPaths->pop();
                if (distancePath.second[0] != fromNodeId) // if the path is not goring through the broken link
                {
                    newPaths.push(distancePath);
                }
            }

            if (newPaths.empty())
            {
                broadcastLSA(myNodeId, destNodeId, -1, NULL, fromNodeId);
            }
            else
            {
                pathVector[destNodeId] = newPaths;
                if (firstPathAffected) // the best path from myNode to destNode was changed
                {
                    broadcastLSA(myNodeId, destNodeId, newPaths.top().first, &(newPaths.top().second), fromNodeId);
                }
            }
        }
        else // link update message
        {
            if ((*fromToDestPath)[0] != myNodeId && !std::count(fromToDestPath->begin(), fromToDestPath->end(), myNodeId)) // this node is not in the path, so to avoid loopy path
            {
                vector<int> newPath = *fromToDestPath;
                if (fromNodeId != destNodeId)
                {
                    newPath.insert(newPath.begin(), fromNodeId);
                }

                currPaths->push(make_pair(newDist, newPath));

                if ((currThisToDestDistance > newDist) || ((currThisToDestDistance == newDist) && (currNextHop > fromNodeId)))
                {
                    broadcastLSA(myNodeId, destNodeId, newDist, &(currPaths->top().second), fromNodeId);
                }
            }
        }
    }
}

void Node::directMessage(int destNodeId, char *message, int messageByte)
{
    char logLine[BUFFERSIZE];

    if (myNodeId == destNodeId)
    {
        sprintf(logLine, "receive packet message %s\n", message + 6);
    }
    else
    {
        // std::cout << "Entered directMessage function." << std::endl;
        if (!pathVector[destNodeId].empty())
        {
            int nexthop = pathVector[destNodeId].top().second[0];

            if (!strncmp(message, "send", 4))
            {
                char fowdMessage[messageByte];
                strcpy(fowdMessage, "fowd");
                memcpy(fowdMessage + 4, message + 4, messageByte - 4);

                // string rmessage(message + 6);
                //  std::cout << "Message received: " << (message + 6) << std::endl;

                sendto(mySocketUDP, fowdMessage, messageByte, 0,
                       (struct sockaddr *)&allNodeSocketAddrs[nexthop], sizeof(allNodeSocketAddrs[nexthop]));

                sprintf(logLine, "sending packet dest %d nexthop %d message %s\n", destNodeId, nexthop, message + 6);
            }
            else if (!strncmp(message, "fowd", 4))
            {
                sendto(mySocketUDP, message, messageByte, 0,
                       (struct sockaddr *)&allNodeSocketAddrs[nexthop], sizeof(allNodeSocketAddrs[nexthop]));

                sprintf(logLine, "forward packet dest %d nexthop %d message %s\n", destNodeId, nexthop, message + 6);
            }
        }
        else
        {
            sprintf(logLine, "unreachable dest %d\n", destNodeId);
            // std::cout << "logLine: " << logLine << std::endl;
        }
    }

    fwrite(logLine, 1, strlen(logLine), flog);
    // std::cout << "Bytes written to the log file: " << bytesWritten << std::endl;
    fflush(flog);
}

void Node::setupNodeSockets()
{
    for (int i = 0; i < 256; i++)
    {
        char tempaddr[100];
        sprintf(tempaddr, "10.1.1.%d", i);
        memset(&allNodeSocketAddrs[i], 0, sizeof(allNodeSocketAddrs[i]));
        allNodeSocketAddrs[i].sin_family = AF_INET;
        allNodeSocketAddrs[i].sin_port = htons(7777);
        inet_pton(AF_INET, tempaddr, &allNodeSocketAddrs[i].sin_addr);
    }

    // socket() and bind() our socket. We will do all sendto()ing and recvfrom()ing on this one.
    if ((mySocketUDP = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        perror("socket error");
        exit(1);
    }

    // char myAddr[100];
    // struct sockaddr_in bindAddr;
    // sprintf(myAddr, "10.1.1.%d", myNodeId);
    // memset(&bindAddr, 0, sizeof(bindAddr));
    // bindAddr.sin_family = AF_INET;
    // bindAddr.sin_port = htons(7777);
    // inet_pton(AF_INET, myAddr, &bindAddr.sin_addr);

    struct sockaddr_in bindAddr = allNodeSocketAddrs[myNodeId];
    if (bind(mySocketUDP, (struct sockaddr *)&bindAddr, sizeof(struct sockaddr_in)) < 0)
    {
        perror("bind error");
        close(mySocketUDP);
        exit(2);
    }
}
