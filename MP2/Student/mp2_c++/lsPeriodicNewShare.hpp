/* Link State Routing
 - Periodically send LSA only if my link status changes. New link status will be sent out. If no change, then no LSA.
 - Upon receiving LSA,
    - check ttl if it is still fresh. If it is too old, discard it.
    - immediately forward it out to my neighbors except myself and the predecessor
    - use it to update my graph database but do not send my LSA out.
 - For new neighbor, share my new valid links as LSA to it.
*/

#pragma once
#include "json.hpp"
#include <iostream>
#include <sstream>
#include <sys/time.h>
#include <time.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <climits>
#include <string>
#include <string.h>
#include <thread>
#include <mutex>
#include <stdio.h>
#include <netdb.h>
#include <map>
#include <queue>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#define BUFFERSIZE 3000

using json = nlohmann::json;
using namespace std;

typedef std::pair<int, int> DN; // Used for Dijkstra's algorithm, total distance to source node and this node's id

int myNodeId, mySocketUDP;
struct timeval previousHeartbeat[256]; // track last time a neighbor is seen to figure out who is lost
struct sockaddr_in allNodeSocketAddrs[256];
int linkCost[256];
int seqRecord[256];
map<int, map<int, int>> graph;  // graph[i][j] indicates i->j link cost, -1 means not connected
std::mutex change_lock;
std::mutex graph_lock;
bool isChanged;                 // my link status has changed since last time send out LSA
char packetBuffer[BUFFERSIZE];
int packetSize;
FILE* flog;

void init(int inputId, string costFile, string logFile);
void readCostFile(const char* costFile);

void* announceHeartbeat(void* unusedParam);
void* announceLSA(void* unusedParam);
void detectNeighbors();
void listenForNeighbors();
void checkNewNeighbor(int heardFrom);
void checkLostNeighbor();

void generateStrLSA(int nodeId);
void sendLSAToNeighbors();
bool processLSAMessage(const char* recvBuf, int bytesRecvd, int heardFrom);
//void sendReceivedLSAToOtherNeighbors(string buffContent, int bytesRecvd, int neighborId, int destId);
//void directMessage(string buffContent, int bytesRecvd);
int getNextHop(int destId);

void setupNodeSockets();
void logMessageAndTime(const char* message);
void logTime();

// void testDij(int MyNodeId, int destId);

void init(int inputId, string costFile, string logFile) {
    myNodeId = inputId;

    // initailize linkCost and myPaths
    for (int i = 0; i < 256; i += 1) {
        linkCost[i] = 1;
        seqRecord[i] = 0;

        for (int j = 0; j < 256; j += 1) {
            graph[i][j] = -1;
        }
    }

    linkCost[myNodeId] = 0;
    graph[myNodeId][myNodeId] = 0;

    setupNodeSockets();
    readCostFile(costFile.c_str());

    flog = fopen(logFile.c_str(), "a");
}


void* announceHeartbeat(void* unusedParam) {
    struct timespec sleepFor;
    sleepFor.tv_sec = 0;
    sleepFor.tv_nsec = 300 * 1000 * 1000; // 300 ms
    const char* heartBeats = "H";

    while (1) {
        for (int i = 0; i < 256; i += 1) {
            if (i != myNodeId)
            {
                sendto(mySocketUDP, heartBeats, 2, 0,
                    (struct sockaddr*)&allNodeSocketAddrs[i], sizeof(allNodeSocketAddrs[i]));
            }
        }
        nanosleep(&sleepFor, 0);
    }
}

