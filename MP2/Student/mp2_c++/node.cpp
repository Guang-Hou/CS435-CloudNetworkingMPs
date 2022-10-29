#include "json.hpp"
#include "node.hpp"
#include <iostream>
#include <sstream>
#include <stdio.h>
#include <netdb.h>
#include <sys/time.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <vector>
#include <climits>
#include <string>

using json = nlohmann::json;

int BUFFERSIZE = 5000;

Node::Node(int inputId, string costFile, string logFile)
{
    myNodeId = inputId;

    // initailize linkCost and paths
    for (int i = 0; i < 256; i += 1)
    {
        previousHeartbeat[i].tv_sec = 0;
        previousHeartbeat[i].tv_usec = 0;

        if (i == myNodeId)
        {

            linkCost[i] = 0;
        }
        else
        {
            linkCost[i] = 1;
        }

        for (int destId = 0; destId < 256; destId += 1)
        {
            get<0>(myPathRecords[i][destId]) = -1; // at beginning in my record, all my neighbors have no path, and I do not have path to them
        }
    }

    myPathRecords[myNodeId][myNodeId] = {0, myNodeId, set<int>{myNodeId}}; // for example of node 3: (0, (3,  {3})), the path includes the node itself

    setupNodeSockets();
    readCostFile(costFile.c_str());

    flog = fopen(logFile.c_str(), "a");
    logTime();
};

void Node::readCostFile(const char *costFile)
{
    FILE *fcost = fopen(costFile, "r");

    if (fcost == NULL)
    {
        return;
    }

    int destNodeId, cost;

    while (fscanf(fcost, "%d %d", &(destNodeId), &(cost)) != EOF)
    {
        linkCost[destNodeId] = cost;
    }
    fclose(fcost);
}

void Node::sendHeartbeats()
{
    // cout << "Beginning of sending heartbeats." << endl;

    struct timespec sleepFor;
    sleepFor.tv_sec = 0;
    sleepFor.tv_nsec = 300 * 1000 * 1000; // 300 ms

    const char *heartBeats = "HEREIAM";

    while (1)
    {
        for (int i = 0; i < 256; i += 1)
        {
            if (i != myNodeId)
            {
                // string logContent = "Inside broadcastMessage, will broadcast to node ";
                // logContent += to_string(i);
                // logMessage(logContent.c_str());

                sendto(mySocketUDP, heartBeats, 8, 0,
                       (struct sockaddr *)&allNodeSocketAddrs[i], sizeof(allNodeSocketAddrs[i]));
            }
        }
        nanosleep(&sleepFor, 0);
    }

    // cout << "Just sent heartbeats." << endl;
}

/**
 * @brief Convert my path to destId into string, append LSAs at the beginning.
 * Make sure there is a path to destId befor calling this function.
 *
 * @param destId
 * @return string
 */
string Node::generateStrPath(int destId) //
{
    // cout << "insider generateStrPaths function." << endl;

    char payload[BUFFERSIZE];
    memset(payload, 0, BUFFERSIZE);

    strcpy(payload, "LSAs");

    json LSA = {
        {"fromID", myNodeId},
        {"destID", destId},
        {"dist", get<0>(myPathRecords[myNodeId][destId])},
        {"nextHop", get<1>(myPathRecords[myNodeId][destId])},
        {"nodesInPath", get<2>(myPathRecords[myNodeId][destId])}};

    string strLSA = LSA.dump();

    memcpy(payload + 4, strLSA.c_str(), strLSA.length());

    string payloadStr(payload, payload + 4 + strLSA.length() + 1);

    // cout << "end of generateStrPaths function." << endl;

    return payloadStr;
}

