/* Link State Routing
 - Periodically send LSA only if my link status changes. New link status will be sent out. If no change, then no LSA.
 - Upon receiving LSA,
    - check ttl if it is still fresh. If it is too old, discard it.
    - immediately forward it out to my neighbors except myself and the predecessor
    - use it to update my graph database but do not send my LSA out.
 - For new neighbor, share my new valid links as LSA to it.
 - Data structure choice is critical for performance, use adjacency list instead of adjaceny matrix.
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
#define BUFFERSIZE 2000

using json = nlohmann::json;
using namespace std;

typedef std::pair<int, int> DN; // Used for Dijkstra's algorithm, total distance to source node and this node's id

int myNodeId, mySocketUDP;
struct timeval previousHeartbeat[256]; // track last time a neighbor is seen to figure out who is lost
struct sockaddr_in allNodeSocketAddrs[256];
int linkCost[256];
//int seqRecord[256];
//map<int, map<int, int>> graph;  // graph[i][j] indicates i->j link cost, -1 means not connected. This is using map to represent adjacency matrix, taking tooo much space and is too slow for processing 
map<int, pair<int, map<int, int>>> graph;    // graph[i] represents node i's latest sequence number and its adjacent neighbors in a map (neighborId -> cost)
std::mutex graph_lock;
atomic<bool> isChanged(false);               // track if my link status has changed since last time send out LSA
FILE* flog;
std::thread th[4];

void init(int inputId, string costFile, string logFile);
void readCostFile(const char* costFile);

void* announceHeartbeat(void* unusedParam);
void* announceLSA(void* unusedParam);
void detectNeighbors();
void listenForNeighbors();
bool checkNewNeighbor(int heardFrom);
void checkLostNeighbor();
void broadcastToValidNeighbor(const char* packetBuffer, int packetSize, int skipNeighbor);

void syncSeq(int newNeighborId);
void processSeqShare(const char* recvBuf, int bytesRecvd, int newNeighborId);
int createLSAPacket(char* packetBuffer, int nodeId);

void sendLSAToNeighbors();
//void processLSAMessage(const char* recvBuf, int bytesRecvd, int heardFrom);
void processLSAMessage(string buffContent, int bytesRecvd, int heardFrom);
int getNextHop(int destId);
//void directMessage(const char* recvBuf, int bytesRecvd);
void directMessage(string buffContent, int bytesRecvd);
// void testDij(int MyNodeId, int destId);

void setupNodeSockets();
void logMessageAndTime(const char* message);
void logTime();


/*  Initialize the graph, read linkcost from file.
    Note the graph doest initizle with other nodes' information, that will be adjacency matrix.
    Here we want to use map as adjacenty list, storing only active nodes in the graph. */
void init(int inputId, string costFile, string logFile) {
    myNodeId = inputId;

    // initailize linkCost and myPaths
    for (int i = 0; i < 256; i += 1) {
        linkCost[i] = 1;
    }

    linkCost[myNodeId] = 0;
    graph[myNodeId].first = 0;             // my initial seq number
    graph[myNodeId].second[myNodeId] = 0;  // my initial adjacency (neighbor, cost), me is a special neighbor of myself

    setupNodeSockets();
    readCostFile(costFile.c_str());

    flog = fopen(logFile.c_str(), "a");
}

/*  Periodically send heart beats to all nodes, neighbor or not. */
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

/*  Periodically chcek if there is any change in my link status (new neighbor or I lost any neighbor),
    send a new LSA to my neighbors. */
void* announceLSA(void* unusedParam) {
    struct timespec sleepFor;
    sleepFor.tv_sec = 0;
    sleepFor.tv_nsec = 400 * 1000 * 1000; // 400 ms

    while (1) {
        checkLostNeighbor(); // periodically check any lost neighbor, immediately check any new neighbor when reciving LSA in the listenForNeighbors function (main thread)
        if (isChanged) {    // isChanged contains any new neighbor changes, which is covered in the listenForNeighbors function (main thread)
            sendLSAToNeighbors();  // create a new LSA and send it out
            isChanged = false;
        }
        nanosleep(&sleepFor, 0);
    }
}

