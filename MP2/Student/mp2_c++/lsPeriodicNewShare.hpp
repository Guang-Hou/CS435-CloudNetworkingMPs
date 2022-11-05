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

#define DEBUG 1

#define DEBUG_PRINTF(fmt, ...)                                                  \
    do                                                                          \
    {                                                                           \
        if (DEBUG)                                                              \
            fprintf(stderr, "node%d:%s:%d:%s(): " fmt "\n", myNodeId, __FILE__, \
                    __LINE__, __func__, __VA_ARGS__);                           \
    } while (0)

#define DEBUG_PRINT(text)                                                        \
    do                                                                           \
    {                                                                            \
        if (DEBUG)                                                               \
            fprintf(stderr, "node%d:%s:%d:%s(): " text "\n", myNodeId, __FILE__, \
                    __LINE__, __func__);                                         \
    } while (0)

using json = nlohmann::json;
using namespace std;

typedef std::pair<int, int> DN; // Used for Dijkstra's algorithm, total distance to source node and this node's id

int BUFFERSIZE = 4000;
int myNodeId, mySocketUDP;
int linkCost[256];
map<int, map<int, int>> graph; // graph[i][j] indicates i->j link cost, -1 means not connected
map<int, int> seq;        // seq[i] means the sequence number of previous LSA from node i
std::mutex change_lock;
std::mutex graph_lock;
std::mutex seq_lock;
bool isChanged; // my link status has changed since last time send out LSA
// int nextHop[256];            // nexHop[i] means the nextHop to reach destination i, -1 means no reachable

struct timeval previousHeartbeat[256]; // track last time a neighbor is seen to figure out who is lost
struct sockaddr_in allNodeSocketAddrs[256];
FILE* flog;

void init(int inputId, string costFile, string logFile);
void readCostFile(const char* costFile);

void* announceHeartbeat(void* unusedParam);
void* announceLSA(void* unusedParam);

void listenForNeighbors();
void checkNewNeighbor(int heardFrom);
void checkLostNeighbor();

void sendMyLSAToNeighbors();
void processLSAMessage(string buffContent, int bytesRecvd, int heardFrom);
void sendReceivedLSAToOtherNeighbors(string buffContent, int bytesRecvd, int neighborId);
void directMessage(string buffContent, int bytesRecvd);
int getNextHop(int destId);

void syncSeq(int newNeighborId);
void processSeqShare(string buffContent, int bytesRecvd, int newNeighborId);
void processSeqShareWrapper(string buffContent, int bytesRecvd, int newNeighborId);

void setupNodeSockets();
void logMessageAndTime(const char* message);
void logTime();

// void testDij(int MyNodeId, int destId);

void init(int inputId, string costFile, string logFile)
{
    myNodeId = inputId;

    // initailize linkCost and myPaths
    for (int i = 0; i < 256; i += 1)
    {
        previousHeartbeat[i].tv_sec = 0;
        previousHeartbeat[i].tv_usec = 0;
        linkCost[i] = 1;
        // nextHop[i] = -1;
        seq[i] = 0;

        for (int j = 0; j < 256; j += 1)
        {
            graph[i][j] = -1;
        }
    }

    linkCost[myNodeId] = 0;
    graph[myNodeId][myNodeId] = 0;
    // nextHop[myNodeId] = myNodeId;

    //std::cout << "Inside init, right before setup socket." << std::endl;

    setupNodeSockets();
    readCostFile(costFile.c_str());

    flog = fopen(logFile.c_str(), "a");

    // string logContent = "Initialization.";
    // logMessageAndTime(logContent.c_str());

    // cout << logContent << endl;
}

void readCostFile(const char* costFile)
{
    FILE* fcost = fopen(costFile, "r");

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

    //cout << "Finished reading cost file" << endl;
}

void* announceHeartbeat(void* unusedParam)
{
    // cout << "Inside announceHeartbeat function." << endl;

    struct timespec sleepFor;
    sleepFor.tv_sec = 0;
    sleepFor.tv_nsec = 300 * 1000 * 1000; // 300 ms
    const char* heartBeats = "H";

    while (1)
    {
        for (int i = 0; i < 256; i += 1)
        {
            if (i != myNodeId)
            {
                sendto(mySocketUDP, heartBeats, 2, 0,
                    (struct sockaddr*)&allNodeSocketAddrs[i], sizeof(allNodeSocketAddrs[i]));
            }
        }
        logMessageAndTime("Sent Heartbeat.");
        nanosleep(&sleepFor, 0);
    }
}