void Node::sendPathToNeighbors(int destId)
{
    // cout << "insider broadcastMyPaths function." << endl;
    for (int i = 0; i < 256; i += 1)
    {
        if (i != destId && i != myNodeId && get<0>(myPathRecords[i][i]) != -1 && get<2>(myPathRecords[myNodeId][destId]).count(i) == 0)
        {
            string strLSA = generateStrPath(destId);
            sendto(mySocketUDP, strLSA.c_str(), strLSA.length(), 0,
                   (struct sockaddr *)&allNodeSocketAddrs[i], sizeof(allNodeSocketAddrs[i]));

            string logContent = "Just sent my path for destID ";
            logContent += to_string(destId);
            logContent += " to neighbor ";
            logContent += to_string(i);

            logContent += " : ";
            logContent += string(strLSA);
            logMessage(logContent.c_str());
            // cout << "end of broadcastMyPaths function." << endl;
        }
    }
}

void Node::sharePathsToNewNeighbor(int newNeighborId)
{
    for (int destId = 0; destId < 256; destId += 1)
    {
        // int distance = get<0>(myPathRecords[myNodeId][destId]);
        if (destId != myNodeId && destId != newNeighborId && get<0>(myPathRecords[myNodeId][destId]) != -1 && get<2>(myPathRecords[myNodeId][destId]).count(newNeighborId) == 0)
        {
            string logContent = "Share myPaths for destId ";
            logContent += to_string(destId);
            logContent += " to neighbor node ";
            logContent += to_string(newNeighborId);
            logMessage(logContent.c_str());

            string strLSA = generateStrPath(destId);
            sendto(mySocketUDP, strLSA.c_str(), BUFFERSIZE, 0,
                   (struct sockaddr *)&allNodeSocketAddrs[newNeighborId], sizeof(allNodeSocketAddrs[newNeighborId]));

            // logContent = "Done sharing myPaths of destId: ";
            // logContent += to_string(destId);
            // logContent += " to neighbor node ";
            // logContent += to_string(newNeighborId);
            // logMessage(logContent.c_str());
        }
    }
}

void Node::listenForNeighbors()
{

    // cout << "Beginning of listenForNeighbors." << endl;

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

            long usecondDifference = (now.tv_sec - previousHeartbeat[heardFrom].tv_sec) * 1000000L + now.tv_usec - previousHeartbeat[heardFrom].tv_usec;

            if (previousHeartbeat[heardFrom].tv_sec == 0 || usecondDifference > 600000) // timeout 0.6 second
            {
                string logContent = "Saw new neighbor ";
                logContent += to_string(heardFrom);
                logMessage(logContent.c_str());
                logTime();

                // string timeStr = "Time difference in usecond: ";
                // timeStr += to_string(usecondDifference);
                // logMessage(timeStr.c_str());

                handleNewNeighbor(heardFrom, heardFrom, 0, heardFrom, set<int>{heardFrom});
            }

            //  record that we heard from heardFrom just now.
            gettimeofday(&previousHeartbeat[heardFrom], 0);
        }

        // check if there is any neighbor link is broken, if so update  pathRecords and broadcast LSA
        for (int i = 0; i < 256; i += 1)
        {
            if (i != myNodeId)
            {
                long timeDifference = (now.tv_sec - previousHeartbeat[i].tv_sec) * 1000000L + now.tv_usec - previousHeartbeat[i].tv_usec;
                if (previousHeartbeat[i].tv_sec != 0 && timeDifference > 600000) // missed two hearbeats
                {
                    string logContent = "Link broken to node: ";
                    logContent += to_string(i);
                    logMessage(logContent.c_str());

                    handleBrokenLink(i);
                }
            }
        }

        if (!strncmp(recvBuf, "send", 4) || !strncmp(recvBuf, "fowd", 4)) // send message
        {
            short int destNodeId;
            memcpy(&destNodeId, recvBuf + 4, 2);
            destNodeId = ntohs(destNodeId);
            recvBuf[bytesRecvd] = 0;
            string content;
            content.assign(recvBuf, bytesRecvd + 1);

            string logContent = "Just received send or forward message.";
            logMessage(logContent.c_str());
            logTime();

            directMessage(destNodeId, content, bytesRecvd + 1);
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

            processNeighborLSA(neighborId, destId, distance, nextHop, nodesInPath);
        }
    }

    //(should never reach here)
    close(mySocketUDP);
}