/* Handle received messages from my neighbors. */
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
            gettimeofday(&previousHeartbeat[heardFrom], 0);
            bool isNew = checkNewNeighbor(heardFrom);
            //if (isNew) {
            //    syncSeq(heardFrom);
            //}
        }
        // send/forward message 
        if (!strncmp(recvBuf, "send", 4) || !strncmp(recvBuf, "fowd", 4)) {
            string buffContent;
            buffContent.assign(recvBuf, recvBuf + bytesRecvd);
            if (th[0].joinable()) {
                th[0].join();
            }
            th[0] = thread(directMessage, buffContent, bytesRecvd);

            // if not using separate thread, no need to create a string
            //directMessage(recvBuf, bytesRecvd);
        }
        // LSA message
        else if (!strncmp(recvBuf, "LSAs", 4)) {

            string buffContent;
            buffContent.assign(recvBuf, recvBuf + bytesRecvd);
            if (th[1].joinable()) {
                th[1].join();
            }
            th[1] = thread(processLSAMessage, buffContent, bytesRecvd, heardFrom);


            // if not using separate thread, no need to create a string
            //processLSAMessage(recvBuf, bytesRecvd, heardFrom);
        }
        // seqShare message from new neighbor
        else if (!strncmp(recvBuf, "seqs", 4)) {
            //string buffContent;
            //buffContent.assign(recvBuf, recvBuf + bytesRecvd);
            //if (th[2].joinable()) {
            //    th[2].join();
            //}
            //th[2] = thread(processSeqShare, buffContent.c_str(), bytesRecvd, heardFrom);

            // if not using separate thread, no need to create a string
            //processSeqShare(recvBuf, bytesRecvd, heardFrom);
        }

    }
    close(mySocketUDP);
}

/* When receiving a data from a neighbor, check if it is a new neighbor. */
bool checkNewNeighbor(int heardFrom)
{
    bool isNew = false;
    graph_lock.lock();
    if (graph[myNodeId].second.find(heardFrom) == graph[myNodeId].second.end()) {
        graph[myNodeId].second[heardFrom] = linkCost[heardFrom];
        isChanged = true;
        isNew = true;
    }
    graph_lock.unlock();

    return isNew;
}

/* Check if there is any neighbor link is broken.
   This is used in announceLSA thread, so we need lock to protect shared data. */
void checkLostNeighbor() {
    struct timeval now;
    gettimeofday(&now, 0);

    // graph[myNodeId].second is the map: neighborId -> cost
    for (auto it = graph[myNodeId].second.cbegin(); it != graph[myNodeId].second.cend(); ) {
        long timeDifference = (now.tv_sec - previousHeartbeat[it->first].tv_sec) * 1000000L + now.tv_usec - previousHeartbeat[it->first].tv_usec;
        if (it->first != myNodeId && timeDifference > 800000) { // saw befor in longer than 800 ms
            graph_lock.lock();
            graph[myNodeId].second.erase(it++);
            graph_lock.unlock();

            isChanged = true;
        }
        else {
            ++it;
        }
    }
}

/* Share my active nodes' seq records to the new neighbor.
   The message format is ("seqs", map of validSeq <activeNodeId, latest seq number>). */
void syncSeq(int newNeighborId)
{
    char payload[BUFFERSIZE];
    memset(payload, 0, BUFFERSIZE);
    strcpy(payload, "seqs");

    map<int, int> validSeq;
    for (int i = 0; i < 256; i += 1) {
        if (graph.find(i) != graph.end()) {  // graph only stores active node sequence sicne a node record is created when seeing a LSA
            validSeq[i] = graph[i].first;
        }
    }
    json myAdj(validSeq);  // it is a map nodeId->seq number for valid node in my graph
    std::string strAdj = myAdj.dump();

    memcpy(payload + 4, strAdj.c_str(), strAdj.length());

    sendto(mySocketUDP, payload, strAdj.length() + 4, 0,
        (struct sockaddr*)&allNodeSocketAddrs[newNeighborId], sizeof(allNodeSocketAddrs[newNeighborId]));
}