void detectNeighbors() {
    //cout << "entering detectNeighbor" << endl;
    char fromAddr[100];
    struct sockaddr_in theirAddr;
    socklen_t theirAddrLen = sizeof(theirAddr);
    char recvBuf[BUFFERSIZE];
    int bytesRecvd;

    auto start = std::chrono::steady_clock::now();

    while (1) {
        memset(recvBuf, 0, sizeof(recvBuf));
        if ((bytesRecvd = recvfrom(mySocketUDP, recvBuf, BUFFERSIZE, 0,
            (struct sockaddr*)&theirAddr, &theirAddrLen)) == -1) {
            perror("connectivity listener: recvfrom failed");
            exit(1);
        }
        inet_ntop(AF_INET, &theirAddr.sin_addr, fromAddr, 100);

        short int heardFrom = -1;
        if (strstr(fromAddr, "10.1.1.")) {
            heardFrom = atoi(
                strchr(strchr(strchr(fromAddr, '.') + 1, '.') + 1, '.') + 1);

            graph_lock.lock();
            change_lock.lock();
            graph[myNodeId][heardFrom] = linkCost[heardFrom];
            graph[heardFrom][myNodeId] = linkCost[heardFrom];
            isChanged = true;
            change_lock.unlock();
            graph_lock.unlock();

            gettimeofday(&previousHeartbeat[heardFrom], 0);
        }

        auto end = std::chrono::steady_clock::now();
        double elapsed_time = double(std::chrono::duration_cast <std::chrono::nanoseconds> (end - start).count());
        if (elapsed_time > 500 * 1000 * 1000) {  // 500 ms
            break;
        }
    }
    //cout << "existing detectNeighbor" << endl;
    return;
}


void* announceLSA(void* unusedParam) {
    struct timespec sleepFor;
    sleepFor.tv_sec = 0;
    sleepFor.tv_nsec = 300 * 1000 * 1000; // 400 ms
    bool hasChanged = false;

    while (1) {
        graph_lock.lock();
        change_lock.lock();

        checkLostNeighbor();

        if (isChanged == true) {
            hasChanged = true;
        }

        graph_lock.unlock();
        change_lock.unlock();

        if (hasChanged) {
            graph_lock.lock();
            change_lock.lock();
            isChanged = false;
            generateStrLSA(myNodeId);
            change_lock.unlock();
            graph_lock.unlock();

            sendLSAToNeighbors();
            string logContent = "CHANGED isChanged to false WHEN SENDING OUT LSA. ";
            logMessageAndTime(logContent.c_str());
        }
        hasChanged = false;
        nanosleep(&sleepFor, 0);
    }
}

void listenForNeighbors() {
    // cout << "Inside listenForNeighbors function." << endl;

    char fromAddr[100];
    struct sockaddr_in theirAddr;
    socklen_t theirAddrLen = sizeof(theirAddr);
    char recvBuf[BUFFERSIZE];
    int bytesRecvd;

    //isChanged = true;

    while (1) {
        memset(recvBuf, 0, sizeof(recvBuf));
        if ((bytesRecvd = recvfrom(mySocketUDP, recvBuf, BUFFERSIZE, 0,
            (struct sockaddr*)&theirAddr, &theirAddrLen)) == -1) {
            perror("connectivity listener: recvfrom failed");
            exit(1);
        }
        inet_ntop(AF_INET, &theirAddr.sin_addr, fromAddr, 100);

        short int heardFrom = -1;
        if (strstr(fromAddr, "10.1.1.")) {
            heardFrom = atoi(
                strchr(strchr(strchr(fromAddr, '.') + 1, '.') + 1, '.') + 1);

            //TODO: this node can consider heardFrom to be directly connected to it; do any such logic now.
            graph_lock.lock();
            change_lock.lock();
            checkNewNeighbor(heardFrom);
            change_lock.unlock();
            graph_lock.unlock();

            gettimeofday(&previousHeartbeat[heardFrom], 0);
        }

        // send/forward message 
        if (!strncmp(recvBuf, "send", 4) || !strncmp(recvBuf, "fowd", 4)) {
            char logLine[100];
            short int destNodeId;
            memcpy(&destNodeId, recvBuf + 4, 2);
            destNodeId = ntohs(destNodeId);

            if (myNodeId == destNodeId) {
                sprintf(logLine, "receive packet message %s\n", recvBuf + 6);
            }
            else {
                int nexthop = getNextHop(destNodeId);
                if (nexthop != -1) {
                    if (!strncmp(recvBuf, "send", 4)) {
                        strcpy(recvBuf, "fowd");
                        sendto(mySocketUDP, recvBuf, bytesRecvd, 0,
                            (struct sockaddr*)&allNodeSocketAddrs[nexthop], sizeof(allNodeSocketAddrs[nexthop]));
                        sprintf(logLine, "sending packet dest %d nexthop %d message %s\n", destNodeId, nexthop, recvBuf + 6);
                    }
                    else if (!strncmp(recvBuf, "fowd", 4)) {
                        sendto(mySocketUDP, recvBuf, bytesRecvd, 0,
                            (struct sockaddr*)&allNodeSocketAddrs[nexthop], sizeof(allNodeSocketAddrs[nexthop]));
                        sprintf(logLine, "forward packet dest %d nexthop %d message %s\n", destNodeId, nexthop, recvBuf + 6);
                    }
                }
                else {
                    sprintf(logLine, "unreachable dest %d\n", destNodeId);
                }
            }

            fwrite(logLine, 1, strlen(logLine), flog);
            fflush(flog);
        }
        // LSA message
        else if (!strncmp(recvBuf, "LSAs", 4)) {
            graph_lock.lock();
            //change_lock.lock();
            bool graphChanged = processLSAMessage(recvBuf, bytesRecvd, heardFrom);
            //change_lock.unlock();
            graph_lock.unlock();

            if (graphChanged == true) {
                graph_lock.lock();
                for (int i = 0; i < 256; i += 1) {
                    if (i != myNodeId && i != heardFrom && graph[myNodeId][i] != -1) {
                        sendto(mySocketUDP, recvBuf, bytesRecvd, 0,
                            (struct sockaddr*)&allNodeSocketAddrs[i], sizeof(allNodeSocketAddrs[i]));
                    }
                }
                graph_lock.unlock();
            }
        }
    }
    close(mySocketUDP);
}

