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
#include <atomic>
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
std::mutex graph_lock;
atomic<bool> isChanged(false);                 // my link status has changed since last time send out LSA
FILE* flog;

void init(int inputId, string costFile, string logFile);
void readCostFile(const char* costFile);

void* announceHeartbeat(void* unusedParam);
void* announceLSA(void* unusedParam);
void detectNeighbors();
void listenForNeighbors();
void checkNewNeighbor(int heardFrom);
void checkLostNeighbor();

void syncSeq(int newNeighborId);
void processSeqShare(const char* recvBuf, int bytesRecvd, int newNeighborId);

void sendLSAToNeighbors();
void processLSAMessage(string buffContent, int bytesRecvd, int heardFrom);
int getNextHop(int destId);
void directMessage(string buffContent, int bytesRecvd);

void setupNodeSockets();
void logMessageAndTime(const char* message);
void logTime();

std::thread th[4];

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
                sendto(mySocketUDP, heartBeats, 1, 0,
                    (struct sockaddr*)&allNodeSocketAddrs[i], sizeof(allNodeSocketAddrs[i]));
            }
        }
        nanosleep(&sleepFor, 0);
    }
}

void* announceLSA(void* unusedParam) {
    struct timespec sleepFor;
    sleepFor.tv_sec = 0;
    sleepFor.tv_nsec = 400 * 1000 * 1000; // 400 ms

    while (1) {
        //checkLostNeighbor();
        struct timeval now;
        gettimeofday(&now, 0);

        for (int i = 0; i < 256; i += 1) {
            if (i != myNodeId && graph[myNodeId][i] != -1) {
                long timeDifference = (now.tv_sec - previousHeartbeat[i].tv_sec) * 1000000L + now.tv_usec - previousHeartbeat[i].tv_usec;
                if (timeDifference > 800000) { // saw befor in longer than 800 ms

                    graph_lock.lock();
                    graph[myNodeId][i] = -1;
                    graph[i][myNodeId] = -1;
                    graph_lock.unlock();

                    isChanged = true;
                }
            }
        }

        if (isChanged) {
            sendLSAToNeighbors();
            isChanged = false;
        }
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
            heardFrom = atoi(strchr(strchr(strchr(fromAddr, '.') + 1, '.') + 1, '.') + 1);

            graph_lock.lock();
            if (graph[myNodeId][heardFrom] == -1) {
                graph[myNodeId][heardFrom] = linkCost[heardFrom];
                graph[heardFrom][myNodeId] = linkCost[heardFrom];
                isChanged = true;
            }
            graph_lock.unlock();

            gettimeofday(&previousHeartbeat[heardFrom], 0);
        }

        // send/forward message 
        if (!strncmp(recvBuf, "send", 4) || !strncmp(recvBuf, "fowd", 4)) {
            string buffContent;
            buffContent.assign(recvBuf, recvBuf + bytesRecvd);

            th[0] = thread(directMessage, buffContent, bytesRecvd);
            th[0].detach();
        }
        // LSA message
        else if (!strncmp(recvBuf, "LSAs", 4)) {
            string buffContent;
            buffContent.assign(recvBuf, recvBuf + bytesRecvd);

            th[1] = thread(processLSAMessage, buffContent, bytesRecvd, heardFrom);
            th[1].detach();

        } /*
        else if (!strncmp(recvBuf, "seqs", 4)) {
            //processSeqShare(recvBuf, bytesRecvd, heardFrom);
        }
        */
    }
    close(mySocketUDP);
}

void checkNewNeighbor(int heardFrom)
{
    // cout << "Inside checkNewNeighbor function." << endl;

    if (graph[myNodeId][heardFrom] != -1) {
        return;
    }

    graph[myNodeId][heardFrom] = linkCost[heardFrom];
    graph[heardFrom][myNodeId] = linkCost[heardFrom];
    isChanged = true;

}

void syncSeq(int newNeighborId) // share my seq records to new neighbor
{
    char payload[BUFFERSIZE];
    memset(payload, 0, BUFFERSIZE);

    strcpy(payload, "seqs");

    map<int, int> validSeq;

    for (int i = 0; i < 256; i += 1) {
        if (i != myNodeId && seqRecord[i] > 0) {
            validSeq[i] = seqRecord[i];
        }
    }

    nlohmann::json LSA = {
        {"seqRecord", validSeq},
    };

    std::string strLSA = LSA.dump();
    memcpy(payload + 4, strLSA.c_str(), strLSA.length());

    sendto(mySocketUDP, payload, strLSA.length() + 5, 0,
        (struct sockaddr*)&allNodeSocketAddrs[newNeighborId], sizeof(allNodeSocketAddrs[newNeighborId]));
}