/* Process the syncSeq message from new neighbor. The message header is "seqs"*/
void processSeqShare(const char* recvBuf, int bytesRecvd, int newNeighborId)
{
    std::string strSeq;
    strSeq.assign(recvBuf + 4, recvBuf + bytesRecvd);

    map<int, int> otherSeq = json::parse(strSeq);  // node->seq in new neighbor's database

    // if I have a node sequence which the other node doesn't have, or I have  a newer sequence for a node, 
    // share this node's LSA to the neighbor.
    for (auto const& kvPair : graph)
    {
        bool needToShare = otherSeq.count(kvPair.first) == 0 || graph[kvPair.first].first > otherSeq[kvPair.first]; // my record does exit in neighbor or my record is newer

        if (needToShare) {
            char packetBuffer[BUFFERSIZE];
            memset(packetBuffer, 0, BUFFERSIZE);

            int packetSize = createLSAPacket(packetBuffer, kvPair.first);

            sendto(mySocketUDP, packetBuffer, packetSize, 0,
                (struct sockaddr*)&allNodeSocketAddrs[newNeighborId], sizeof(allNodeSocketAddrs[newNeighborId]));
        }
    }
}

/* Create LSA packet for the nodeId in my graph. Store the restuls in bufer packetBuffer, return the byte size of LSA packet.
   LSA format is ("LSAs", nodeId, sequenceNum, map of all adj neighbors <neighbor, cost>).
   This is used in sendLSAToNeighbors() and processSeqShare(). */
int createLSAPacket(char* packetBuffer, int nodeId) {

    strcpy(packetBuffer, "LSAs");

    if (nodeId == myNodeId) {
        graph_lock.lock();
        graph[myNodeId].first += 1;  //The sequence number will increase by 1 if I for sending my LSA instead of other active nodes in graph.
        graph_lock.unlock();
    }

    int seqence = graph[nodeId].first;

    json j_adj(graph[nodeId].second);
    string adjStr = j_adj.dump();

    memcpy(packetBuffer + 4, &nodeId, sizeof(short int));
    memcpy(packetBuffer + 4 + sizeof(short int), &seqence, sizeof(int));
    memcpy(packetBuffer + 4 + sizeof(short int) + sizeof(int), adjStr.c_str(), adjStr.length());

    return 4 + sizeof(short int) + sizeof(int) + adjStr.length();
}


/* Link status changes, create a new LSA packet and send it to my neighbors.
   This is used in announceLSA function thread, so we need lock to protect the data.
   The sequence number will increase by 1 in the packet creation process. */
void sendLSAToNeighbors() {
    char packetBuffer[BUFFERSIZE];
    int packetSize;
    memset(packetBuffer, 0, BUFFERSIZE);

    packetSize = createLSAPacket(packetBuffer, myNodeId);

    broadcastToValidNeighbor(packetBuffer, packetSize, myNodeId);  // myNodeId is a dummy num to fill the function parameter
}

/*  Process the received LSA message, discard it if it is not newer than my record.
    Send it to other neighbors (except the neighbor sendind this to me). */
    //void processLSAMessage(const char* recvBuf, int bytesRecvd, int neighborId)