void checkNewNeighbor(int heardFrom)
{
    // cout << "Inside checkNewNeighbor function." << endl;

    if (graph[myNodeId][heardFrom] != -1) {
        return;
    }

    std::string logContent = "Saw a new neighbor ";
    logContent += std::to_string(heardFrom);
    logMessageAndTime(logContent.c_str());
    //  cout << logContent << endl;

    //graph_lock.lock();
    //change_lock.lock();
    graph[myNodeId][heardFrom] = linkCost[heardFrom];
    graph[heardFrom][myNodeId] = linkCost[heardFrom];
    isChanged = true;
    //change_lock.unlock();
    //graph_lock.unlock();
    for (int i = 0; i < 256; i += 1) {
        if (seqRecord[i] > 0) {
            generateStrLSA(i);
            sendto(mySocketUDP, packetBuffer, packetSize, 0,
                (struct sockaddr*)&allNodeSocketAddrs[heardFrom], sizeof(allNodeSocketAddrs[heardFrom]));
        }
    }

    logContent = "CHANGED isChanged to true WHEN SEEING A NEW NEIGHBOR. ";
    logMessageAndTime(logContent.c_str());

    //thread syncSeqThread(syncSeq, heardFrom);
    //syncSeqThread.detach();

}

void checkLostNeighbor()     //  check if there is any neighbor link is broken
{
    // cout << "Inside checkLostNeighbor function." << endl;

    struct timeval now;
    gettimeofday(&now, 0);

    for (int i = 0; i < 256; i += 1)
    {
        if (i != myNodeId && graph[myNodeId][i] != -1)
        {
            long timeDifference = (now.tv_sec - previousHeartbeat[i].tv_sec) * 1000000L + now.tv_usec - previousHeartbeat[i].tv_usec;
            if (timeDifference > 900000) // saw befor in longer than 900 ms
            {
                char buff[200];
                snprintf(buff, sizeof(buff),
                    "  Link broken to node %d. The node was previously seen at %ld s, and %ld us.  Now the time is %ld s, and %ld us. The time difference is %ld.",
                    i, previousHeartbeat[i].tv_sec, previousHeartbeat[i].tv_usec, now.tv_sec, now.tv_usec, timeDifference);
                logMessageAndTime(buff);

                //graph_lock.lock();
                graph[myNodeId][i] = -1;
                graph[i][myNodeId] = -1;
                //graph_lock.unlock();

                //change_lock.lock();
                isChanged = true;
                //change_lock.unlock();

                string logContent = "CHANGED isChanged to true WHEN LOST A NEIGHBOR. ";
                logMessageAndTime(logContent.c_str());
            }
        }
    }
}

void sendLSAToNeighbors() {
    graph_lock.lock();
    for (int i = 0; i < 256; i += 1) {
        if (i != myNodeId && graph[myNodeId][i] != -1) {
            sendto(mySocketUDP, packetBuffer, packetSize, 0,
                (struct sockaddr*)&allNodeSocketAddrs[i], sizeof(allNodeSocketAddrs[i]));
        }
    }
    graph_lock.unlock();
}

