/* Path Vector Routing
 - One node stores its paths to destId and all its neighbors' paths.
 - Periodically send only NEW, changed paths to existing neighbors. This avoids overflooding the network if immediately sending every new path.
 - Upon receiving a neighbor LSA <fromID(for sure my neighbor), destID, dist, nextHop, nodesInPath>
    - Immediately store this LSA in my database for this neighbor's paths.
    - Use this LSA to update my paths but do not immediately send my updated paths out.
 - During the whole process, check if a path to destId is changed since previous periodical sending out. This changedPaths set will track the destId whose path from me to it has chagned.
    - Periodically check lost neighbor right before sending out updated LSA, to see if any of my paths changed.
    - Check any paths are changed when processing neighbor LSA.
- For a new neighbor
    - Immediately share all my valid paths to this new neighbor.
- For a lost neighbor
    - Clear this neighbor's paths in my database.
    - Check if any of my path is affected, if so choose a new path by going through other neighbors.
- One alternative strategy is to just store my paths.
    - When I lost a neighbor, I mark any of my path going through this neighbor as GONE. Send out LSA to my other neighbors.
    - When the other neighbor receive any GONE path from me, they will sends back there valid path to that destId to me.
    - In this way, one node only needs to store my paths. But there are too many LSA relating to broken link. It overfloods the nodes.
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
map<int, map<int, PATH>> nodePaths;  // nodePaths[i][j] the path from node i to destNode i; 
// only store my paths and my active neighbors' paths to active destId
// In temporary state, it may contain a destId of distance -1 when processign LSA and before sending this out in announceLSA.
unordered_set<int> myNeighbors;      // track my live neighbors, necessary?
unordered_set<int> changedPaths;     // track the destIds where my paths to them has changed since last sending LSA
std::mutex change_lock;     // lock changedPaths when modifying it
std::mutex paths_lock;      // lock myPaths when modifying it
std::mutex neighbors_lock;  // lock myNeighbors when modifying it

int myNodeId, mySocketUDP;
struct timeval previousHeartbeat[256]; // track last time a neighbor is seen to figure out who is lost
struct sockaddr_in allNodeSocketAddrs[256];
int linkCost[256];
FILE* flog;

std::thread th[4];

void init(int inputId, string costFile, string logFile);

void* announceHeartbeat(void* unusedParam);
void* announceLSA(void* unusedParam);

void listenForNeighbors();
void processLSAMessage(string buffContent, int bytesRecvd, int heardFrom);
void directMessage(string buffContent, int bytesRecvd);

bool checkNewNeighbor(int heardFrom);
void shareMyPathsToNewNeighbor(int newNeighborId);
void checkLostNeighbor();
void chooseAlternativePath(int destId, int lostNb);

int createLSAPacket(char* packetBuffer, int nodeId);
void sendLSAToNeighbors();

void readCostFile(const char* costFile);
void setupNodeSockets();
void logMessageAndTime(const char* message);
void logTime();


/*  Initialize myPaths, read linkcost information from file. */
void init(int inputId, string costFile, string logFile) {
    myNodeId = inputId;

    for (int i = 0; i < 256; i += 1) {
        linkCost[i] = 1;
    }

    linkCost[myNodeId] = 0;
    nodePaths[myNodeId][myNodeId] = { 0, myNodeId, unordered_set<int>{myNodeId} }; // for example of node 3: (0, (3,  {3})), the path includes the node itself

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
            if (i != myNodeId) {
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
        checkLostNeighbor();   // periodically check if I lost any neighbor and if any paths are affected

        sendLSAToNeighbors();  // create a new LSA of path to dest node i and send it out to my neighbors

        change_lock.lock();
        changedPaths.clear();
        change_lock.unlock();

        nanosleep(&sleepFor, 0);
    }
}