void processLSAMessage(string buffContent, int bytesRecvd, int neighborId)
{
    const char* recvBuf = buffContent.c_str();
    std::string strLSA;
    strLSA.assign(recvBuf + 4 + sizeof(short int) + sizeof(int), recvBuf + bytesRecvd);

    short int sourceId;
    int receivedSeq;
    memcpy(&sourceId, recvBuf + 4, sizeof(short int));
    memcpy(&receivedSeq, recvBuf + 4 + sizeof(short int), sizeof(int));
    bool seqExistAndLarger = graph.find(sourceId) != graph.end() && graph[sourceId].first >= receivedSeq;

    if (sourceId == myNodeId || seqExistAndLarger) {
        char buff[200];
        snprintf(buff, sizeof(buff), "Old LSA from neighbor %d, for sourceId %d, the received seq number is %d, and my recorded seq is %d.", neighborId, sourceId, receivedSeq, graph[sourceId].first);
        logMessageAndTime(buff);
        return;
    }

    map<int, int> otherAdj = json::parse(strLSA);

    graph_lock.lock();
    graph[sourceId].first = receivedSeq;
    graph[sourceId].second = otherAdj;
    graph_lock.unlock();

    char buff[200];
    snprintf(buff, sizeof(buff), "New LSA from neighbor %d, for sourceId %d, the received seq number is %d, and my recorded seq is %d. The links are %s.", neighborId, sourceId, receivedSeq, graph[sourceId].first, recvBuf + 4 + sizeof(short int) + sizeof(int));
    logMessageAndTime(buff);

    broadcastToValidNeighbor(recvBuf, bytesRecvd, neighborId);
}

/* Send out content to my valid neighbors. It is used for sendLSAToNeighbors() and processLSAMessage(). */
void broadcastToValidNeighbor(const char* packetBuffer, int packetSize, int skipNeighbor) {
    // iterate through the adjacey list Map:  neighborID -> cost
    for (auto const& kv : graph[myNodeId].second) {
        // here we will not send the message to the skiNeighbor, it is useful for resend LSA and skip the neighbor sending this LSA to me
        if (kv.first != myNodeId && kv.first != skipNeighbor) {
            sendto(mySocketUDP, packetBuffer, packetSize, 0,
                (struct sockaddr*)&allNodeSocketAddrs[kv.first], sizeof(allNodeSocketAddrs[kv.first]));
        }
    }
}

/* Run Dijkstra's algorithm to find nextHop to reach destId from myself. */
int getNextHop(int destId)
{
    char payload[BUFFERSIZE];
    memset(payload, 0, BUFFERSIZE);
    strcpy(payload, "seqs");

    map<int, int> validSeq;
    for (int i = 0; i < 256; i += 1) {
        if (graph.find(i) != graph.end()) {  // graph only stores active node sequence sicne a node record is created when seeing a LSA
            validSeq[i] = graph[i].first;
        }
    }
    json myAdj(validSeq);  // it is a map nodeId->seq number for valid node in my graph
    std::string strAdj = myAdj.dump();

    memcpy(payload + 4, strAdj.c_str(), strAdj.length());

    logMessageAndTime(payload);

    // cout << "Inside getNextHop function." << endl;
    int prev[256];             // i's previous node in path to desId
    int distanceToMyNode[256]; // i's total distance to desId
    prev[destId] = -1;
    bool visited[256];
    visited[myNodeId] = true;

    class Compare
    {
    public:
        bool operator() (DN d1, DN d2)
        {
            return d1.first > d2.first || (d1.first == d2.first && d1.second > d2.second);
        }
    };

    std::priority_queue<DN, std::vector<DN>, Compare> frontier;  // pair<distanceToMyNode, nodeId>
    for (auto const& kv : graph) {
        int nodeId = kv.first;
        distanceToMyNode[nodeId] = INT_MAX;
        //frontier.push(std::make_pair(INT_MAX, nodeId));
    }
    distanceToMyNode[myNodeId] = 0;
    frontier.push(std::make_pair(0, myNodeId));

    bool found = false;
    while (!frontier.empty()) {

        int uDist = frontier.top().first;
        int u = frontier.top().second;

        frontier.pop();
        //visited[u] = true;
        //if (u == destId) {
        //    break;
        //}
        for (auto const& kvPair : graph[u].second) {  // kv will be u's adjacent list map: (neighborId, cost)
            int v = kvPair.first;
            int uvCost = kvPair.second;
            if (distanceToMyNode[u] + uvCost < distanceToMyNode[v]) {
                distanceToMyNode[v] = uDist + uvCost;
                frontier.push(std::make_pair(distanceToMyNode[v], v));
                prev[v] = u;
            }

            /*
            if (visited[v] == false) {
                if (distanceToMyNode[u] + uvCost < distanceToMyNode[v]) {
                    distanceToMyNode[v] = distanceToMyNode[u] + uvCost;
                    frontier.push(std::make_pair(distanceToMyNode[v], v));
                    prev[v] = u;
                }
                visited[v] = true;
            }
            */
        }
    }

    if (prev[destId] == -1) {
        string logContent = "In getNextHop, not find next hop";
        logMessageAndTime(logContent.c_str());
        return -1;
    }

    int p = destId;
    while (prev[p] != myNodeId) {
        p = prev[p];
    }

    // string logContent = "In getNextHop, found next hop";
    // logMessageAndTime(logContent.c_str());
    return p;
}