void Node::handleNewNeighbor(int neighborId, int destId, int distance, int nextHop, set<int> nodesInPath)
{
    processNeighborLSA(neighborId, neighborId, 0, neighborId, set<int>{neighborId});
    sharePathsToNewNeighbor(neighborId);
}

void Node::processNeighborLSA(int neighborId, int destId, int distance, int nextHop, set<int> nodesInPath)
{
    json LSA = {
        {"fromID", neighborId},
        {"destID", destId},
        {"dist", distance},
        {"nextHop", nextHop},
        {"nodesInPath", nodesInPath}};

    string strLSA = LSA.dump();

    // cout << "Received LSA: " << endl;
    // cout << strLSA << endl;
    string logContent = "Just received LSA: ";
    logContent += strLSA;
    logMessage(logContent.c_str());
    logTime();
    // logPath();

    // update this neighbor and LSA in myPathRecords
    myPathRecords[neighborId][destId] = {distance, nextHop, nodesInPath};

    // if neighbor LSA affects my path to destId
    bool containsMyNodeId = nodesInPath.count(myNodeId) != 0;
    int myDistance = get<0>(myPathRecords[myNodeId][destId]);
    int myNexthop = get<1>(myPathRecords[myNodeId][destId]);

    if ((destId == myNodeId) || containsMyNodeId)
    {
        return;
    }

    if (distance != -1 && myDistance == -1)
    { // I do not have path to dest, but my neighbor has
      // use this neighbor path to destId
        // usePath(neighborId, destId, distance, nodesInPath);
        myPathRecords[myNodeId][destId] = {linkCost[neighborId] + distance, neighborId, nodesInPath};
        get<2>(myPathRecords[myNodeId][destId]).insert(myNodeId);
        sendPathToNeighbors(destId);
    }
    else if (distance != -1 && myDistance != -1)
    { // I have path to dest, and my neighbor has
      // compare two paths, if neighbor path is better then update it in myPathToNode[destId]
        int newDistance = distance + linkCost[neighborId];
        if ((myDistance > newDistance) || ((myDistance == newDistance) && (neighborId < myNexthop)))
        {
            myPathRecords[myNodeId][destId] = {linkCost[neighborId] + distance, neighborId, nodesInPath};
            get<2>(myPathRecords[myNodeId][destId]).insert(myNodeId);
            sendPathToNeighbors(destId);
        }
    }
    else if (distance == -1 && myDistance != -1)
    { // neighbor lost a path to destId
        if (get<1>(myPathRecords[myNodeId][destId]) == neighborId)
        {
            selectBestPath(destId);
        }
    }

    // logContent = "Done processing LSA.";
    // logMessage(logContent.c_str());
    //  logPath();
}

void Node::selectBestPath(int destId)
{
    int smallestDistance = INT_MAX;
    int bestMiddleNodeId = -1;
    int bestNextHop = -1;

    for (int middleNode = 0; middleNode < 256; middleNode += 1)
    {
        if (middleNode != myNodeId)
        {
            int middleDistanceToDest = get<0>(myPathRecords[middleNode][destId]);
            int myDistanceToNeighbor = get<0>(myPathRecords[myNodeId][destId]);
            int nextHop = get<1>(myPathRecords[myNodeId][middleNode]);
            int totalDistance = myDistanceToNeighbor + middleDistanceToDest;

            if (middleDistanceToDest != -1 && myDistanceToNeighbor != -1)
            {
                if ((totalDistance < smallestDistance) || (totalDistance == smallestDistance && nextHop < bestNextHop))
                {
                    smallestDistance = totalDistance;
                    bestMiddleNodeId = middleNode;
                    bestNextHop = nextHop;
                }
            }
        }
    }

    if (bestMiddleNodeId != -1)
    {
        // usePath(bestNeighborId, destId, get<0>(myPathRecords[bestNeighborId][destId]), get<2>(myPathRecords[bestNeighborId][destId]));
        get<0>(myPathRecords[myNodeId][destId]) = get<0>(myPathRecords[myNodeId][bestMiddleNodeId]) + get<0>(myPathRecords[bestMiddleNodeId][destId]);
        get<1>(myPathRecords[myNodeId][destId]) = get<1>(myPathRecords[myNodeId][bestMiddleNodeId]);
        get<2>(myPathRecords[myNodeId][destId]) = get<2>(myPathRecords[myNodeId][bestMiddleNodeId]);
        get<2>(myPathRecords[myNodeId][destId]).insert(get<2>(myPathRecords[bestMiddleNodeId][destId]).begin(), get<2>(myPathRecords[bestMiddleNodeId][destId]).end());
    }
    else
    {
        get<0>(myPathRecords[myNodeId][destId]) = -1;
    }

    sendPathToNeighbors(destId);
}