void generateStrLSA(int nodeId)  // it is inside a thread of announceLSA
{
    // cout << "Inside sendMyLSAToNeighbors function." << endl;

    struct timeval now;
    gettimeofday(&now, 0);

    //graph_lock.lock();
    //map<int, int> links = graph[myNodeId];
    //graph_lock.unlock();

    //seq_lock.lock();
    int seqNum;

    if (nodeId == myNodeId) {
        seqRecord[myNodeId] += 1;
        seqNum = seqRecord[myNodeId];
    }
    else {
        seqNum = seqRecord[nodeId];
    }
    //seq_lock.unlock();

    memset(packetBuffer, 0, BUFFERSIZE);
    strcpy(packetBuffer, "LSAs");

    map<int, int> validLinks;

    for (int i = 0; i < 256; i += 1) {
        if (graph[nodeId][i] != -1) {
            validLinks[i] = graph[nodeId][i];
        }
    }

    nlohmann::json LSA = {
        {"sourceId", nodeId},
        {"seq", seqNum},
        {"validLinks", validLinks} };

    std::string strLSA = LSA.dump();
    memcpy(packetBuffer + 4, strLSA.c_str(), strLSA.length());

    packetSize = strLSA.length() + 4;

    //string logContent = "My link status changed, and the wait time passed, sent out my updated LSA links. ";
    //logContent += strLSA;
    //logMessageAndTime(logContent.c_str());
    //  cout << logContent << endl;
}

bool processLSAMessage(const char* recvBuf, int bytesRecvd, int neighborId)
{
    // cout << "Inside processLSAMessage function." << endl;
    //string logContent = "    Entering processLSAMessage function, received LSA.";
    //logMessageAndTime(logContent.c_str());
    // cout << logContent << endl;

    std::string strLSA;
    strLSA.assign(recvBuf + 4, recvBuf + bytesRecvd);

    //string logContent = "Received LSA of size ";
    //logContent += to_string(bytesRecvd);
    //logMessageAndTime(logContent.c_str());

    //cout << logContent << endl;
    nlohmann::json LSA = nlohmann::json::parse(strLSA);
    int sourceId = LSA["sourceId"];
    int receivedSeq = LSA["seq"];

    //seq_lock.lock();
    //int sourceSeqInRecord = seq[sourceId];
    //seq_lock.unlock();

    if (sourceId == myNodeId || receivedSeq <= seqRecord[sourceId]) {
        char buff[200];
        snprintf(buff, sizeof(buff), "Old LSA from neighbor %d, for sourceId %d, the received seq number is %d, and my recorded seq is %d.", neighborId, sourceId, receivedSeq, seqRecord[sourceId]);
        logMessageAndTime(buff);
        return false;
    }

    //seq_lock.lock();
    seqRecord[sourceId] = receivedSeq;
    //seq_lock.unlock();

    map<int, int> otherLinks = LSA["validLinks"];

    string logContent = "New LSA: ";
    logContent += strLSA;
    logMessageAndTime(logContent.c_str());

    //graph_lock.lock();
    for (int destId = 0; destId < 256; destId += 1) {
        if (otherLinks.find(destId) != otherLinks.end()) {
            graph[sourceId][destId] = otherLinks[destId];
        }
        else {

            graph[sourceId][destId] = -1;
        }
    }
    //graph_lock.unlock();

    return true;
    // cout << logContent << endl;
}

