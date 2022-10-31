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
#include <mutex>

using json = nlohmann::json;

int BUFFERSIZE = 1000;
mutex m;

void init(int inputId, string costFile, string logFile)
{
    myNodeId = inputId;

    // initailize linkCost and myPaths
    for (int i = 0; i < 256; i += 1)
    {
        previousHeartbeat[i].tv_sec = 0;
        previousHeartbeat[i].tv_usec = 0;

        if (i == myNodeId)
        {
            linkCost[i] = 0;
            myPaths[i] = {0, myNodeId, set<int>{myNodeId}}; // for example of node 3: (0, (3,  {3})), the path includes the node itself
        }
        else
        {
            linkCost[i] = 1;
            get<0>(myPaths[i]) = -1; // at beginning I do not have any path to other nodes
        }
    }

    setupNodeSockets();
    readCostFile(costFile.c_str());

    flog = fopen(logFile.c_str(), "a");

    logMessage("Initialization.");
    logTime();
}

void readCostFile(const char *costFile)
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

void sendHeartbeats()
{
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
                // string logContent = "Inside sendHearbeat, will send haerbeat to node ";
                // logContent += to_string(i);
                // logMessage(logContent.c_str());

                sendto(mySocketUDP, heartBeats, 8, 0,
                       (struct sockaddr *)&allNodeSocketAddrs[i], sizeof(allNodeSocketAddrs[i]));
            }
        }
        nanosleep(&sleepFor, 0);
    }
}

string generateStrPath(int destId) //
{
    char payload[BUFFERSIZE];
    memset(payload, 0, BUFFERSIZE);

    strcpy(payload, "LSAs");

    distance = get<0>(myPaths[destId]);
    if (distance == -1) {
        json LSA = {
            {"fromID", myNodeId},
            {"destID", destId},
            {"dist", -1},
            {"nextHop", -1},
            {"nodesInPath", set<int> {}}};
    } else {
        json LSA = {
            {"fromID", myNodeId},
            {"destID", destId},
            {"dist", distance},
            {"nextHop", get<1>(myPaths[destId])},
            {"nodesInPath", get<2>(myPaths[destId])}};
    }

    string strLSA = LSA.dump();
    memcpy(payload + 4, strLSA.c_str(), strLSA.length());

    string payloadStr(payload, payload + 4 + strLSA.length() + 1);

    return payloadStr;
}

void sendPathToNeighbors(int destId)
{
    for (int i = 0; i < 256; i += 1)
    {
        if (i != myNodeId && i != destId) // no filtering to avoid nodes in the path, filtering will cause problem for broken link annoucnements
        {
            string strLSA = generateStrPath(destId);
            sendto(mySocketUDP, strLSA.c_str(), strLSA.length(), 0,
                   (struct sockaddr *)&allNodeSocketAddrs[i], sizeof(allNodeSocketAddrs[i]));

            string logContent = "Sent out my path update of destId ";
            logContent += to_string(destId);
            logContent += " to neighbor ";
            logContent += to_string(i);
            logContent += " : \n";
            logContent += strLSA;
            logMessage(logContent.c_str());
            logTime();
        }
    }
}

void checkNewAndLostNeighbor(int heardFrom)
{
    struct timeval now;
    gettimeofday(&now, 0);

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

        processSingleLSA(heardFrom, heardFrom, 0, heardFrom, set<int>{heardFrom});
        sharePathsToNewNeighbor(heardFrom);
    }

    //  record that we heard from heardFrom just now.
    gettimeofday(&previousHeartbeat[heardFrom], 0);

    // check if there is any neighbor link is broken, if so update pathRecords and broadcast LSA
    for (int i = 0; i < 256; i += 1)
    {
        if (i != myNodeId)
        {
            long timeDifference = (now.tv_sec - previousHeartbeat[i].tv_sec) * 1000000L + now.tv_usec - previousHeartbeat[i].tv_usec;
            if (previousHeartbeat[i].tv_sec != 0 && timeDifference > 600000) // missed two hearbeats
            {
                string logContent = "Link broken to node ";
                logContent += to_string(i);
                logMessage(logContent.c_str());
                logTime();

                handleBrokenLink(i);
            }
        }
    }
}

void sendOrFowdMessage(string buffContent, int bytesRecvd)
{
    const char *recvBuf = buffContent.c_str();

    short int destNodeId;
    memcpy(&destNodeId, recvBuf + 4, 2);
    destNodeId = ntohs(destNodeId);

    string logContent = "Received send or forward message.";
    logMessage(logContent.c_str());
    logTime();

    directMessage(destNodeId, buffContent, bytesRecvd + 1);
}

void processLSAMessage(string buffContent)
{
    string strLSA;
    strLSA.assign(buffContent.begin() + 4, buffContent.end() - 0);
    json LSA = json::parse(strLSA);

    int neighborId = LSA["fromID"];
    int destId = LSA["destID"];
    int distance = LSA["dist"];
    int nextHop = LSA["nextHop"];
    set<int> nodesInPath = LSA["nodesInPath"];

    string logContent = "Received path : \n";
    logContent += strLSA;
    logMessage(logContent.c_str());
    logTime();

    processSingleLSA(neighborId, destId, distance, nextHop, nodesInPath);
}

