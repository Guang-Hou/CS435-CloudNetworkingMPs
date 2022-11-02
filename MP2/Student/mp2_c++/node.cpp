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

int BUFFERSIZE = 8000;
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
            myPaths[i] = {0, myNodeId, unordered_set<int>{myNodeId}}; // for example of node 3: (0, (3,  {3})), the path includes the node itself
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

    string logContent = "Initialization.";
    logMessageAndTime(logContent.c_str());

    // cout << logContent << endl;
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

    // cout << "Finished reading cost file" << endl;
}

void broadcastLSA()
{
    // cout << "Inside broadcastLSA function." << endl;

    struct timespec sleepFor;
    sleepFor.tv_sec = 0;
    sleepFor.tv_nsec = 300 * 1000 * 1000; // 300 ms

    while (1)
    {
        string paths = generateStrPaths();

        for (int i = 0; i < 256; i += 1)
        {
            if (i != myNodeId)
            {
                sendto(mySocketUDP, paths.c_str(), paths.length(), 0,
                       (struct sockaddr *)&allNodeSocketAddrs[i], sizeof(allNodeSocketAddrs[i]));
            }
        }

        string logContent = "Sent out my LSA. ";
        logMessageAndTime(logContent.c_str());
        // cout << logContent << endl;

        nanosleep(&sleepFor, 0);
    }
}

void processNeighborHeartbeat(int heardFrom)
{
    // cout << "Inside processNeighborHeartbeat function." << endl;

    struct timeval now;
    gettimeofday(&now, 0);
    long previousSeenInSecond = previousHeartbeat[heardFrom].tv_sec;

    //  record that we heard from heardFrom just now.
    m.lock();
    gettimeofday(&previousHeartbeat[heardFrom], 0);
    m.unlock();

    if (previousSeenInSecond == 0)
    {
        string logContent = "Saw a new neighbor ";
        logContent += to_string(heardFrom);
        logMessageAndTime(logContent.c_str());
        //  cout << logContent << endl;

        m.lock();
        myNeighbors.insert(heardFrom);
        m.unlock();

        PATH selfPath = make_tuple(0, heardFrom, unordered_set<int>{heardFrom});
        processSingleLSA(heardFrom, heardFrom, selfPath);

        logContent = "Finished processing seeing new neighbor ";
        logContent += to_string(heardFrom);
        logMessageAndTime(logContent.c_str());
        // cout << logContent << endl;
    }
}

void checkLostNeighbor()
{
    // cout << "Inside checkLostNeighbor function." << endl;
    // check if there is any neighbor link is broken, if so update pathRecords and broadcast LSA
    for (int i = 0; i < 256; i += 1)
    {
        struct timeval now;
        gettimeofday(&now, 0);

        if (i != myNodeId)
        {
            long timeDifference = (now.tv_sec - previousHeartbeat[i].tv_sec) * 1000000L + now.tv_usec - previousHeartbeat[i].tv_usec;
            if (previousHeartbeat[i].tv_sec != 0 && timeDifference > 800000) // missed two hearbeats
            {
                string logContent = "Link broken to node ";
                logContent += to_string(i);

                logContent += ". The node was previously seen at ";
                logContent += to_string(previousHeartbeat[i].tv_sec);
                logContent += " s, and ";
                logContent += to_string(previousHeartbeat[i].tv_usec);
                logContent += " ms. \n";

                logContent += "Now the time is ";
                logContent += to_string(now.tv_sec);
                logContent += " s, and ";
                logContent += to_string(now.tv_usec);
                logContent += " ms. \n";

                logContent += "The time difference is  ";
                logContent += to_string(timeDifference);

                logMessageAndTime(logContent.c_str());
                // cout << logContent << endl;

                m.lock();
                previousHeartbeat[i].tv_sec = 0;
                myNeighbors.erase(i);
                m.unlock();

                handleBrokenLink(i);
            }
        }
    }
}

string generateStrPaths()
{
    // cout << "Inside generateStrPaths function." << endl;
    char payload[BUFFERSIZE];
    memset(payload, 0, BUFFERSIZE);

    strcpy(payload, "LSAs");

    json j_map(myPaths);
    string strLSA = j_map.dump();

    // cout << "strLSA length: " << strLSA.length() << endl;

    memcpy(payload + 4, strLSA.c_str(), strLSA.length());

    string payloadStr(payload, payload + 4 + strLSA.length() + 1);

    return payloadStr;
}

/*
void sendPathToNeighbors(int destId)
{
    for (int i = 0; i < 256; i += 1)
    {
        if (i != myNodeId && i != destId && get<2>(myPaths[destId]).count(i) == 0 && myNeighbors.count(i) == 1)
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
            logMessageAndTime(logContent.c_str());
        }
    }
}
*/

