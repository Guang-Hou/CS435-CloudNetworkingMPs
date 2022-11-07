/* Path Vector Routing
 - Periodically send only new, changed paths to existing neighbors. This avoids overflooding the network if dynamically send every new path.
 - Upon receiving a neighbor LSA (fromID-it is for sure my neighbor, destID, dist, nextHop, nodesInPath)
    - use it to update my paths but do not immediately send my updated paths out.
 - During the whole process, check if a path to destId is changed since previous periodical sending out. This changed[] will be used in periodical send out.
    - New link or lost link may change some of my path to some destID
    - processing LSA may change some of my path to some destID
- For a new neighbor
    - Immediately share all my valid paths to this new neighbor, either old or new paths. To accelerate the convergence.
    - This is not enabled now.Test other first.
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
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#define BUFFERSIZE 3000

using json = nlohmann::json;
using namespace std;

typedef tuple<int, int, unordered_set<int>> PATH; // (distance, nextHop, nodesInPath)
map<int, PATH> myPaths;   // myPaths[i] means my path to destNode i; i -> (my distance to node i, nextHop, nodesInPath)
                          // Keep invariant: myPaths only store destId if I have a path to it.
unordered_set<int> myNeighbors;    // track my neighbor list
unordered_set<int> changedPaths;   // track the destIds where my paths to them has changed since last sending LSA

int myNodeId, mySocketUDP;
struct timeval previousHeartbeat[256]; // track last time a neighbor is seen to figure out who is lost
struct sockaddr_in allNodeSocketAddrs[256];
int linkCost[256];
std::mutex change_lock;
std::mutex paths_lock;
FILE* flog;

std::thread th[4];

void init(int inputId, string costFile, string logFile);
void readCostFile(const char* costFile);

void* announceHeartbeat(void* unusedParam);
void* announceLSA(void* unusedParam);
void detectNeighbors();
void listenForNeighbors();
bool checkNewNeighbor(int heardFrom)
void checkLostNeighbor();

void syncSeq(int newNeighborId);
void processSeqShare(const char* recvBuf, int bytesRecvd, int newNeighborId);
int createLSAPacket(char* packetBuffer, int nodeId);

void sendLSAToNeighbors();
void processLSAMessage(const char* recvBuf,, int bytesRecvd, int heardFrom);
int getNextHop(int destId);
void directMessage(const char* recvBuf, int bytesRecvd);
// void testDij(int MyNodeId, int destId);

void setupNodeSockets();
void logMessageAndTime(const char* message);
void logTime();


/*  Initialize myPaths, read linkcost information from file. */
void init(int inputId, string costFile, string logFile) {
    myNodeId = inputId;

    for (int i = 0; i < 256; i += 1) {
        isChanged[i] = false;
        linkCost[i] = 1;
        get<0>(myPaths[i]) = -1; // at beginning I do not have any path to other nodes
    }

    linkCost[myNodeId] = 0;
    myPaths[i] = { 0, myNodeId, unordered_set<int>{myNodeId} }; // for example of node 3: (0, (3,  {3})), the path includes the node itself

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

/*  Periodically send changed paths to my neighbors. Also use this to regularly check if I lost any neighbor. */
void* announceLSA(void* unusedParam) {
    struct timespec sleepFor;
    sleepFor.tv_sec = 0;
    sleepFor.tv_nsec = 400 * 1000 * 1000; // 400 ms

    while (1) {

        checkLostNeighbor();  // periodically check if I lost any neighbor

        for (const auto& i : changedPaths) {
            sendLSAToNeighbors(i);  // create a new LSA of path to dest node i and send it out to my neighbors
        }

        change_lock.lock();
        changedPaths.clear();
        change_lock.unlock();

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
            //    shareMyPathsToNewNeighbor(heardFrom);
            //}
        }
        // send/forward message 
        if (!strncmp(recvBuf, "send", 4) || !strncmp(recvBuf, "fowd", 4)) {
            string buffContent;
            buffContent.assign(recvBuf, recvBuf + bytesRecvd);
            if (th[0].joinable()) {
                th[0].join();
            }
            th[0] = thread(directMessage, buffContent.c_str(), bytesRecvd);

            // if not using separate thread, no need to create a string
            // directMessage(recvBuf, bytesRecvd);
        }
        // LSA message
        else if (!strncmp(recvBuf, "LSAs", 4)) {
            string buffContent;
            buffContent.assign(recvBuf, recvBuf + bytesRecvd);
            if (th[1].joinable()) {
                th[1].join();
            }
            th[1] = thread(processLSAMessage, buffContent.c_str(), bytesRecvd, heardFrom);

            // if not using separate thread, no need to create a string
            // processLSAMessage(recvBuf, bytesRecvd, heardFrom);
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

/* When receiving a data from a neighbor, if it is a new neighbor, add it to myNeighbors set. */
bool checkNewNeighbor(int heardFrom)
{   
    bool isNew = false;
    if (myNeighbors.count(heardFrom) == 0)
    {
        string logContent = "  Saw a new neighbor ";
        logContent += to_string(heardFrom);
        logMessageAndTime(logContent.c_str());
        //  cout << logContent << endl;

        myNeighbors.insert(heardFrom);

        unordered_set<int> selfPath{ heardFrom };
        processSingleLSA(heardFrom, heardFrom, 0, heardFrom, selfPath);  // later this will not be needed since neighbor will share this to me

        isNew = true;

        logContent = "  Finished processing seeing new neighbor ";
        logContent += to_string(heardFrom);
        logMessageAndTime(logContent.c_str());
        // cout << logContent << endl;
    }

    return isNew;
}

/* Check if there is any neighbor link is broken. If so, remove it from myNeighbor set, check any path is changed because of this borken link.
   This is used in announceLSA thread, so we need lock to protect shared data. */
void checkLostNeighbor()  {
    struct timeval now;
    gettimeofday(&now, 0);

    for (auto i = myNeighbors.begin(); i != myNeighbors.end(); ) {
        long timeDifference = (now.tv_sec - previousHeartbeat[i].tv_sec) * 1000000L + now.tv_usec - previousHeartbeat[i].tv_usec;
        if (timeDifference > 800000) { // saw befor in longer than 800 ms
            myNeighbors.erase(i);
            // this broken link may or may not cause myPahts to chagne
            for (auto destId = myPaths.begin(); destId != myPaths.end(); )    {
                if (get<1>(myPaths[destId]) == neighborId) { // I lost the path to destId due to broken link to the neighbor
                    changedPaths.insert(destId);
                    myPaths.erase(destId); // myPaths only store destId if I have a path to it
                }
            }
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

    sendto(mySocketUDP, payload, strLSA.length() + 4, 0,
        (struct sockaddr*)&allNodeSocketAddrs[newNeighborId], sizeof(allNodeSocketAddrs[newNeighborId]));
}

/* When seeing a new neighbor, share my paths not including the new neighbor to it. */
void shareMyPathsToNewNeighbor(const char* recvBuf, int bytesRecvd, int newNeighborId) 
{
    std::string strSeq;
    strSeq.assign(recvBuf + 4, recvBuf + bytesRecvd);

    map<int, int> otherSeq = json::parse(strSeq);  // node->seq in new neighbor's database

    // if I have newer sequence for a node, share this node's LSA to the neighbor
    for (auto const& i : graph)
    {   
        bool needToShare = otherSeq.find(i) == otherSeq.end() || graph[i].first > otherSeq[i]; // my record does exit in neighbor or my record is newer

        char packetBuffer[BUFFERSIZE];
        memset(packetBuffer, 0, BUFFERSIZE);

        int packetSize = createLSAPacket(packetBuffer, i);

        sendto(mySocketUDP, packetBuffer, packetSize, 0,
            (struct sockaddr*)&allNodeSocketAddrs[newNeighborId], sizeof(allNodeSocketAddrs[newNeighborId]));
        
    }
}

/* Create LSA packet for my path to destId. Store the restuls in buffer packetBuffer, return the byte size of LSA packet. 
   LSA format is ("LSAs", fromId, destId, distance, nextHop, nodesInPath).
   This is used in ?. */
int createLSAPacket(char* packetBuffer, int destinationId) {

    strcpy(packetBuffer, "LSAs");
    short int fromId = int myNodeId;
    short int destId = (short int) destinationId;
    short int distance, nextHop;
    string strSet; // serialized nodesInPath

    // if I do not have a path to this destId, this is used for sendLSAtoNeighbors when I lost my path
    if (myPahts.find(destId) == myPahts.end()) {
        distance = -1; 
        strSet = "";
    } else {
        distance = get<0>(myPahts[destId]);
        nextHop = get<1>(myPahts[destId]);
        json j_set(get<1>(myPahts[destId]));
        string strSet = j_set.dump();
    }

    memcpy(packetBuffer + 4, &fromId, sizeof(short int));
    memcpy(packetBuffer + 4 + sizeof(short int), &destId, sizeof(short int));
    memcpy(packetBuffer + 4 + 2*sizeof(short int), &distance, sizeof(short int));
    memcpy(packetBuffer + 4 + 3*sizeof(short int), &nextHop, sizeof(short int));
    memcpy(packetBuffer + 4 + 4*sizeof(short int), strSet.c_str(), strSet.length());

    return 4 + 4*sizeof(short int) + strSet.length();
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
void processLSAMessage(const char* recvBuf, int bytesRecvd, int neighborId)
{
    std::string strLSA;
    strLSA.assign(recvBuf + 4 + sizeof(short int) + sizeof(int), recvBuf + bytesRecvd);

    short int sourceId;
    int receivedSeq;
    memcpy(&sourceId, recvBuf + 4, sizeof(short int));
    memcpy(&receivedSeq, recvBuf + 4 + sizeof(short int), sizeof(int));
    bool seqExistAndLarger = graph.find[i] != graph.end() && graph[i].first >= receiveSeq;

    if (sourceId == myNodeId || seqExistAndLarger) {
        char buff[200];
        snprintf(buff, sizeof(buff), "Old LSA from neighbor %d, for sourceId %d, the received seq number is %d, and my recorded seq is %d.", neighborId, sourceId, receivedSeq, graph[i].first);
        logMessageAndTime(buff);
        return;
    }

    map<int, int> otherAdj = json::parse(strLSA);

    graph_lock.lock();
    graph[i].first = receivedSeq;
    graph[i].second = otherAdj;
    graph_lock.unlock();

    char buff[200];
    snprintf(buff, sizeof(buff), "New LSA from neighbor %d, for sourceId %d, the received seq number is %d, and my recorded seq is %d. The links are %s.", neighborId, sourceId, receivedSeq, graph[i].first, buffContent.c_str() + 8);
    logMessageAndTime(buff);

    broadcastToValidNeighbor(recvBuf, bytesRecvd, neighborId);
}

/* send out content to my valid neighbors. It isused for sendLSAToNeighbors() and processLSAMessage(). */
void broadcastToValidNeighbor(const char* packetBuffer, int packetSize, int skipNeighbor) {
    for (int i = 0; i < 256; i += 1) {
        if (i != myNodeId && i != skipNeighbor && graph[myNodeId].second.find(i) != graph[myNodeId].second.end()) {
            sendto(mySocketUDP, packetBuffer, packetSize, 0,
                (struct sockaddr*)&allNodeSocketAddrs[i], sizeof(allNodeSocketAddrs[i]));
        }
    }
}

/* Run Dijkstra's algorithm to find nextHop to reach destId from myself. */
int getNextHop(int destId)
{
    // cout << "Inside getNextHop function." << endl;
    int prev[256];             // i's previous node in path to desId
    int distanceToMyNode[256]; // i's total distance to desId
    prev[destId] = -1;
    bool visited[256];
    visited[myNodeId] = true;

    std::priority_queue<DN, std::vector<DN>, std::greater<DN>> frontier;  // pair<distanceToMyNode, nodeId>
    frontier.push(std::make_pair(0, myNodeId));

    for (int i = 0; i < 256; i += 1)
    {
        distanceToMyNode[i] = INT_MAX;
    }
    distanceToMyNode[myNodeId] = 0;

    bool found = false;

    while (!frontier.empty())    {
        int size = frontier.size();

        for (int i = 0; i < size; i += 1)    {
            int dist = frontier.top().first;
            int u = frontier.top().second;

            frontier.pop();

            if (u == destId)   {
                found = true;
                break;
            }

            if (graph.find(u) != graph.end()) {
                for (auto const& v : graph[u].second) {
                    if (!visited[v]) {
                        if (distanceToMyNode[v] > distanceToMyNode[u] + graph[u].second[v])      {
                            distanceToMyNode[v] = distanceToMyNode[u] + graph[u].second[v];
                        }

                        frontier.push(std::make_pair(distanceToMyNode[v], v));
                        prev[v] = u;
                        visited[v] = true;
                    }
                }
            }
        }

        if (found)     {
            break;
        }
    }

    if (prev[destId] == -1)   {
        // string logContent = "In getNextHop, not find next hop";
        // logMessageAndTime(logContent.c_str());
        return -1;
    }

    int p = destId;
    while (prev[p] != myNodeId)   {
        p = prev[p];
    }

    // string logContent = "In getNextHop, found next hop";
    // logMessageAndTime(logContent.c_str());

    return p;
}

/* Run init() first, then run this function to find the shortest path from myNodeId to destId. */
void testDij(int destId)
{
    graph[0].second = {{255, 555}};
    graph[1].second = {{2, 54}, {4, 1}, {5, 2}, {6, 1}, {255, 1}};
    graph[2].second = {{1, 54}, {3, 1}, {5, 1}};
    graph[3].second = {{2, 1}, {4, 1}};
    graph[4].second = {{1, 1}, {3, 1}, {7, 1}};
    graph[5].second = {{1, 2}, {2, 2}, {6, 1}};
    graph[6].second = {{7, 3}, {1, 1}, {5, 1}};
    graph[7].second = {{6, 3}, {4, 1}};
    graph[255].second = {{0, 555}, {1, 1}};

    return getNextHop(destId);
}

/* Handling send or fowd message.
   If we run this with another subthread, we need to copy the recvBuf and pass a sring. */
void directMessage(const char* recvBuf, int bytesRecvd)
{
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
