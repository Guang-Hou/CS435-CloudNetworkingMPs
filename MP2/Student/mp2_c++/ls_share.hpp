/* Link State Routing
 // Once my link changes, send out LSA to neighbors.
 - Periodically share LSA if my link status changes.
 - Upon receiving LSA,
    - immediately forward it out to my neighbors
    - use it to update my graph database but do not send my LSA out.
 - For new neighbor, share all my links to it. To begin, turn off this function.
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
using namespace std;

typedef pair<int, int> DN; // total distance to source node and this node's id

int BUFFERSIZE = 3000;
// mutex m;
int myNodeId, mySocketUDP;
int linkCost[256];
// unordered_set<int> myNeighbors;
bool isChanged;                // my link status has changed since last time send out LSA
map<int, map<int, int>> graph; // graph[i][j] indicates i->j link status, -1 means not connected and 1 means they are not connected
int nextHop[256];              // nexHop[i] means the nextHop to reach destination i, -1 means no reachable
int seq[256];                  // seq[i] means the sequence number of previous LSA from node i

struct timeval previousHeartbeat[256]; // track last time a neighbor is seen to figure out who is lost
struct sockaddr_in allNodeSocketAddrs[256];
FILE *flog;

void init(int inputId, string costFile, string logFile);
void readCostFile(const char *costFile);

void listenForNeighbors();

void *announceToNeighbors(void *unusedParam);
void *announceLSA(void *unusedParam);
void checkNewNeighbor(int heardFrom);
void checkLostNeighbor();
// void handleBrokenLink(int neighborId);

// void sendPathToNeighbors(int destId);
// string generateStrGraph();
string generateStrLSA();
void shareSeq(int newNeighborId);
void sendReceivedLSAToOtherNeighbors(char *recvBuf, int bytesRecvd, int neighborId);
void sendMyLSAToNeighbors();

void processLSAMessage(char *recvBuf, int bytesRecvd, int heardFrom);
// void processSingleLSA(int neighborId, int destId, int distance, int nextHop, set<int> nodesInPath);
void processLinkShare(string content, int heardFrom);
void processSeqShare(string content, int heardFrom);

void sendOrFowdMessage(char *recvBuf, int bytesRecvd);
void directMessage(char *recvBuf, int bytesRecvd);
int getNextHop(int destId);

void setupNodeSockets();
void logMessageAndTime(const char *message);
void logTime();
void logMyPaths();

void testDij(int MyNodeId, int destId);

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

    setupNodeSockets();
    readCostFile(costFile.c_str());

    flog = fopen(logFile.c_str(), "a");

    // string logContent = "Initialization.";
    // logMessageAndTime(logContent.c_str());

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
    // cout << "Inside announceToNeighbors function." << endl;

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
                                   (struct sockaddr *)&theirAddr, &theirAddrLen)) == -1)
        {
            perror("connectivity listener: recvfrom failed");
            exit(1);
        }

        inet_ntop(AF_INET, &theirAddr.sin_addr, fromAddr, 100);

        if (bytesRecvd > BUFFERSIZE)
        {
            string logContent = "Buffer size is not large enough!!!!";
            logMessageAndTime(logContent.c_str());
        }

        short int heardFrom = -1;
        heardFrom = atoi(strchr(strchr(strchr(fromAddr, '.') + 1, '.') + 1, '.') + 1);

        string content(recvBuf, recvBuf + bytesRecvd + 1);

        if (strstr(fromAddr, "10.1.1."))
        {
            // string logContent = "Received message(hello or LSA or send) from neighbor node ";
            // logContent += to_string(heardFrom);
            // logMessageAndTime(logContent.c_str());
            // cout << logContent << endl;

            checkNewNeighbor(heardFrom);
            checkLostNeighbor();
        }

        if (!strncmp(recvBuf, "send", 4) || !strncmp(recvBuf, "fowd", 4)) // send/forward message
        {
            directMessage(recvBuf, bytesRecvd);
        }
        else if (!strncmp(recvBuf, "LSAs", 4)) // LSA message
        {
            processLSAMessage(recvBuf, bytesRecvd, heardFrom);
        }
        else if (!strncmp(recvBuf, "seqs", 4)) // new neighbor share path message
        {
            processSeqShare(content, heardFrom);
        }
        else if (!strncmp(recvBuf, "shar", 4)) // new neighbor share path message
        {
            processLinkShare(content, heardFrom);
        }
    }
    //(should never reach here)
    close(mySocketUDP);
}

void checkNewNeighbor(int heardFrom)
{
    // cout << "Inside checkNewNeighbor function." << endl;

    struct timeval now;
    gettimeofday(&now, 0);
    long previousSeenInSecond = previousHeartbeat[heardFrom].tv_sec;

    //  record that we heard from heardFrom just now.
    // m.lock();
    gettimeofday(&previousHeartbeat[heardFrom], 0);
    // m.unlock();

    if (previousSeenInSecond == 0)
    {
        // string logContent = "  Saw a new neighbor ";
        // logContent += to_string(heardFrom);
        // logMessageAndTime(logContent.c_str());
        //  cout << logContent << endl;

        graph[myNodeId][heardFrom] = linkCost[heardFrom];
        isChanged = true;
        // shareSeq(heardFrom);
        //  sendMyLSAToNeighbors();

        // logContent = "  Finished processing seeing new neighbor ";
        // logContent += to_string(heardFrom);
        //  logMessageAndTime(logContent.c_str());
        //  cout << logContent << endl;
    }
    // string logContent = "  Finished checkNewNeighbor.";
    //  logMessageAndTime(logContent.c_str());
}

void checkLostNeighbor()
{
    // cout << "Inside checkLostNeighbor function." << endl;
    //

    // string logContent = "Inside checkLostNeighbor function.";
    //  logMessageAndTime(logContent.c_str());
    //  check if there is any neighbor link is broken, if so update pathRecords and broadcast LSA
    for (int i = 0; i < 256; i += 1)
    {
        struct timeval now;
        gettimeofday(&now, 0);

        if (i != myNodeId)
        {
            long timeDifference = (now.tv_sec - previousHeartbeat[i].tv_sec) * 1000000L + now.tv_usec - previousHeartbeat[i].tv_usec;
            if (previousHeartbeat[i].tv_sec != 0 && timeDifference > 800000) // missed two hearbeats
            {
                char buff[200];
                snprintf(buff, sizeof(buff),
                         "  Link broken to node %d. The node was previously seen at %ld s, and %ld us.\n  Now the time is %ld s, and %ld us. \n The time difference is %ld.",
                         i, previousHeartbeat[i].tv_sec, previousHeartbeat[i].tv_usec, now.tv_sec, now.tv_usec, timeDifference);
                logMessageAndTime(buff);

                previousHeartbeat[i].tv_sec = 0;
                graph[myNodeId][i] = -1;
                isChanged = false;
                // sendMyLSAToNeighbors();
            }
        }
    }
    // logContent = "  Finished checkLostNeighbor.";
    //  logMessageAndTime(logContent.c_str());
    //  cout << logContent << endl;
}

void processLSAMessage(char *recvBuff, int bytesRecvd, int neighborId)
{
    // cout << "Inside processLSAMessage function." << endl;
    // string logContent = "    Entering processLSAMessage function, received LSA.";
    // logMessageAndTime(logContent.c_str());
    // cout << logContent << endl;

    string strLSA;
    strLSA.assign(recvBuff + 4, recvBuff + bytesRecvd);

    json LSA = json::parse(strLSA);
    int sourceId = LSA["sourceId"];
    int receivedSeq = LSA["seq"];

    if (receivedSeq <= seq[sourceId])
    {
        char buff[200];
        snprintf(buff, sizeof(buff), "    Old LSA from node %d, the received seq number is %d, and my recorded seq is %d.", sourceId, receivedSeq, seq[sourceId]);
        logMessageAndTime(buff);
        return;
    }
    else
    {
        seq[sourceId] = receivedSeq;
        map<int, int> otherLinks = LSA["links"];

        for (int destId = 0; destId < 256; destId += 1)
        {
            graph[sourceId][destId] = otherLinks[destId];
        }

        char buff[200];
        snprintf(buff, sizeof(buff), "     New LSA from node %d, with seq number %d.", sourceId, receivedSeq);
        logMessageAndTime(buff);
        logMessageAndTime(strLSA.c_str());

        sendReceivedLSAToOtherNeighbors(recvBuff, bytesRecvd, neighborId);
    }
    // cout << logContent << endl;
}

void sendReceivedLSAToOtherNeighbors(char *recvBuff, int bytesRecvd, int neighborId)
{
    // cout << "Inside sendReceivedLSAToOtherNeighbors function." << endl;
    for (int destId = 0; destId < 256; destId += 1)
    {
        bool isNeighbor = graph[myNodeId][destId] != -1;

        if (destId != neighborId && isNeighbor)
        {
            sendto(mySocketUDP, recvBuff, bytesRecvd, 0,
                   (struct sockaddr *)&allNodeSocketAddrs[destId], sizeof(allNodeSocketAddrs[destId]));
        }
    }

    // string logContent = "Resent received LSA from node ";
    // logContent += to_string(neighborId);
    // logMessageAndTime(logContent.c_str());
    // cout << logContent << endl;
}

void *announceLSA(void *unusedParam)
{
    struct timespec sleepFor;
    sleepFor.tv_sec = 0;
    sleepFor.tv_nsec = 500 * 1000 * 1000; // 500 ms

    while (1)
    {
        sendMyLSAToNeighbors();
        nanosleep(&sleepFor, 0);
    }
}

void sendMyLSAToNeighbors()
{
    // cout << "Inside sendMyLSAToNeighbors function." << endl;
    if (isChanged)
    {
        for (int destId = 0; destId < 256; destId += 1)
        {
            bool isNeighbor = graph[myNodeId][destId] != -1;

            if (isNeighbor)
            {
                char payload[BUFFERSIZE];
                memset(payload, 0, BUFFERSIZE);
                strcpy(payload, "LSAs");
                seq[myNodeId] += 1;

                json LSA = {
                    {"sourceId", myNodeId},
                    {"seq", seq[myNodeId]},
                    {"links", graph[myNodeId]}};
                string strLSA = LSA.dump();

                memcpy(payload + 4, strLSA.c_str(), strLSA.length());

                // cout << "LSA string length " << strLSA.length() << endl;
                sendto(mySocketUDP, payload, strLSA.length() + 5, 0,
                       (struct sockaddr *)&allNodeSocketAddrs[destId], sizeof(allNodeSocketAddrs[destId]));
            }
        }
    }

    isChanged = false;

    // string logContent = "Sent out my updated LSA links. ";
    // logMessageAndTime(logContent.c_str());
    //  cout << logContent << endl;
}

void shareSeq(int newNeighborId) // share my seq records to new neighbor
{
    char payload[BUFFERSIZE];

    memset(payload, 0, BUFFERSIZE);
    strcpy(payload, "seqs");

    json LSA = {
        {"seqRecord", seq},
    };
    string strLSA = LSA.dump();

    // cout << "strLSA length " << strLSA.length() << endl;
    memcpy(payload + 4, strLSA.c_str(), strLSA.length());

    sendto(mySocketUDP, payload, strLSA.length() + 5, 0,
           (struct sockaddr *)&allNodeSocketAddrs[newNeighborId], sizeof(allNodeSocketAddrs[newNeighborId]));
}

void processSeqShare(string buffContent, int newNeighborId) // when receiving header "seqs"
{
    // cout << "Inside sharePathsToNewNeighbor function." << endl;
    // string logContent = "Inside processSeqShare function. ";
    // logContent += to_string(newNeighborId);
    // logMessageAndTime(logContent.c_str());

    string strLSA;
    strLSA.assign(buffContent.begin() + 4, buffContent.end() - 0);

    json LSA = json::parse(strLSA);
    // int otherSeq[256];
    auto otherSeq = LSA["seqRecord"];

    char payload[BUFFERSIZE];

    for (int i = 0; i < 256; i += 1)
    {
        if (i != newNeighborId && seq[i] > 0 && seq[i] > otherSeq[i])
        {
            memset(payload, 0, BUFFERSIZE);
            strcpy(payload, "shar");

            json LSA = {
                {"row", i},
                {"links", graph[i]},
            };
            string strLSA = LSA.dump();

            // cout << "strLSA length " << strLSA.length() << endl;
            memcpy(payload + 4, strLSA.c_str(), strLSA.length());

            sendto(mySocketUDP, payload, strLSA.length() + 5, 0,
                   (struct sockaddr *)&allNodeSocketAddrs[newNeighborId], sizeof(allNodeSocketAddrs[newNeighborId]));
        }
    }

    // logContent = "Shared all my newer pahts to new neighbor ";
    // logContent += to_string(newNeighborId);
    //  logContent += " : ";
    //  logContent += string(strLSA);
    // logMessageAndTime(logContent.c_str());
    //  cout << logContent << endl;
}

void processLinkShare(string buffContent, int neighborId)
{
    string logContent = "    Entering processLinkShare function.";
    // logMessageAndTime(logContent.c_str());
    //   cout << "Inside hanldling Share Message function." << endl;
    //   cout << logContent << endl;

    string strLSA;
    strLSA.assign(buffContent.begin() + 4, buffContent.end() - 0);

    json LSA = json::parse(strLSA);
    int row = LSA["row"];
    map<int, int> otherLinks = LSA["links"];

    for (int j = 0; j < 256; j += 1)
    {
        if (j != myNodeId)
        {
            graph[row][j] = otherLinks[j];
        }
    }

    logContent = "    Finished processing the neighbor ";
    logContent += to_string(neighborId);
    logContent += " 's shared graph ";
    // logMessageAndTime(logContent.c_str());
    //   cout << logContent << endl;
}

/*
string generateStrLSA()
{
    cout << "Inside generateStrLSA function." << endl;
    char payload[BUFFERSIZE];
    memset(payload, 0, BUFFERSIZE);
    strcpy(payload, "LSAs");
    seq[myNodeId] += 1;

    json LSA = {
        {"seq", seq[myNodeId]},
        {"links", graph[myNodeId]}};
    string strLSA = LSA.dump();

    memcpy(payload + 4, strLSA.c_str(), strLSA.length());
    string payloadStr(payload, payload + 4 + strLSA.length() + 1);
    return payloadStr;
}
*/

