// path vector, sending single LSA updates to neighbors periodically
// upon receiving LSA, process it but do not send it out immediately

#pragma once
#include "json.hpp"
#include <iostream>
#include <sstream>
#include <sys/time.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <climits>
#include <string>
#include <thread>
#include <mutex>
#include <stdio.h>
#include <netdb.h>
#include <map>
#include <queue> 
#include <unordered_set>
#include <unordered_map>
#include <string.h>
#include <atomic>
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

int BUFFERSIZE = 1000;
mutex m;

using namespace std;

int myNodeId, mySocketUDP;
int linkCost[256];
unordered_set<int> myNeighbors;
bool changed[256];  // has myPath[i] changed since last time 
map<int, map<int, int>> graph; // graph[i][j] indicates i->j link status, -1 means not connected and 1 means they are not connected
int nextHop[256];   // nexHop[i] means the nextHop to reach destination i, -1 means no reachable
int seq[256]; // seq[i] means the sequence number of previous LSA from node i 

struct timeval previousHeartbeat[256]; // track last time a neighbor is seen to figure out who is lost
struct sockaddr_in allNodeSocketAddrs[256];
FILE *flog;

void init(int inputId, string costFile, string logFile);
void readCostFile(const char *costFile);

void listenForNeighbors();

void *announceToNeighbors(void *unusedParam);
void processNeighborHeartbeat(int heardFrom);
void checkNewNeighbor(int heardFrom);
void checkLostNeighbor();
void handleBrokenLink(int neighborId);

void sharePathsToNewNeighbor(int newNeighborId);
// void sendPathToNeighbors(int destId);
string generateStrGraph();
void sendLSAsToNeighbors();

void processLSAMessage(string buffContent, int neighborId);
void processSingleLSA(int neighborId, int destId, int distance, int nextHop, set<int> nodesInPath);
int getNextHop(int destId);

void sendOrFowdMessage(string recvBuf, int bytesRecvd);
void directMessage(int destNodeId, string message, int messageByte);

void setupNodeSockets();
void logMessageAndTime(const char *message);
void logTime();
void logMyPaths();

void init(int inputId, string costFile, string logFile)
{
    myNodeId = inputId;

    // initailize linkCost and myPaths
    for (int i = 0; i < 256; i += 1)
    {
        previousHeartbeat[i].tv_sec = 0;
        previousHeartbeat[i].tv_usec = 0;
        linkCost[i] = 1;
        nextHop[i] = -1;
        seq[i] = 0;

        for (int j = 0; j < 256; j ++ 1) {
            graph[i][j] = -1;
        }
    }

    linkCost[myNodeId] = 0;
    graph[myNodeId][myNodeId] = 0;
    nextHop[myNodeId] = myNodeId;

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


void *announceToNeighbors(void *unusedParam)
{
    // cout << "Inside broadcastLSA function." << endl;

    struct timespec sleepFor;
    sleepFor.tv_sec = 0;
    sleepFor.tv_nsec = 300 * 1000 * 1000; // 300 ms

    while (1)
    {
        struct timespec sleepFor;
        sleepFor.tv_sec = 0;
        sleepFor.tv_nsec = 300 * 1000 * 1000; // 300 ms
        const char *heartBeats = "HEREIAM";
        while (1)
        {
            for (int i = 0; i < 256; i += 1)
            {
                // string logContent = "Sent haerbeats out. ";
                // logMessageAndTime(logContent.c_str());

                if (i != myNodeId)
                {
                    // string logContent = "Inside sendHearbeat, will send haerbeat to node ";
                    // logContent += to_string(i);
                    // logMessageAndTime(logContent.c_str());

                    sendto(mySocketUDP, heartBeats, 8, 0,
                        (struct sockaddr *)&allNodeSocketAddrs[i], sizeof(allNodeSocketAddrs[i]));
                }
            }
            nanosleep(&sleepFor, 0);
        }
    }
}

void listenForNeighbors()
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

        string content(recvBuf, recvBuf + bytesRecvd + 1);

        if (bytesRecvd > BUFFERSIZE)
        {
            string logContent = "Buffer size is not large enough!!!!";
            logMessageAndTime(logContent.c_str());
        }

        short int heardFrom = -1;
        heardFrom = atoi(strchr(strchr(strchr(fromAddr, '.') + 1, '.') + 1, '.') + 1);

        if (strstr(fromAddr, "10.1.1."))
        {
            string logContent = "Received message from neighbor node ";
            logContent += to_string(heardFrom);
            logMessageAndTime(logContent.c_str());

            processNeighborHeartbeat(heardFrom);
            checkLostNeighbor();
        }

        if (!strncmp(recvBuf, "send", 4) || !strncmp(recvBuf, "fowd", 4)) // send/forward message
        {
            sendOrFowdMessage(content, bytesRecvd);
        }
        else if (!strncmp(recvBuf, "LSAs", 4)) // LSA message
        {
            processLSAMessage(content, heardFrom);
        }
    }
    //(should never reach here)
    close(mySocketUDP);
}