void processSingleLSA(int neighborId, int destId, int distance, int nextHop, set<int> nodesInPath)
{
    bool containsMyNodeId = nodesInPath.count(myNodeId) != 0;     // if neighbor LSA path has myNodeId in the path
    int myDistance = get<0>(myPaths[destId]);
    int myNexthop = get<1>(myPaths[destId]);

    if ((destId == myNodeId) || containsMyNodeId)
    {
        return;
    }

    if (distance != -1 && myDistance == -1)
    { // I do not have path to dest, but my neighbor has one. Use this neighbor path to destId
        m.lock();
        myPaths[destId] = {linkCost[neighborId] + distance, neighborId, nodesInPath.insert(myNodeId};
        m.unlock();
        sendPathToNeighbors(destId);
    }
    else if (distance != -1 && myDistance != -1)
    { // I have a path to dest, and my neighbor has one.
      // compare two paths, if neighbor path is better then update it in myPathToNode[destId]
        int newDistance = distance + linkCost[neighborId];
        if ((myDistance > newDistance) || (myDistance == newDistance && neighborId < myNexthop))
        {
            m.lock();
            myPaths[destId] = {newDistance, neighborId, nodesInPath.insert(myNodeId};
            m.unlock();
            sendPathToNeighbors(destId);
        }
    }
    else if (distance == -1 && myDistance != -1) // neighbor lost a path to destId
    {   
        if (get<1>(myPaths[destId]).count(neighborId) != 0) {
            // The neighbor is in my path to destId, then I lost the path to destId
            m.lock();
            get<0>(myPaths[destId]) = -1;
            m.unlock();
        } 

        sendPathToNeighbors(destId);  // broadcast my path to my neighbors (also covers the case if my path is not using the neighbor)
    }

    logContent = "Finished processing singleLSA path.";
    logMessage(logContent.c_str());
    logTime();
}

void sharePathsToNewNeighbor(int newNeighborId) // do we need to send desId if disance == -1
{
    for (int destId = 0; destId < 256; destId += 1)
    {
        if (destId != myNodeId && destId != newNeighborId && get<0>(myPaths[destId]) != -1 && get<2>(myPaths[destId]).count(newNeighborId) == 0)
        {
            string strLSA = generateStrPath(destId);
            sendto(mySocketUDP, strLSA.c_str(), strLSA.length(), 0,
                   (struct sockaddr *)&allNodeSocketAddrs[newNeighborId], sizeof(allNodeSocketAddrs[newNeighborId]));

            string logContent = "Shared my path of destId ";
            logContent += to_string(destId);
            logContent += " to new neighbor ";
            logContent += to_string(newNeighborId);
            logContent += " : \n";
            logContent += string(strLSA);
            logMessage(logContent.c_str());
            logTime();
        }
    }
}

void handleBrokenLink(int brokenNeighborId)
{
    for (int destId = 0; destId < 256; destId += 1)
    {
        if (destId != myNodeId && get<0>(myPaths[destId]) != -1 && get<1>(myPaths[destId]) == brokenNeighborId)
        {   // I lost the path to destId due to broken link to the neighbor
            m.lock();
            get<0>(myPaths[destId]) = -1;
            m.unlock();
            sendPathToNeighbors(destId);
        }
    }
}

void logMessage(const char *message)
{
    char logLine[BUFFERSIZE];
    sprintf(logLine, "Log: %s\n", message);
    fwrite(logLine, 1, strlen(logLine), flog);
    fflush(flog);
}

void logTime()
{
    struct timeval now;
    gettimeofday(&now, 0);

    char logLine[100];
    sprintf(logLine, "Processed at <%ld.%06ld>\n", (long int)(now.tv_sec), (long int)(now.tv_usec));
    fwrite(logLine, 1, strlen(logLine), flog);
    fflush(flog);
}

void logMyPaths()
{
    char logLine[BUFFERSIZE];
    json j_map(myPaths);
    string path = j_map.dump();
    sprintf(logLine, "%s\n", path.c_str());
    fwrite(logLine, 1, strlen(logLine), flog);
    fflush(flog);
}

void directMessage(int destNodeId, string message, int messageByte)
{
    char logLine[BUFFERSIZE];

    if (myNodeId == destNodeId)
    {
        sprintf(logLine, "receive packet message %s\n", message.c_str() + 6);
    }
    else
    {
        int distance = get<0>(myPaths[destNodeId]);
        if (distance != -1)
        {
            int nexthop = get<1>(myPaths[destNodeId]);

            if (!strncmp(message.c_str(), "send", 4))
            {
                char fowdMessage[messageByte];
                strcpy(fowdMessage, "fowd");
                memcpy(fowdMessage + 4, message.c_str() + 4, messageByte - 4);

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
        }
    }

    fwrite(logLine, 1, strlen(logLine), flog);
    fflush(flog);
}

void setupNodeSockets()
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