/* Run init() first, then run this function to find the shortest path from myNodeId to destId. */
void testDij(int destId)
{
    //graph =  [[0,[1,[[0,0],[1,1],[10,1]]]],[1,[1,[[0,1],[1,0],[2,1],[11,1]]]],[2,[1,[[1,1],[2,0],[3,1],[12,1]]]],[3,[1,[[2,1],[3,0],[4,1],[13,1]]]],[4,[2,[[3,1],[4,0],[5,1],[14,1]]]],[5,[2,[[4,1],[5,0],[6,1],[15,1]]]],[6,[1,[[5,1],[6,0],[7,1],[16,1]]]],[7,[1,[[6,1],[7,0],[17,1]]]],[10,[1,[[0,1],[10,0],[11,1],[20,1]]]],[11,[2,[[1,1],[10,1],[11,0],[12,1],[21,1]]]],[12,[1,[[2,1],[11,1],[12,0],[13,1],[22,1]]]],[13,[2,[[3,1],[12,1],[13,0],[14,1],[23,1]]]],[14,[1,[[4,1],[13,1],[14,0],[15,1],[24,1]]]],[15,[2,[[5,1],[14,1],[15,0],[16,1],[25,1]]]],[16,[2,[[6,1],[15,1],[16,0],[17,1],[26,1]]]],[17,[1,[[7,1],[16,1],[17,0],[27,1]]]],[20,[1,[[10,1],[20,0],[21,1],[30,1]]]],[21,[1,[[11,1],[20,1],[21,0],[22,1],[31,1]]]],[22,[2,[[12,1],[21,1],[22,0],[23,1],[32,1]]]],[23,[1,[[13,1],[22,1],[23,0],[24,1],[33,1]]]],[24,[2,[[14,1],[23,1],[24,0],[25,1],[34,1]]]],[25,[1,[[15,1],[24,1],[25,0],[26,1],[35,1]]]],[26,[1,[[16,1],[25,1],[26,0],[27,1],[36,1]]]],[27,[2,[[17,1],[26,1],[27,0],[37,1]]]],[30,[2,[[20,1],[30,0],[31,1],[40,1]]]],[31,[1,[[21,1],[30,1],[31,0],[32,1],[41,1]]]],[32,[2,[[22,1],[31,1],[32,0],[33,1],[42,1]]]],[33,[2,[[23,1],[32,1],[33,0],[34,1],[43,1]]]],[34,[2,[[24,1],[33,1],[34,0],[35,1],[44,1]]]],[35,[2,[[25,1],[34,1],[35,0],[36,1],[45,1]]]],[36,[1,[[26,1],[35,1],[36,0],[37,1],[46,1]]]],[37,[1,[[27,1],[36,1],[37,0],[47,1]]]],[40,[1,[[30,1],[40,0],[41,1],[50,1]]]],[41,[1,[[31,1],[40,1],[41,0],[42,1],[51,1]]]],[42,[2,[[32,1],[41,1],[42,0],[43,1],[52,1]]]],[43,[1,[[33,1],[42,1],[43,0],[44,1],[53,1]]]],[44,[2,[[34,1],[43,1],[44,0],[45,1],[54,1]]]],[45,[2,[[35,1],[44,1],[45,0],[46,1],[55,1]]]],[46,[1,[[36,1],[45,1],[46,0],[47,1],[56,1]]]],[47,[1,[[37,1],[46,1],[47,0],[57,1]]]],[50,[2,[[40,1],[50,0],[51,1],[60,1]]]],[51,[2,[[41,1],[50,1],[51,0],[52,1],[61,1]]]],[52,[2,[[42,1],[51,1],[52,0],[53,1],[62,1]]]],[53,[1,[[43,1],[52,1],[53,0],[54,1],[63,1]]]],[54,[1,[[44,1],[53,1],[54,0],[55,1],[64,1]]]],[55,[1,[[45,1],[54,1],[55,0],[56,1],[65,1]]]],[56,[1,[[46,1],[55,1],[56,0],[57,1],[66,1]]]],[57,[1,[[47,1],[56,1],[57,0],[67,1]]]],[60,[1,[[50,1],[60,0],[61,1],[70,1]]]],[61,[1,[[51,1],[60,1],[61,0],[62,1],[71,1]]]],[62,[1,[[52,1],[61,1],[62,0],[63,1],[72,1]]]],[63,[2,[[53,1],[62,1],[63,0],[64,1],[73,1]]]],[64,[1,[[54,1],[63,1],[64,0],[65,1],[74,1]]]],[65,[2,[[55,1],[64,1],[65,0],[66,1],[75,1]]]],[66,[2,[[56,1],[65,1],[66,0],[67,1],[76,1]]]],[67,[1,[[57,1],[66,1],[67,0],[77,1]]]],[70,[1,[[60,1],[70,0],[71,1]]]],[71,[2,[[61,1],[70,1],[71,0],[72,1]]]],[72,[1,[[62,1],[71,1],[72,0],[73,1]]]],[73,[2,[[63,1],[72,1],[73,0],[74,1]]]],[74,[1,[[64,1],[73,1],[74,0],[75,1]]]],[75,[2,[[65,1],[74,1],[75,0],[76,1]]]],[76,[1,[[66,1],[75,1],[76,0],[77,1]]]],[77,[1,[[67,1],[76,1],[77,0]]]]];
    graph[0].second = { {255, 555} };
    graph[1].second = { {2, 54}, {4, 1}, {5, 2}, {6, 1}, {255, 1} };
    graph[2].second = { {1, 54}, {3, 1}, {5, 1} };
    graph[3].second = { {2, 1}, {4, 1} };
    graph[4].second = { {1, 1}, {3, 1}, {7, 1} };
    graph[5].second = { {1, 2}, {2, 2}, {6, 1} };
    graph[6].second = { {7, 3}, {1, 1}, {5, 1} };
    graph[7].second = { {6, 3}, {4, 1} };
    graph[255].second = { {0, 555}, {1, 1} };

    getNextHop(destId);
}