/* Handle received messages from my neighbors. */
void listenForNeighbors() {
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
            if (isNew) {
                shareMyPathsToNewNeighbor(heardFrom);
            }
        }
        // send/forward message 
        if (!strncmp(recvBuf, "send", 4) || !strncmp(recvBuf, "fowd", 4)) {
            string buffContent;
            buffContent.assign(recvBuf, recvBuf + bytesRecvd);
            if (th[0].joinable()) {
                th[0].join();
            }
            th[0] = thread(directMessage, buffContent, bytesRecvd);
        }
        // LSA message
        else if (!strncmp(recvBuf, "LSAs", 4)) {
            string buffContent;
            buffContent.assign(recvBuf, recvBuf + bytesRecvd);
            if (th[1].joinable()) {
                th[1].join();
            }
            th[1] = thread(processLSAMessage, buffContent, bytesRecvd, heardFrom);
        }
    }
    close(mySocketUDP);
}

/* When receiving a data from a neighbor, if it is a new neighbor, add it to myNeighbors set.
   No need to consider this neighbor's impact on myPaths. In next periodical announceLSA, I will receive this new neighbor's valid paths.*/
bool checkNewNeighbor(int heardFrom) {
    bool isNew = false;
    if (nodePaths.find(heardFrom) == nodePaths.end())
    {
        string logContent = "  Saw a new neighbor ";
        logContent += to_string(heardFrom);
        logMessageAndTime(logContent.c_str());

        paths_lock.lock();
        nodePaths[heardFrom][heardFrom] = { 0, heardFrom, unordered_set<int>{heardFrom} };
        paths_lock.unlock();
        isNew = true;
    }

    return isNew;
}

/* Check if there is any neighbor link is broken. If so, remove it from myNeighbor set,
   check if any path is changed because of this borken link.
   This is used in announceLSA thread, so we need lock to protect shared data. */
void checkLostNeighbor() {
    string logContent = "Inside checkLostNeighbor.";
    logMessageAndTime(logContent.c_str());
    struct timeval now;
    gettimeofday(&now, 0);

    // get the list of lost neighbors
    unordered_set<int> lostNeighbors;
    for (auto i = nodePaths.begin(); i != nodePaths.end(); ) {
        logContent = "Inside checkLostNeighbor, iterating through neighbor ";
        logContent += to_string(i->first);
        logMessageAndTime(logContent.c_str());
        long timeDifference = (now.tv_sec - previousHeartbeat[i->first].tv_sec) * 1000000L
            + now.tv_usec - previousHeartbeat[i->first].tv_usec;

        if (*i != myNodeId && timeDifference > 800000) { // saw befor in longer than 800 ms
            logContent = "I lost neighbor node  ";
            logContent += to_string(i->first);
            logMessageAndTime(logContent.c_str());

            paths_lock.lock();
            i = nodePaths.erase(i);
            paths_lock.unlock();
            lostNeighbors.insert(i->first);
        }
        else {
            ++i;
        }
    }

    // this broken link may or may not cause myPaths to chagne
    for (auto lostNb : lostNeighbors) {
        for (auto pathPtr = nodePaths[myNodeId].begin(); destIdPtr != nodePaths[myNodeId].end(); ) {
            if (get<1>(pathPtr->second) == lostNb) { // I lost the path to destId due to broken link to the neighbor
                change_lock.lock();
                paths_lock.lock();
                changedPaths.insert(pathPtr->first);
                pathPtr = myPaths[myNodeId].erase(pathPtr);   // myPaths only store destId if I have a path to it
                paths_lock.unlock();
                change_lock.unlock();

                chooseAlternativePath(pathPtr->first, lostNb);
            }
            else {
                ++pathPtr;
            }
        }
    }
}

void chooseAlternativePath(int destId) {
    int bestNb = -1;
    int myDistToDest = INT_MAX;
    for (const auto& i : nodePaths) {
        logContent = "Inside chooseAlternativePath, iterating through neighbor ";
        logContent += to_string(i->first);
        logMessageAndTime(logContent.c_str());

        int nb = i->first;
        if (nodePaths[nb].find(destId) != nodePaths[nb].end()) {      // this neibhor has a path to destId
            if (get<2>(nodePaths[nb][destId]).count(myNodeId) == 0) { // this path doesn't go through me
                int newDist = get<0>(nodePaths[nb][destId]) + linkCost[nb];
                if (newDist < myDistToDest || (newDist == myDistToDest && nb < bestNb)) {
                    bestNb = nb;
                    myDistToDest = newDist;
                }
            }
        }
    }

    if (bestNb != -1) {
        paths_lock.lock();
        nodePaths[myNodeId] = { myDistToDet, bestNb, nodePaths[bestNb][destId] };
        get<2>(nodePaths[myNodeId]).insert(myNodeId);
        paths_lock.unlock();
    }
}