void* announceLSA(void* unusedParam)
{
    struct timespec sleepFor;
    sleepFor.tv_sec = 0;
    sleepFor.tv_nsec = 500 * 1000 * 1000; // 500 ms

    while (1)
    {
        change_lock.lock();
        bool hasChanged = isChanged;
        change_lock.unlock();

        if (hasChanged)
        {
            sendMyLSAToNeighbors();

            change_lock.lock();
            isChanged = false;
            change_lock.unlock();

            nanosleep(&sleepFor, 0);
        }
    }
}

void listenForNeighbors()
{
    // cout << "Inside listenForNeighbors function." << endl;

    char fromAddr[100];
    struct sockaddr_in theirAddr;
    socklen_t theirAddrLen = sizeof(theirAddr);
    char recvBuf[BUFFERSIZE];
    int bytesRecvd;

    while (1)
    {
        memset(recvBuf, 0, sizeof(recvBuf));
        if ((bytesRecvd = recvfrom(mySocketUDP, recvBuf, BUFFERSIZE, 0,
            (struct sockaddr*)&theirAddr, &theirAddrLen)) == -1)
        {
            perror("connectivity listener: recvfrom failed");
            exit(1);
        }
        inet_ntop(AF_INET, &theirAddr.sin_addr, fromAddr, 100);

        //std::string logContent = "Received recvBuf content:  ";
        std::string buffContent;
        buffContent.assign(recvBuf, recvBuf + bytesRecvd);
        //logContent += content;
        //logMessageAndTime(logContent.c_str());

        short int heardFrom = -1;
        heardFrom = atoi(strchr(strchr(strchr(fromAddr, '.') + 1, '.') + 1, '.') + 1);

        if (strstr(fromAddr, "10.1.1."))
        {
            // cout << logContent << endl;

            checkNewNeighbor(heardFrom);
            checkLostNeighbor();
        }

        if (!strncmp(recvBuf, "send", 4) || !strncmp(recvBuf, "fowd", 4)) // send/forward message
        {
            //string logContent = "Got send or fowd message.";
            //logMessageAndTime(logContent.c_str());
            directMessage(buffContent, bytesRecvd);
        }
        else if (!strncmp(recvBuf, "LSAs", 4)) // LSA message
        {
            processLSAMessage(buffContent, bytesRecvd, heardFrom);
        }
        else if (!strncmp(recvBuf, "seqs", 4)) // new neighbor share path message
        {
            processSeqShareWrapper(buffContent, bytesRecvd, heardFrom);
        }
        else {
            string logContent = "Received heartbeat from neighbor node ";
            logContent += to_string(heardFrom);
            logMessageAndTime(logContent.c_str());
        }
    }
    close(mySocketUDP);
}

void checkNewNeighbor(int heardFrom)
{
    // cout << "Inside checkNewNeighbor function." << endl;

    struct timeval now;
    gettimeofday(&now, 0);

    long timeDifference = (now.tv_sec - previousHeartbeat[heardFrom].tv_sec) * 1000000L + now.tv_usec - previousHeartbeat[heardFrom].tv_usec;

    //  record that we heard from heardFrom just now.
    gettimeofday(&previousHeartbeat[heardFrom], 0);

    if (timeDifference > 800000) // larger than 800 ms
    {
        std::string logContent = "  Saw a new neighbor ";
        logContent += std::to_string(heardFrom);
        logMessageAndTime(logContent.c_str());
        //  cout << logContent << endl;

        graph_lock.lock();
        graph[myNodeId][heardFrom] = linkCost[heardFrom];
        graph_lock.unlock();

        change_lock.lock();
        isChanged = true;
        change_lock.unlock();

        syncSeq(heardFrom);
    }
}

void checkLostNeighbor()
{
    // cout << "Inside checkLostNeighbor function." << endl;

    //  check if there is any neighbor link is broken, if so update pathRecords and broadcast LSA
    for (int i = 0; i < 256; i += 1)
    {
        struct timeval now;
        gettimeofday(&now, 0);

        if (i != myNodeId)
        {
            long timeDifference = (now.tv_sec - previousHeartbeat[i].tv_sec) * 1000000L + now.tv_usec - previousHeartbeat[i].tv_usec;
            if (previousHeartbeat[i].tv_sec != 0 && timeDifference > 800000) // larger than 800 ms
            {
                char buff[200];
                snprintf(buff, sizeof(buff),
                    "  Link broken to node %d. The node was previously seen at %ld s, and %ld us.\n  Now the time is %ld s, and %ld us. \n The time difference is %ld.",
                    i, previousHeartbeat[i].tv_sec, previousHeartbeat[i].tv_usec, now.tv_sec, now.tv_usec, timeDifference);
                logMessageAndTime(buff);

                graph_lock.lock();
                graph[myNodeId][i] = -1;
                graph_lock.unlock();

                change_lock.lock();
                isChanged = true;
                change_lock.unlock();
            }
        }
    }
}