void sendOrFowdMessage(string buffContent, int bytesRecvd)
{
    const char *recvBuf = buffContent.c_str();

    short int destNodeId;
    memcpy(&destNodeId, recvBuf + 4, 2);
    destNodeId = ntohs(destNodeId);

    string logContent = "Received send or forward message.";
    logMessageAndTime(logContent.c_str());
    // cout << logContent << endl;

    directMessage(destNodeId, buffContent, bytesRecvd + 1);
}

void processLSAMessage(string buffContent, int neighborId)
{
    // cout << "Inside processLSAMessage function." << endl;
    string strLSA;
    strLSA.assign(buffContent.begin() + 4, buffContent.end() - 0);
    json LSA = json::parse(strLSA);
    auto neighborPaths = LSA.get<map<int, PATH>>();

    string logContent = "Received paths from neighbor node ";
    logContent += to_string(neighborId);
    // logContent += strLSA;
    logMessageAndTime(logContent.c_str());
    // cout << logContent << endl;

    for (int i = 0; i < 256; i += 1)
    {
        if (i != myNodeId && i != neighborId)
        {
            processSingleLSA(neighborId, i, neighborPaths[i]);
        }
    }

    logContent = "Finished processing the neighbor ";
    logContent += to_string(neighborId);
    logContent += " 's LSA ";

    logMessageAndTime(logContent.c_str());
    // cout << logContent << endl;
}

void processSingleLSA(int neighborId, int destId, PATH neighborPath)
{
    // cout << "Inside processSingleLSA function." << endl;
    int distance = get<0>(neighborPath);
    bool containsMyNodeId = get<2>(neighborPath).count(myNodeId) == 1; // if neighbor LSA path has myNodeId in the path
    int myDistance = get<0>(myPaths[destId]);
    int myNextHop = get<1>(myPaths[destId]);

    if ((destId == myNodeId) || containsMyNodeId)
    {
        return;
    }

    if (distance != -1 && myDistance == -1)
    { // I do not have path to dest, but my neighbor has one. Use this neighbor path to destId
        m.lock();
        get<0>(myPaths[destId]) = linkCost[neighborId] + distance;
        get<1>(myPaths[destId]) = neighborId;
        get<2>(myPaths[destId]) = get<2>(neighborPath);
        get<2>(myPaths[destId]).insert(myNodeId);
        m.unlock();
    }
    else if (distance != -1 && myDistance != -1)
    { // I have a path to dest, and my neighbor has one.
      // compare two paths, if neighbor path is better then update it in myPathToNode[destId]
        int newDistance = distance + linkCost[neighborId];
        if ((myDistance > newDistance) || (myDistance == newDistance && neighborId < myNextHop))
        {
            m.lock();
            get<0>(myPaths[destId]) = linkCost[neighborId] + distance;
            get<1>(myPaths[destId]) = neighborId;
            get<2>(myPaths[destId]) = get<2>(neighborPath);
            get<2>(myPaths[destId]).insert(myNodeId);
            m.unlock();
        }
    }
    else if (distance == -1 && myDistance != -1) // neighbor lost a path to destId
    {
        if (get<2>(myPaths[destId]).count(neighborId) == 1)
        {
            // The neighbor is in my path to destId, then I lost the path to destId
            m.lock();
            get<0>(myPaths[destId]) = -1;
            get<2>(myPaths[destId]).clear();
            m.unlock();
        }
    }
}

/*
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
            logContent += " : ";
            logContent += string(strLSA);
            logMessageAndTime(logContent.c_str());
        }
    }
}
*/

void handleBrokenLink(int neighborId)
{
    for (int destId = 0; destId < 256; destId += 1)
    {
        if (destId != myNodeId && destId != neighborId && get<0>(myPaths[destId]) != -1 && get<1>(myPaths[destId]) == neighborId)
        { // I lost the path to destId due to broken link to the neighbor
            m.lock();
            get<0>(myPaths[destId]) = -1;
            get<2>(myPaths[destId]).clear();
            m.unlock();
        }
    }
}

void logMessageAndTime(const char *message)
{
    // return;

    char logLine[BUFFERSIZE];
    sprintf(logLine, "Log: %s\n", message);
    fwrite(logLine, 1, strlen(logLine), flog);
    fflush(flog);
    logTime();
}

void logTime()
{
    struct timeval now;
    gettimeofday(&now, 0);

    char logLine[100];
    sprintf(logLine, "Time is at <%ld.%06ld>\n", (long int)(now.tv_sec), (long int)(now.tv_usec));
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