/* When seeing a new neighbor, share my paths not including the new neighbor to it. */
void shareMyPathsToNewNeighbor(int newNeighborId) {
    //logMessageAndTime("Inside shareMyPathsToNewNeighbor.");
    char packetBuffer[BUFFERSIZE];
    int packetSize;
    for (const auto& destIdPtr : myPaths[myNodeId]) {
        //string logContent = "destId: ";
        //logContent += to_string(destIdPtr->first);
        //logMessageAndTime(logContent.c_str());

        if (destIdPtr->first != newNeighborId && get<2>(destIdPtr->second).count(newNeighborId) == 0) { // the new neighbor is not in the path, then share this to it
            memset(packetBuffer, 0, BUFFERSIZE);                                                        // also no need to share my self path, the other neighbor has handled this when seeing me as a new neighbor
            packetSize = createLSAPacket(packetBuffer, destIdPtr->first);
            sendto(mySocketUDP, packetBuffer, packetSize, 0,
                (struct sockaddr*)&allNodeSocketAddrs[newNeighborId], sizeof(allNodeSocketAddrs[newNeighborId]));
        }
    }
    //logMessageAndTime("Enf of shareMyPathsToNewNeighbor.");
}

/* Create LSA packet for my path to destId. Store the restuls in buffer packetBuffer, return the byte size of LSA packet.
   LSA format is ("LSAs", fromId, destId, distance, nextHop, nodesInPath).
   This is used in ?. */
int createLSAPacket(char* packetBuffer, int destinationId) {
    //logMessageAndTime("Inside createLSAPacket.");
    strcpy(packetBuffer, "LSAs");
    short int fromId = (short int)myNodeId;
    short int destId = (short int)destinationId;
    short int distance, nextHop;
    string strSet; // serialized nodesInPath

    // if I do not have a path to this destId, this is used for sendLSAtoNeighbors when I lost my path
    if (nodePaths[myNodeId].find(destId) == nodePaths[myNodeId].end()) {
        distance = -1;
        strSet = "";
    }
    else {
        distance = (short int)get<0>(nodePaths[myNodeId][destId]);
        nextHop = (short int)get<1>(nodePaths[myNodeId][destId]);
        json j_set(get<2>(nodePaths[myNodeId][destId]));
        strSet = j_set.dump();
    }

    memcpy(packetBuffer + 4, &fromId, sizeof(short int));
    memcpy(packetBuffer + 4 + sizeof(short int), &destId, sizeof(short int));
    memcpy(packetBuffer + 4 + 2 * sizeof(short int), &distance, sizeof(short int));
    memcpy(packetBuffer + 4 + 3 * sizeof(short int), &nextHop, sizeof(short int));
    memcpy(packetBuffer + 4 + 4 * sizeof(short int), strSet.c_str(), strSet.length());

    //logMessageAndTime("End of createLSAPacket.");
    return 4 + 4 * sizeof(short int) + strSet.length();
}


/* Send out all my changed paths to neighbors periodically. It is used in announceLSA(). */
void sendLSAToNeighbors() {
    //logMessageAndTime("Inside sendLSAToNeighbors.");
    char packetBuffer[BUFFERSIZE];
    int packetSize;

    for (auto const& destId : changedPaths) {
        memset(packetBuffer, 0, BUFFERSIZE);
        packetSize = createLSAPacket(packetBuffer, destId);
        for (auto const& i : nodePaths) {
            if (i->first != myNodeId && get<2>(nodePaths[myNodeId][destId]).count(i->first) == 0) {  // only send path not going through the neighbor
                sendto(mySocketUDP, packetBuffer, packetSize, 0,
                    (struct sockaddr*)&allNodeSocketAddrs[i->first], sizeof(allNodeSocketAddrs[i->first]));
            }
        }
    }
}