/*
void createLSA(int sourceId, int row, long ttl) //
{
    char payload[BUFFERSIZE];
    memset(payload, 0, BUFFERSIZE);
    strcpy(payload, "LSAs");

    json LSA = {
        {"sourceId", myNodeId},
        {"seq", seq[row]},
        {"links", graph[myNodeId]},
        {"ttl", ttl}};  // 500 ms
    string strLSA = LSA.dump();

    memcpy(payload + 4, strLSA.c_str(), strLSA.length());

    return payload;
}
*/

void sendMyLSAToNeighbors()  // it is inside a thread of announceLSA
{
    // cout << "Inside sendMyLSAToNeighbors function." << endl;

    struct timeval now;
    gettimeofday(&now, 0);

    graph_lock.lock();
    map<int, int> links = graph[myNodeId];
    graph_lock.unlock();

    seq_lock.lock();
    seq[myNodeId] += 1;
    int mySeq = seq[myNodeId];
    seq_lock.unlock();

    for (int destId = 0; destId < 256; destId += 1)
    {
        bool isNeighbor = links[destId] != -1;

        if (destId != myNodeId && isNeighbor)
        {
            char payload[BUFFERSIZE];
            memset(payload, 0, BUFFERSIZE);
            strcpy(payload, "LSAs");

            nlohmann::json LSA = {
                {"sourceId", myNodeId},
                {"seq", mySeq},
                {"links", links},
                {"ttl", (now.tv_sec * 1000000L + now.tv_usec + 500000) / 1000} }; // 500 ms

            std::string strLSA = LSA.dump();

            memcpy(payload + 4, strLSA.c_str(), strLSA.length());

            // cout << "LSA string length " << strLSA.length() << endl;
            sendto(mySocketUDP, payload, strLSA.length() + 4, 0,
                (struct sockaddr*)&allNodeSocketAddrs[destId], sizeof(allNodeSocketAddrs[destId]));
        }
    }

    string logContent = "My link status changed, and the wait time passed, sent out my updated LSA links. ";
    logMessageAndTime(logContent.c_str());
    //  cout << logContent << endl;
}

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

    string logContent = "Received LSA of size ";
    logContent += to_string(bytesRecvd);
    logMessageAndTime(logContent.c_str());

    //cout << logContent << endl;
    nlohmann::json LSA = nlohmann::json::parse(strLSA);
    int sourceId = LSA["sourceId"];
    int receivedSeq = LSA["seq"];
    long ttl = LSA["ttl"];
    long timestamp = (now.tv_sec * 1000000L + now.tv_usec) / 1000;  // unit in ms

    seq_lock.lock();
    int sourceSeqInRecord = seq[sourceId];
    seq_lock.unlock();

    if (receivedSeq <= sourceSeqInRecord || timestamp > ttl)
    {
        char buff[200];
        snprintf(buff, sizeof(buff), "    Old LSA from node %d, ttl is %ld, the received seq number is %d, and my recorded seq is %d.", sourceId, ttl, receivedSeq, seq[sourceId]);
        logMessageAndTime(buff);
        return;
    }
    else
    {
        seq_lock.lock();
        seq[sourceId] = receivedSeq;
        seq_lock.unlock();

        map<int, int> otherLinks = LSA["links"];

        graph_lock.lock();
        for (int destId = 0; destId < 256; destId += 1)
        {
            graph[sourceId][destId] = otherLinks[destId];
        }
        graph_lock.unlock();

        char buff[200];
        snprintf(buff, sizeof(buff), "     New LSA from node %d, with seq number %d.", sourceId, receivedSeq);
        logMessageAndTime(buff);
        logMessageAndTime(strLSA.c_str());

        thread reSend_thread(sendReceivedLSAToOtherNeighbors, buffContent, bytesRecvd, neighborId);
        reSend_thread.detach();
    }
    // cout << logContent << endl;
}