/*
void processLSAMessage(string buffContent, int bytesRecvd, int neighborId)
{
    // cout << "Inside processLSAMessage function." << endl;
    // string logContent = "    Entering processLSAMessage function, received LSA.";
    // logMessageAndTime(logContent.c_str());
    // cout << logContent << endl;

    std::string strLSA;
    strLSA.assign(buffContent.c_str() + 4, buffContent.c_str() + bytesRecvd);

    struct timeval now;
    gettimeofday(&now, 0);

    //string logContent = "Received LSA of size ";
    //logContent += to_string(bytesRecvd);
    //logMessageAndTime(logContent.c_str());

    //cout << logContent << endl;
    nlohmann::json LSA = nlohmann::json::parse(strLSA);
    int sourceId = LSA["sourceId"];
    int receivedSeq = LSA["seq"];
    long timestamp = (now.tv_sec * 1000000L + now.tv_usec) / 1000;  // unit in ms

    //seq_lock.lock();
    //int sourceSeqInRecord = seq[sourceId];
    //seq_lock.unlock();

    if (sourceId == myNodeId || receivedSeq <= seqRecord[sourceId])  // || timestamp > ttl -> this is disabled for now
    {
        char buff[200];
        snprintf(buff, sizeof(buff), "Old LSA from neighbor %d, for sourceId %d, the received seq number is %d, and my recorded seq is %d.", neighborId, sourceId, receivedSeq, seqRecord[sourceId]);
        logMessageAndTime(buff);
        return;
    }
    else
    {
        //seq_lock.lock();
        seqRecord[sourceId] = receivedSeq;
        //seq_lock.unlock();

        map<int, int> otherLinks = LSA["links"];

        //graph_lock.lock();
        for (int destId = 0; destId < 256; destId += 1)
        {
            graph[sourceId][destId] = otherLinks[destId];
        }
        //graph_lock.unlock();

        char buff[200];
        snprintf(buff, sizeof(buff), "New LSA from neighbor %d, for sourceId %d, with seq number %d.", neighborId, sourceId, receivedSeq);
        logMessageAndTime(buff);
        logMessageAndTime(strLSA.c_str());

        //sendReceivedLSAToOtherNeighbors(buffContent, bytesRecvd, neighborId, sourceId);

        for (int i = 0; i < 256; i += 1)
        {
            bool isNeighbor = graph[myNodeId][i] != -1;

            if (i != neighborId && i != sourceId && i != myNodeId && isNeighbor)
            {
                sendto(mySocketUDP, buffContent.c_str(), bytesRecvd, 0,
                    (struct sockaddr*)&allNodeSocketAddrs[i], sizeof(allNodeSocketAddrs[i]));

                string logContent = "        Resent received LSA from neighbor node ";
                logContent += to_string(neighborId);
                logContent += " for sourceId ";
                logContent += to_string(sourceId);
                logContent += " to other neighbor ";
                logContent += to_string(i);
                logMessageAndTime(logContent.c_str());
            }
        }

    }
    // cout << logContent << endl;
}
*/

void sendReceivedLSAToOtherNeighbors(string buffContent, int bytesRecvd, int neighborId, int sourceId)
{
    // cout << "Inside sendReceivedLSAToOtherNeighbors function." << endl;
    //graph_lock.lock();
    //map<int, int> links = graph[myNodeId];
    //graph_lock.unlock();

    for (int i = 0; i < 256; i += 1)
    {
        bool isNeighbor = graph[myNodeId][i] != -1;

        if (i != neighborId && i != sourceId && i != myNodeId && isNeighbor)
        {
            sendto(mySocketUDP, buffContent.c_str(), bytesRecvd, 0,
                (struct sockaddr*)&allNodeSocketAddrs[i], sizeof(allNodeSocketAddrs[i]));

            // string logContent = "        Resent received LSA from neighbor node ";
            // logContent += to_string(neighborId);
            // logContent += " for sourceId ";
            // logContent += to_string(sourceId);
            // logContent += " to other neighbor ";
             //logContent += to_string(i);
             //logMessageAndTime(logContent.c_str());
        }
    }
    // cout << logContent << endl;
}