void Node::handleBrokenLink(int brokenNeighborId)
{
    get<0>(myPathRecords[brokenNeighborId][brokenNeighborId]) = -1;

    for (int destId = 0; destId < 256; destId += 1)
    {
        if (get<0>(myPathRecords[myNodeId][destId]) != -1 && get<1>(myPathRecords[myNodeId][destId]) == brokenNeighborId)
        {
            selectBestPath(destId);
        }
    }
}

void Node::logMessage(const char *message)
{
    char logLine[BUFFERSIZE];
    sprintf(logLine, "Log Message: %s\n", message);
    fwrite(logLine, 1, strlen(logLine), flog);
    fflush(flog);
}

void Node::logTime()
{
    struct timeval now;
    gettimeofday(&now, 0);

    char logLine[100];
    sprintf(logLine, "Log Message: %ld\n", now.tv_sec);
    fwrite(logLine, 1, strlen(logLine), flog);
    fflush(flog);
}

void Node::logPath()
{
    char logLine[BUFFERSIZE];
    json j_map(myPathRecords[myNodeId]);
    string path = j_map.dump();
    sprintf(logLine, "%s\n", path.c_str());
    fwrite(logLine, 1, strlen(logLine), flog);
    fflush(flog);
}

void Node::directMessage(int destNodeId, string message, int messageByte)
{
    char logLine[BUFFERSIZE];

    if (myNodeId == destNodeId)
    {
        sprintf(logLine, "receive packet message %s\n", message.c_str() + 6);
    }
    else
    {
        // std::cout << "Entered directMessage function." << std::endl;
        int distance = get<0>(myPathRecords[myNodeId][destNodeId]);
        if (distance != -1)
        {
            int nexthop = get<1>(myPathRecords[myNodeId][destNodeId]);

            if (!strncmp(message.c_str(), "send", 4))
            {
                char fowdMessage[messageByte];
                strcpy(fowdMessage, "fowd");
                memcpy(fowdMessage + 4, message.c_str() + 4, messageByte - 4);

                // string rmessage(message + 6);
                //  std::cout << "Message received: " << (message + 6) << std::endl;

                sendto(mySocketUDP, fowdMessage, messageByte, 0,
                       (struct sockaddr *)&allNodeSocketAddrs[nexthop], sizeof(allNodeSocketAddrs[nexthop]));

                sprintf(logLine, "sending packet dest %d nexthop %d message %s\n", destNodeId, nexthop, message.c_str() + 6);
            }
            else if (!strncmp(message.c_str(), "fowd", 4))
            {
                sendto(mySocketUDP, message.c_str(), messageByte, 0,
                       (struct sockaddr *)&allNodeSocketAddrs[nexthop], sizeof(allNodeSocketAddrs[nexthop]));

                sprintf(logLine, "forward packet dest %d nexthop %d message %s\n", destNodeId, nexthop, message.c_str() + 6);
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

    struct sockaddr_in bindAddr = allNodeSocketAddrs[myNodeId];
    if (bind(mySocketUDP, (struct sockaddr *)&bindAddr, sizeof(struct sockaddr_in)) < 0)
    {
        perror("bind error");
        close(mySocketUDP);
        exit(2);
    }
}