void processSeqShare(const char* recvBuf, int bytesRecvd, int newNeighborId) // when receiving header "seqs"
{
    std::string strSeq;
    strSeq.assign(recvBuf + 4, recvBuf + bytesRecvd);

    nlohmann::json LSA = nlohmann::json::parse(strSeq);
    map<int, int> otherSeq = LSA["seqRecord"];

    for (int i = 0; i < 256; i += 1)
    {
        if (i != newNeighborId && i != myNodeId && seqRecord[i] > 0 && seqRecord[i] > otherSeq[i]) // do not need to share mine, a new neibhbor is an enent triggering a LSA already
        {
            char packetBuffer[BUFFERSIZE];
            int packetSize;
            memset(packetBuffer, 0, BUFFERSIZE);
            strcpy(packetBuffer, "LSAs");

            int seqNum = seqRecord[myNodeId];
            map<int, int> validLinks;
            for (int j = 0; j < 256; j += 1) {
                if (graph[i][j] != -1) {
                    validLinks[j] = graph[i][j];
                }
            }

            nlohmann::json LSA = {
                {"sourceId", i},
                {"seq", seqNum},
                {"validLinks", validLinks} };
            std::string strLSA = LSA.dump();

            memcpy(packetBuffer + 4, strLSA.c_str(), strLSA.length());
            packetSize = strLSA.length() + 4;

            sendto(mySocketUDP, packetBuffer, packetSize, 0,
                (struct sockaddr*)&allNodeSocketAddrs[i], sizeof(allNodeSocketAddrs[i]));
        }
    }
}


void checkLostNeighbor()     //  check if there is any neighbor link is broken
{
    // cout << "Inside checkLostNeighbor function." << endl;

    struct timeval now;
    gettimeofday(&now, 0);

    for (int i = 0; i < 256; i += 1) {
        if (i != myNodeId && graph[myNodeId][i] != -1) {
            long timeDifference = (now.tv_sec - previousHeartbeat[i].tv_sec) * 1000000L + now.tv_usec - previousHeartbeat[i].tv_usec;
            if (timeDifference > 900000) { // saw befor in longer than 900 ms
                //graph_lock.lock();
                graph[myNodeId][i] = -1;
                graph[i][myNodeId] = -1;
                //graph_lock.unlock();

                isChanged = true;
            }
        }
    }
}

// send nodeId's latest LSA in my graph to my neighbors
void sendLSAToNeighbors() {
    char packetBuffer[BUFFERSIZE];
    int packetSize;
    memset(packetBuffer, 0, BUFFERSIZE);
    strcpy(packetBuffer, "LSAs");

    seqRecord[myNodeId] += 1;
    short int id = myNodeId;
    short int seqence = seqRecord[myNodeId];

    map<int, int> validLinks;
    for (int i = 0; i < 256; i += 1) {
        if (graph[myNodeId][i] != -1) {
            validLinks[i] = graph[myNodeId][i];
        }
    }

    json j_map(validLinks);
    string linkStr = j_map.dump();

    memcpy(packetBuffer + 4, &id, sizeof(short int));
    memcpy(packetBuffer + 6, &seqence, sizeof(int));
    memcpy(packetBuffer + 8, linkStr.c_str(), linkStr.length());
    packetSize = linkStr.length() + 8;

    for (int i = 0; i < 256; i += 1) {
        if (i != myNodeId && graph[myNodeId][i] != -1) {
            sendto(mySocketUDP, packetBuffer, packetSize, 0,
                (struct sockaddr*)&allNodeSocketAddrs[i], sizeof(allNodeSocketAddrs[i]));
        }
    }
}

void processLSAMessage(string buffContent, int bytesRecvd, int neighborId)
{
    const char* recvBuf = buffContent.c_str();
    std::string strLSA;
    strLSA.assign(recvBuf + 8, recvBuf + bytesRecvd);

    short int sourceId;
    short int receivedSeq;
    memcpy(&sourceId, recvBuf + 4, 2);
    memcpy(&receivedSeq, recvBuf + 6, 2);

    if (sourceId == myNodeId || receivedSeq <= seqRecord[sourceId]) {
        char buff[200];
        snprintf(buff, sizeof(buff), "Old LSA from neighbor %d, for sourceId %d, the received seq number is %d, and my recorded seq is %d.", neighborId, sourceId, receivedSeq, seqRecord[sourceId]);
        logMessageAndTime(buff);
        return;
    }

    seqRecord[sourceId] = receivedSeq;
    map<int, int> otherLinks = json::parse(strLSA);

    char buff[200];
    snprintf(buff, sizeof(buff), "New LSA from neighbor %d, for sourceId %d, the received seq number is %d, and my recorded seq is %d. The links are %s.", neighborId, sourceId, receivedSeq, seqRecord[sourceId], buffContent.c_str() + 8);
    logMessageAndTime(buff);

    graph_lock.lock();
    for (int destId = 0; destId < 256; destId += 1) {
        if (otherLinks.find(destId) != otherLinks.end()) {
            graph[sourceId][destId] = otherLinks[destId];
        }
        else {

            graph[sourceId][destId] = -1;
        }
    }
    graph_lock.unlock();

    for (int i = 0; i < 256; i += 1) {
        if (i != myNodeId && i != neighborId && graph[myNodeId][i] != -1) {
            sendto(mySocketUDP, recvBuf, bytesRecvd, 0,
                (struct sockaddr*)&allNodeSocketAddrs[i], sizeof(allNodeSocketAddrs[i]));
        }
    }
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


void directMessage(string buffContent, int bytesRecvd)
{
    const char* recvBuf = buffContent.c_str();

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
                char fowdMessage[bytesRecvd];
                strcpy(fowdMessage, "fowd");
                memcpy(fowdMessage, recvBuf + 4, bytesRecvd - 4);

                sendto(mySocketUDP, fowdMessage, bytesRecvd, 0,
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
    //return;
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