/* Handling send or fowd message.
   If we run this with another subthread, we need to copy the recvBuf and pass a sring. */
   //void directMessage(const char* recvBuf, int bytesRecvd)
void directMessage(string buffContent, int bytesRecvd)
{
    const char* recvBuf = buffContent.c_str();
    logMessageAndTime(recvBuf + 6);

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
                memset(fowdMessage, 0, bytesRecvd);

                strcpy(fowdMessage, "fowd");
                memcpy(fowdMessage + 4, recvBuf + 4, bytesRecvd - 4);

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

/* Read the link cost between me and my neighbors if we are connected. */
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

/* Setup sockets for all possible nodes. */
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

/* Helper function to log transitant information and time during program execution. */
void logMessageAndTime(const char* message) {
    //return;
    char logLine[BUFFERSIZE];
    sprintf(logLine, " %s\n", message);
    fwrite(logLine, 1, strlen(logLine), flog);
    fflush(flog);
    logTime();
}

/* Helper function to log time during program execution. */
void logTime() {
    struct timeval now;
    gettimeofday(&now, 0);

    char logLine[100];
    sprintf(logLine, "    Time is at %ld ms.\n", (now.tv_sec * 1000000L + now.tv_usec) / 1000);
    fwrite(logLine, 1, strlen(logLine), flog);
    fflush(flog);
}