// run Dijkstra's algorithm to find nextHop to reach destId
int getNextHop(int destId)
{
    /*
    string logContent = "Dij for destId: ";
    logContent += to_string(destId);
    json j_map1(graph[myNodeId]);
    logContent += j_map1.dump();
    logMessageAndTime(logContent.c_str());

    json j_map2(graph[1]);
    logContent = j_map2.dump();
    logMessageAndTime(logContent.c_str());

    json j_map3(graph[5]);
    logContent = j_map3.dump();
    logMessageAndTime(logContent.c_str());

    json j_map4(graph[7]);
    logContent = j_map4.dump();
    logMessageAndTime(logContent.c_str());
    */

    // cout << "Inside getNextHop function." << endl;
    int prev[256];             // i's previous node in path to desId
    int distanceToMyNode[256]; // i's total distance to desId
    prev[destId] = -1;
    bool visited[256];
    visited[myNodeId] = true;

    std::priority_queue<DN, std::vector<DN>, std::greater<DN>> frontier;
    frontier.push(std::make_pair(0, myNodeId));

    for (int i = 0; i < 256; i += 1)
    {
        distanceToMyNode[i] = INT_MAX;
    }
    distanceToMyNode[myNodeId] = 0;

    bool found = false;

    //graph_lock.lock();
    while (!frontier.empty())
    {
        int size = frontier.size();

        for (int i = 0; i < size; i += 1)
        {
            int dist = frontier.top().first;
            int u = frontier.top().second;

            frontier.pop();

            if (u == destId)
            {
                break;
            }

            for (int v = 0; v < 256; v += 1)
            {
                if (graph[u][v] != -1 && !visited[v])
                {
                    if (distanceToMyNode[v] > distanceToMyNode[u] + graph[u][v])
                    {
                        distanceToMyNode[v] = distanceToMyNode[u] + graph[u][v];
                    }

                    prev[v] = u;
                    frontier.push(std::make_pair(distanceToMyNode[v], v));
                    visited[v] = true;
                }
            }
        }

        if (found)
        {
            break;
        }
    }
    //graph_lock.unlock();

    if (prev[destId] == -1)
    {
        // string logContent = "In getNextHop, not find next hop";
        // logMessageAndTime(logContent.c_str());
        return -1;
    }

    int p = destId;
    while (prev[p] != myNodeId)
    {
        p = prev[p];
    }

    // string logContent = "In getNextHop, found next hop";
    // logMessageAndTime(logContent.c_str());

    return p;
}

/*
void directMessage(string buffContent, int bytesRecvd)
{
    short int destNodeId;
    memcpy(&destNodeId, buffContent.c_str() + 4, 2);
    destNodeId = ntohs(destNodeId);

    //string logContent = "Received send or forward message.";
    //logContent += string(recvBuf + 4);
    //logMessageAndTime(logContent.c_str());
    // cout << logContent << endl;

    char logLine[BUFFERSIZE];

    if (myNodeId == destNodeId)
    {
        sprintf(logLine, "receive packet message %s\n", buffContent.c_str() + 6);
    }
    else
    {
        int nexthop = getNextHop(destNodeId);
        if (nexthop != -1)
        {
            if (!strncmp(buffContent.c_str(), "send", 4))
            {
                char fowdMessage[bytesRecvd];
                memset(fowdMessage, 0, bytesRecvd + 1);

                strcpy(fowdMessage, "fowd");
                memcpy(fowdMessage + 4, buffContent.c_str() + 4, bytesRecvd - 4);

                sendto(mySocketUDP, fowdMessage, bytesRecvd, 0,
                    (struct sockaddr*)&allNodeSocketAddrs[nexthop], sizeof(allNodeSocketAddrs[nexthop]));

                sprintf(logLine, "sending packet dest %d nexthop %d message %s\n", destNodeId, nexthop, buffContent.c_str() + 6);
            }
            else if (!strncmp(buffContent.c_str(), "fowd", 4))
            {
                sendto(mySocketUDP, buffContent.c_str(), bytesRecvd, 0,
                    (struct sockaddr*)&allNodeSocketAddrs[nexthop], sizeof(allNodeSocketAddrs[nexthop]));

                sprintf(logLine, "forward packet dest %d nexthop %d message %s\n", destNodeId, nexthop, buffContent.c_str() + 6);
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

*/

void readCostFile(const char* costFile) {
    FILE* fcost = fopen(costFile, "r");

    if (fcost == NULL) {
        return;
    }

    int destNodeId, cost;
    while (fscanf(fcost, "%d %d", &(destNodeId), &(cost)) != EOF) {
        linkCost[destNodeId] = cost;
    }
    fclose(fcost);
    //cout << "Finished reading cost file" << endl;
}