/*  Process the received LSA message, update myPaths.
    LSA format is ("LSAs", fromId, destId, distance, nextHop, nodesInPath).*/
void processLSAMessage(string buffContent, int bytesRecvd, int neighborId)
{
    //logMessageAndTime("Inside processLSAMessage.");
    const char* recvBuf = buffContent.c_str();
    short int fromId, destId, distance, nextHop;
    string otherNodesInPathStr;

    memcpy(&fromId, recvBuf + 4, sizeof(short int));
    memcpy(&destId, recvBuf + 4 + sizeof(short int), sizeof(short int));
    memcpy(&distance, recvBuf + 4 + 2 * sizeof(short int), sizeof(short int));
    memcpy(&nextHop, recvBuf + 4 + 3 * sizeof(short int), sizeof(short int));
    otherNodesInPathStr.assign(recvBuf + 4 + 4 * sizeof(short int), recvBuf + bytesRecvd);

    unordered_set<int> otherNodesInPath;
    if (distance != -1) {
        json j_path = json::parse(otherNodesInPathStr);
        otherNodesInPath = j_path.get<unordered_set<int>>();
        //otherNodesInPath = (unordered_set<int>)json::parse(otherNodesInPathStr);
    }

    // neighbor lost a path 
    if (distance == -1) {
        // update in nodePaths[fromId], remove path for this destID

        bool pathExistAndAffected = myPaths.find(destId) != myPaths.end() && get<1>(myPaths[destId]) == fromId;
        bool pathExistAndNotAffected = myPaths.find(destId) != myPaths.end() && get<1>(myPaths[destId]) != fromId && get<2>(myPaths[destId]).count(fromId) == 0;
        if (pathExistAndAffected) {  // my path to destId becomes invalid
            change_lock.lock();
            paths_lock.lock();
            changedPaths.insert(destId);
            chooseAlternativePath(destId);
            paths_lock.unlock();
            change_lock.unlock();
        }
    }
    else { // neighbor has an alternative path 
        // update in nodePaths[fromId], update the path for destID
        if (myPaths.find(destId) == myPaths.end() || get<0>(myPaths[destId]) == -1) { // I do not have a path to this destId
            change_lock.lock();
            paths_lock.lock();
            changedPaths.insert(destId);
            get<0>(myPaths[destId]) = distance + linkCost[fromId];
            get<1>(myPaths[destId]) = fromId;
            get<2>(myPaths[destId]) = otherNodesInPath;
            get<2>(myPaths[destId]).insert(myNodeId);
            paths_lock.unlock();
            change_lock.unlock();
        }
        else if (otherNodesInPath.count(myNodeId) == 0) {  // the alternative path is valid to me, i.e. i am not in the neighbor's path
            if (distance + linkCost[fromId] < get<0>(myPaths[destId])
                || (distance + linkCost[fromId] == get<0>(myPaths[destId]) && get<1>(myPaths[destId]) > fromId)) { // neighbor path is better
                change_lock.lock();
                paths_lock.lock();
                changedPaths.insert(destId);
                get<0>(myPaths[destId]) = distance + linkCost[fromId];
                get<1>(myPaths[destId]) = fromId;
                get<2>(myPaths[destId]) = otherNodesInPath;
                get<2>(myPaths[destId]).insert(myNodeId);
                paths_lock.unlock();
                change_lock.unlock();
            }
        }
    }

    char buff[200];
    snprintf(buff, sizeof(buff), "Processed LSA from neighbor %d, for destId %d, its path has nodes: %s.", fromId, destId, otherNodesInPathStr.c_str());
    logMessageAndTime(buff);
}


/* Handling send or fowd message.
   If we run this with another subthread, we need to copy the recvBuf and pass a sring. */
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
        int nexthop = get<1>(myPaths[destNodeId]);
        if (nexthop != -1) {
            if (!strncmp(recvBuf, "send", 4)) {
                char fowdMessage[bytesRecvd];
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