void sendReceivedLSAToOtherNeighbors(string buffContent, int neighborId) {
    for (int destId = 0; destId < 256; destId += 1)
    {   
        bool isNeighbor = graph[myNodeId][destId] != -1;

        if (destId != neighborId && isNeighbor) {
            string path = generateStrGraph();
            sendto(mySocketUDP, buffContent.c_str(), buffContent.length(), 0,
                (struct sockaddr *)&allNodeSocketAddrs[destId], sizeof(allNodeSocketAddrs[destId]));
        }
    }

    string logContent = "Sent out my LSA graph. ";
    logMessageAndTime(logContent.c_str());
    // cout << logContent << endl;
}

void sendMyLSAToOtherNeighbors() {

    for (int destId = 0; destId < 256; destId += 1)
    {   
        bool isNeighbor = graph[myNodeId][destId] != -1;

        if (isNeighbor) {
            string path = generateStrGraph();
            sendto(mySocketUDP, path.c_str(), path.length(), 0,
                (struct sockaddr *)&allNodeSocketAddrs[destId], sizeof(allNodeSocketAddrs[destId]));
        }
    }

    string logContent = "Sent out my updated LSA graph. ";
    logMessageAndTime(logContent.c_str());
    // cout << logContent << endl;
}

void processNeighborHeartbeat(int heardFrom)
{
    // cout << "Inside processNeighborHeartbeat function." << endl;

    struct timeval now;
    gettimeofday(&now, 0);
    long previousSeenInSecond = previousHeartbeat[heardFrom].tv_sec;

    //  record that we heard from heardFrom just now.
    // m.lock();
    gettimeofday(&previousHeartbeat[heardFrom], 0);
    // m.unlock();

    if (previousSeenInSecond == 0)
    {
        string logContent = "  Saw a new neighbor ";
        logContent += to_string(heardFrom);
        logMessageAndTime(logContent.c_str());
        //  cout << logContent << endl;

        graph[i][heardFrom] = linkCost[heardFrom];

        // processSingleLSA(heardFrom, heardFrom, 0, heardFrom, unordered_set<int>{heardFrom});   this will be handled by the sharePathsToNeiNeighbor function
        // sharePathsToNewNeighbor(heardFrom);

        logContent = "  Finished processing seeing new neighbor ";
        logContent += to_string(heardFrom);
        logMessageAndTime(logContent.c_str());
        // cout << logContent << endl;
    }
    string logContent = "  Finished processNeighborHeartbeat.";
    logMessageAndTime(logContent.c_str());
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

                string logContent = "  Link broken to node ";
                logContent += to_string(i);

                logContent += ". The node was previously seen at ";
                logContent += to_string(previousHeartbeat[i].tv_sec);
                logContent += " s, and ";
                logContent += to_string(previousHeartbeat[i].tv_usec);
                logContent += " ms. \n";

                logContent += "  Now the time is ";
                logContent += to_string(now.tv_sec);
                logContent += " s, and ";
                logContent += to_string(now.tv_usec);
                logContent += " ms. \n";

                logContent += "  The time difference is  ";
                logContent += to_string(timeDifference);

                logMessageAndTime(logContent.c_str());

                // cout << logContent << endl;

                previousHeartbeat[i].tv_sec = 0;
                graph[myNodeId][i] = -1;
            }
        }
    }
    string logContent = "  Finished checkLostNeighbor.";
    logMessageAndTime(logContent.c_str());
}

string generateStrGraph() 
{
    // cout << "Inside generateStrGraph function." << endl;
    char payload[BUFFERSIZE];
    memset(payload, 0, BUFFERSIZE);
    strcpy(payload, "LSAs");
    seq[myNodeId] += 1;

    json LSA = {
        {"seq", seq[myNodeId]},
        {"graph", graph}};
    strLSA = LSA.dump();
  
    memcpy(payload + 4, strLSA.c_str(), strLSA.length());
    string payloadStr(payload, payload + 4 + strLSA.length() + 1);
    return payloadStr;
}


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
    string logContent = "    Entering processLSAMessage function.";
    logMessageAndTime(logContent.c_str());
    // cout << "Inside processLSAMessage function." << endl;
    string strLSA;
    strLSA.assign(buffContent.begin() + 4, buffContent.end() - 0);

    json LSA = json::parse(strLSA);
    int otherSeq = LSA["seq"];

    if (neighborId == myNodeId || otherSeq < seq[neighborId]) {
        return;
    } else {
        seq[neighborId] = otherSeq;
    }

    auto otherGraph = LSA["graph"];

    sendReceivedLSAToOtherNeighbors(string buffContent, int neighborId)

    logContent = "    Received and forwarded paths from neighbor node ";
    logContent += to_string(neighborId);
    logContent += strLSA;
    logMessageAndTime(logContent.c_str());
    // cout << logContent << endl;

    bool changed = false;
    for (int i = 0; i < 256; i += 1)
    {   
        for (int j = 0; j < 256; j += 1) {
            if (i != myNodeId && j != myNodeId) {
                graph[i][j] = otherGraph[i][j];
                changed = true;
            }
        }
    }

    if (changed) {
        sendMyLSAToOtherNeighbors();
    }

    logContent = "    Finished processing the neighbor ";
    logContent += to_string(neighborId);
    logContent += " 's LSA ";
    logMessageAndTime(logContent.c_str());
    // cout << logContent << endl;
}