void setupNodeSockets() {
    //std::cout << "Inside setup socket function." << std::endl;
    for (int i = 0; i < 256; i++) {
        char tempaddr[100];
        sprintf(tempaddr, "10.1.1.%d", i);
        memset(&allNodeSocketAddrs[i], 0, sizeof(allNodeSocketAddrs[i]));
        allNodeSocketAddrs[i].sin_family = AF_INET;
        allNodeSocketAddrs[i].sin_port = htons(7777);
        inet_pton(AF_INET, tempaddr, &allNodeSocketAddrs[i].sin_addr);
    }

    // socket() and bind() our socket. We will do all sendto()ing and recvfrom()ing on this one.
    if ((mySocketUDP = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket error");
        exit(1);
    }

    struct sockaddr_in bindAddr = allNodeSocketAddrs[myNodeId];
    if (bind(mySocketUDP, (struct sockaddr*)&bindAddr, sizeof(struct sockaddr_in)) < 0) {
        perror("bind error");
        close(mySocketUDP);
        exit(2);
    }
}

void logMessageAndTime(const char* message) {
    return;
    char logLine[BUFFERSIZE];
    sprintf(logLine, " %s\n", message);
    fwrite(logLine, 1, strlen(logLine), flog);
    fflush(flog);
    logTime();
}

void logTime() {
    struct timeval now;
    gettimeofday(&now, 0);

    char logLine[100];
    sprintf(logLine, "    Time is at %ld ms.\n", (now.tv_sec * 1000000L + now.tv_usec) / 1000);
    fwrite(logLine, 1, strlen(logLine), flog);
    fflush(flog);
}

/*
void testDij(int nodeId, int destId)
{
    map<int, map<int, int>> theG;

    theG[0] = {{255, 555}};
    theG[1] = {{2, 54}, {4, 1}, {5, 2}, {6, 1}, {255, 1}};
    theG[2] = {{1, 54}, {3, 1}, {5, 1}};
    theG[3] = {{2, 1}, {4, 1}};
    theG[4] = {{1, 1}, {3, 1}, {7, 1}};
    theG[5] = {{1, 2}, {2, 2}, {6, 1}};
    theG[6] = {{7, 3}, {1, 1}, {5, 1}};
    theG[7] = {{6, 3}, {4, 1}};
    theG[255] = {{0, 555}, {1, 1}};

    for (int i = 0; i < 256; i += 1)
    {
        for (int j = 0; j < 256; j += 1)
        {
            if (theG[i][j] == 0)
            {
                theG[i][j] = -1;
            }
        }
    }

    theG[nodeId][nodeId] = 0;

    int prev[256];             // i's previous node in path to desId
    int distanceToMyNode[256]; // i's total distance to desId
    prev[destId] = -1;
    bool visited[256];
    visited[nodeId] = true;

    priority_queue<DN, std::vector<DN>, greater<DN>> frontier;
    frontier.push(make_pair(0, nodeId));

    for (int i = 0; i < 256; i += 1)
    {
        distanceToMyNode[i] = INT_MAX;
    }
    distanceToMyNode[nodeId] = 0;

    bool found = false;
    while (!frontier.empty())
    {
        int size = frontier.size();

        for (int i = 0; i < size; i += 1)
        {
            int dist = frontier.top().first;
            int u = frontier.top().second;

            frontier.pop();
            cout << "Poped out: ( " << dist << ", " << u << ")." << endl;

            if (u == destId)
            {
                found = true;
                break;
            }

            for (int v = 0; v < 256; v += 1)
            {
                //  cout << "theG[u][v]: " << theG[u][v] << endl;
                if (theG[u][v] != -1 && !visited[v])
                {
                    cout << "checking new edge node " << v << endl;
                    cout << "Three distances: " << distanceToMyNode[v] << ", " << distanceToMyNode[u] << ", " << theG[u][v] << endl;
                    if (distanceToMyNode[v] > distanceToMyNode[u] + theG[u][v])
                    {
                        distanceToMyNode[v] = distanceToMyNode[u] + theG[u][v];
                    }

                    prev[v] = u;
                    cout << "In prev[v] = u, the v is " << v << ", and the u is " << u << endl;
                    frontier.push(make_pair(distanceToMyNode[v], v));
                    cout << "Added: ( " << distanceToMyNode[v] << ", " << v << ")." << endl;
                    visited[v] = true;
                }
            }
        }

        if (found)
        {
            break;
        }
    }

    if (prev[destId] == -1)
    {
        cout << "Failed!" << endl;
    }

    int p = destId;
    while (prev[p] != nodeId)
    {
        cout << " " << p << endl;
        p = prev[p];
    }
    cout << " " << p << endl;
}
*/