/*
string generateStrGraph()
{
    cout << "Inside generateStrGraph function." << endl;
    char payload[BUFFERSIZE];
    memset(payload, 0, BUFFERSIZE);
    strcpy(payload, "shar");

    json LSA(graph);
    string strG = LSA.dump();

    cout << "strGraph length " << strG.length() << endl;
    memcpy(payload + 4, strG.c_str(), strG.length());
    string payloadStr(payload, payload + 4 + strG.length() + 1);
    return payloadStr;
}
*/

void sendOrFowdMessage(char *recvBuf, int bytesRecvd)
{
    // cout << "Inside sendOrFowdMessage function." << endl;
    // const char *recvBuf = buffContent.c_str();

    short int destNodeId;
    memcpy(&destNodeId, recvBuf + 4, 2);
    destNodeId = ntohs(destNodeId);

    // string logContent = "Received send or forward message.";
    // logMessageAndTime(logContent.c_str());
    // cout << logContent << endl;

    // directMessage(destNodeId, recvBuf, bytesRecvd + 1);
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
int getNextHop(int destId)
{
    // cout << "Inside getNextHop function." << endl;
    int prev[256];             // i's previous node in path to desId
    int distanceToMyNode[256]; // i's total distance to desId
    prev[destId] = -1;
    bool visited[256];
    visited[myNodeId] = true;

    priority_queue<DN, std::vector<DN>, greater<DN>> frontier;
    frontier.push(make_pair(0, myNodeId));

    for (int i = 0; i < 256; i += 1)
    {
        distanceToMyNode[i] = INT_MAX;
    }
    distanceToMyNode[myNodeId] = 0;

    bool found = false;
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
                    frontier.push(make_pair(distanceToMyNode[v], v));
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
    return;

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

void directMessage(char *recvBuf, int bytesRecvd)
{
    short int destNodeId;
    memcpy(&destNodeId, recvBuf + 4, 2);
    destNodeId = ntohs(destNodeId);

    // string logContent = "Received send or forward message.";
    // logMessageAndTime(logContent.c_str());
    // cout << logContent << endl;

    char logLine[BUFFERSIZE];

    if (myNodeId == destNodeId)
    {
        sprintf(logLine, "receive packet message %s\n", recvBuf + 6);
    }
    else
    {
        int nexthop = getNextHop(destNodeId);
        if (nexthop != -1)
        {
            if (!strncmp(recvBuf, "send", 4))
            {
                char fowdMessage[bytesRecvd + 1];
                strcpy(fowdMessage, "fowd");
                memcpy(fowdMessage + 4, recvBuf + 4, bytesRecvd - 4);

                sendto(mySocketUDP, fowdMessage, bytesRecvd, 0,
                       (struct sockaddr *)&allNodeSocketAddrs[nexthop], sizeof(allNodeSocketAddrs[nexthop]));

                sprintf(logLine, "sending packet dest %d nexthop %d message %s\n", destNodeId, nexthop, recvBuf + 6);
            }
            else if (!strncmp(recvBuf, "fowd", 4))
            {
                sendto(mySocketUDP, recvBuf, bytesRecvd, 0,
                       (struct sockaddr *)&allNodeSocketAddrs[nexthop], sizeof(allNodeSocketAddrs[nexthop]));

                sprintf(logLine, "forward packet dest %d nexthop %d message %s\n", destNodeId, nexthop, recvBuf + 6);
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