/*
void processSingleLSA(int neighborId, int destId, int distance, int nextHop, set<int> nodesInPath)
{
    // cout << "Inside processSingleLSA function." << endl;
    bool containsMyNodeId = nodesInPath.count(myNodeId) == 1; // if neighbor LSA path has myNodeId in the path
    int myDistance = get<0>(myPaths[destId]);
    int myNexthop = get<1>(myPaths[destId]);

    if ((destId == myNodeId) || containsMyNodeId)
    {   
        return;
    }

    if (distance != -1 && myDistance == -1)
    { // I do not have path to dest, but my neighbor has one. Use this neighbor path to destId

        get<0>(myPaths[destId]) = linkCost[neighborId] + distance;
        get<1>(myPaths[destId]) = neighborId;
        get<2>(myPaths[destId]) = get<2>(neighborPath);
        get<2>(myPaths[destId]).insert(myNodeId);

        m.lock();
        changed[destId] = true;
        m.unlock();
    }
    else if (distance != -1 && myDistance != -1)
    { // I have a path to dest, and my neighbor has one.
      // compare two paths, if neighbor path is better then update it in myPathToNode[destId]
        int newDistance = distance + linkCost[neighborId];
        if ((myDistance > newDistance) || (myDistance == newDistance && neighborId < myNextHop))
        {

            get<0>(myPaths[destId]) = linkCost[neighborId] + distance;
            get<1>(myPaths[destId]) = neighborId;
            get<2>(myPaths[destId]) = get<2>(neighborPath);
            get<2>(myPaths[destId]).insert(myNodeId);

            m.lock();
            changed[destId] = true;
            m.unlock();
        } 
    }
    else if (distance == -1 && myDistance != -1) // neighbor lost a path to destId
    {
        if (get<2>(myPaths[destId]).count(neighborId) == 1)
        {
            // The neighbor is in my path to destId, then I lost the path to destId
            get<0>(myPaths[destId]) = -1;
            get<2>(myPaths[destId]).clear();

            m.lock();
            changed[destId] = true;
            m.unlock();
        } 
    }
}
*/

// run Dijkstra's algorithm to find nextHop to reach destId
int getNextHop(int destId) {
    int prev[256];      // i's previous node in path to desId
    int distance[256];  // i's total distance to desId
    prev[destId] = -1;
    bool visited[256];
    visited[myNodeId] = true;

    class ComparisonClass {
    public:
        bool operator() (Node1, Node2) {
            return distance[Node1] > distance[Node2] || (distance[Node1] == distance[Node2] && Node1 > Node2)
        }
    };

    priority_queue<int, std::vector<int>, ComparisonClass> frontier;
    frontier.push(myNodeId);

    while (!frontier.empty()) {
        int size = frontier.size();

        for (int i = 0; i < size; i += 1) {
            int ele = frontier.pop();
            if (ele == destId) {
                break;
            }
            for (int j = 0; j < 256; j += 1) {
                if (graph[ele][j] != -1 && !visited[j]) {
                    distance[j] = link[graph[ele][j]];
                    prev[j] = ele;
                    frontier.push(j);
                }
            }
        }
    }

    if (prev[destId] = -1) {
        return -1;
    }

    int p = int destId;
    while (prev[p] != myNodeId) {
        p = prev[p];
    }

    return p;
}

/*
void sharePathsToNewNeighbor(int newNeighborId) // this will send my self path to the new neighbor
{
    for (int destId = 0; destId < 256; destId += 1)
    {
        if (destId != newNeighborId && get<0>(myPaths[destId]) != -1 && get<2>(myPaths[destId]).count(newNeighborId) == 0)
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

/*
void sendPathToNeighbors(int neighborId) 
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

/*
void handleBrokenLink(int neighborId)
{
    for (int destId = 0; destId < 256; destId += 1)
    {
        if (destId != myNodeId && destId != neighborId && get<0>(myPaths[destId]) != -1 && get<1>(myPaths[destId]) == neighborId)
        { // I lost the path to destId due to broken link to the neighbor

            get<0>(myPaths[destId]) = -1;
            get<2>(myPaths[destId]).clear();

            m.lock();
            changed[destId] = true;
            m.unlock();
        }
    }
}
*/

void logMessageAndTime(const char *message)
{
    // return;

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
void logMyPaths()
{
    char logLine[BUFFERSIZE];
    json j_map(myPaths);
    string path = j_map.dump();
    sprintf(logLine, "%s\n", path.c_str());
    fwrite(logLine, 1, strlen(logLine), flog);
    fflush(flog);
}
*/

void directMessage(int destNodeId, string message, int messageByte)
{
    char logLine[BUFFERSIZE];

    if (myNodeId == destNodeId)
    {
        sprintf(logLine, "receive packet message %s\n", message.c_str() + 6);
    }
    else
    {
        int nexthop = getNextHop(destNodeId);
        if (nexthop != -1)
        {
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