void sendReceivedLSAToOtherNeighbors(string buffContent, int bytesRecvd, int neighborId)
{
    // cout << "Inside sendReceivedLSAToOtherNeighbors function." << endl;
    graph_lock.lock();
    map<int, int> links = graph[myNodeId];
    graph_lock.unlock();

    for (int destId = 0; destId < 256; destId += 1)
    {
        bool isNeighbor = links[destId] != -1;

        if (destId != neighborId && destId != myNodeId && isNeighbor)
        {
            sendto(mySocketUDP, buffContent.c_str(), bytesRecvd, 0,
                (struct sockaddr*)&allNodeSocketAddrs[destId], sizeof(allNodeSocketAddrs[destId]));
        }
    }

    string logContent = "Resent received LSA from node(not sourceNode) ";
    logContent += to_string(neighborId);
    logMessageAndTime(logContent.c_str());
    // cout << logContent << endl;
}

void syncSeq(int newNeighborId) // share my seq records to new neighbor
{
    char payload[BUFFERSIZE];
    memset(payload, 0, BUFFERSIZE);

    strcpy(payload, "seqs");

    seq_lock.lock();
    nlohmann::json LSA = {
        {"seqRecord", seq},
    };
    seq_lock.unlock();

    std::string strLSA = LSA.dump();

    // cout << "strLSA length " << strLSA.length() << endl;
    memcpy(payload + 4, strLSA.c_str(), strLSA.length());

    sendto(mySocketUDP, payload, strLSA.length() + 4, 0,
        (struct sockaddr*)&allNodeSocketAddrs[newNeighborId], sizeof(allNodeSocketAddrs[newNeighborId]));
}

void processSeqShareWrapper(string buffContent, int bytesRecvd, int newNeighborId) // when receiving header "seqs"
{
    thread share_thread(processSeqShare, buffContent, bytesRecvd, newNeighborId);
    share_thread.detach();
}

void processSeqShare(string buffContent, int bytesRecvd, int newNeighborId) // when receiving header "seqs"
{
    // cout << "Inside sharePathsToNewNeighbor function." << endl;
    // string logContent = "Inside processSeqShare function. ";
    // logContent += to_string(newNeighborId);
    // logMessageAndTime(logContent.c_str());

    std::string strSeq;
    strSeq.assign(buffContent.c_str() + 4, buffContent.c_str() + bytesRecvd);

    string logContent = "Received new neighbor seq list, the file size is ";
    logContent += to_string(bytesRecvd);
    logMessageAndTime(logContent.c_str());

    //cout << logContent << endl;
    nlohmann::json LSA = nlohmann::json::parse(strSeq);
    map<int, int> otherSeq = LSA["seqRecord"];

    graph_lock.lock();
    map<int, map<int, int>> graphCopy = graph;
    graph_lock.unlock();

    seq_lock.lock();
    map<int, int> seqCopy = seq;
    seq_lock.unlock();

    char payload[BUFFERSIZE];
    for (int i = 0; i < 256; i += 1)
    {
        if (i != newNeighborId && i != newNeighborId && seq[i] > 0 && seq[i] > otherSeq[i])
        {
            struct timeval now;
            gettimeofday(&now, 0);

            memset(payload, 0, BUFFERSIZE);
            strcpy(payload, "LSAs");

            nlohmann::json LSA = {
                {"sourceId", i},
                {"seq", seqCopy[i]},
                {"links", graphCopy[i]},
                {"ttl", (now.tv_sec * 1000000L + now.tv_usec + 500000) / 1000} }; // 500 ms

            std::string strLSA = LSA.dump();

            memcpy(payload + 4, strLSA.c_str(), strLSA.length());

            sendto(mySocketUDP, payload, strLSA.length() + 5, 0,
                (struct sockaddr*)&allNodeSocketAddrs[newNeighborId], sizeof(allNodeSocketAddrs[newNeighborId]));
        }
    }

    //std::string 
    logContent = "Shared all my newer pahts to new neighbor ";
    logContent += std::to_string(newNeighborId);
    logContent += " : ";
    //  logContent += string(strLSA);
    logMessageAndTime(logContent.c_str());
    //  cout << logContent << endl;
}

// run Dijkstra's algorithm to find nextHop to reach destId
int getNextHop(int destId)
{
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

    graph_lock.lock();
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
    graph_lock.unlock();

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

void setupNodeSockets()
{
    //std::cout << "Inside setup socket function." << std::endl;
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
    if (bind(mySocketUDP, (struct sockaddr*)&bindAddr, sizeof(struct sockaddr_in)) < 0)
    {
        perror("bind error");
        close(mySocketUDP);
        exit(2);
    }
}

void logMessageAndTime(const char* message)
{
    //return;
    char logLine[BUFFERSIZE];
    sprintf(logLine, " %s\n", message);
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
