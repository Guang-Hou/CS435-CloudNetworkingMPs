/* Path Vector Routing
 - One node stores its paths to destId and all its neighbors' paths.
 - Periodically send only NEW, changed paths to existing neighbors. This avoids overflooding the network if immediately sending every new path.
 - Upon receiving a neighbor LSA <fromID(for sure my neighbor), destID, dist, nextHop, nodesInPath>
    - Immediately store this LSA in my database for this neighbor's paths.
    - Use this LSA to update my paths but do not immediately send my updated paths out.
 - During the whole process, track changedPaths and lostPaths.
    - For lostPaths, we will chooseAlternativeForLostPaths, and for changedPaths we will sendLSAToNeighbors. This separation will balance convergence and avoid loops.
    - Periodically check lost neighbor right before sending out updated LSA, to see if any of my paths changed and lost.
    - Check any paths are changed or lost when processing neighbor LSA.
        - Special case if my nextHop sends me a path including me for same destId, this means I lost my path to this destId.
    - For any destId path that was changed, immediately remove its entry from nodePaths[myNode], add it to changedPaths list. Smae for lostPaths.
    - Before sending out LSA, choose alternative paths for all destIds in lostPaths. If we choose alternative path right away when finding a lost destID,
      we may use obselate paths and create loops.
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

typedef tuple<int, short int, short int, unordered_set<int>> PATH; // (seq, distance, nextHop, nodesInPath)
map<int, map<int, PATH>> nodePaths;  // nodePaths[i][j] the PATH from node i to destNode i
                                     // nodePaths[i], i can only be myself and my active neighbors
                                     // nodePaths[i][j], j can be both valid and invalid destId
//unordered_set<int> changedPaths;   // track the destIds where my paths to them changed since last sending LSA: no path-> got a path; old path -> new path; old path -> lost the path
//unordered_set<int> lostPaths;      // track the destIds where my paths to them are lost: only tracking old path -> lost the path. 
//std::mutex change_lock;     // lock changedPaths when modifying it
//std::mutex lost_lock;       // lock lostPaths when modifying it
std::mutex paths_lock;      // lock nodePaths[myNodeId] when modifying it

int myNodeId, mySocketUDP;
struct timeval previousHeartbeat[256]; // track last time a neighbor is seen to figure out who is lost
struct sockaddr_in allNodeSocketAddrs[256];
int linkCost[256];
FILE* flog;
bool enableLog = true;  // whether to log run time information

std::thread th[4];

void init(int inputId, string costFile, string logFile);

void* announceHeartbeat(void* unusedParam);
void* announceLSA(void* unusedParam);

void listenForNeighbors();
void processLSAMessage(string buffContent, int bytesRecvd, int heardFrom);
void sendNewPathToRelativeNeighbors(int destId, char* packetBuffer, int packetSize);

//bool checkNewNeighbor(int heardFrom);
void sharePathsToNewNeighbor(int newNeighborId);
//void checkLostNeighbor();
void handleLostNeighbors();
void handleLostPath(int destId);
//void chooseAlternativeForLostPaths();
//void sendAlternativePathToNeighbors();
void sendAlternativePathToAllNeighbors(int destId, char *packetBuffer, int packetSize);

int createLSAPacket(char* packetBuffer, int nodeId);
//void sendLSAToNeighbors();

void directMessage(string buffContent, int bytesRecvd);
void readCostFile(const char* costFile);
void setupNodeSockets();
void logMessageAndTime(const char* message);
void logTime();


/*  Initialize nodePaths[myNodeId], read linkcost information from file. */
void init(int inputId, string costFile, string logFile) {
    myNodeId = inputId;

    for (int i = 0; i < 256; i += 1) {
        linkCost[i] = 1;
    }

    linkCost[myNodeId] = 0;
    nodePaths[myNodeId][myNodeId] = { 0, 0, myNodeId, unordered_set<int>{myNodeId} }; // for example of node 3: (0, 0, 3, {3}), the path includes the node itself

    setupNodeSockets();
    readCostFile(costFile.c_str());

    flog = fopen(logFile.c_str(), "a");
}

/*  Periodically send heart beats to all nodes, neighbor or not. 
    Check if I lost any neighbor. If there is any path lost due to that neighbor, send LSA updates out. */
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
        
        handleLostNeighbors();

        nanosleep(&sleepFor, 0);
    }
}

/*  Periodically send changed paths to my neighbors. Also use this to regularly check if I lost any neighbor. */
/*
void* announceLSA(void* unusedParam) {
    struct timespec sleepFor;
    sleepFor.tv_sec = 0;
    sleepFor.tv_nsec = 300 * 1000 * 1000; // 300 ms

    while (1) {
        //checkLostNeighbor();              // periodically check if I lost any neighbor and if any paths are affected
        //chooseAlternativeForLostPaths();  // select alternative paths for destId in lostPaths
        //sendAlternativePathToAllNeighbors(); // create a new LSA of path for all destId in lostPaths and send it out to all my neighbors

        handleLostNeighbors();

        nanosleep(&sleepFor, 0);
    }
}
*/

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
            //bool isNew = checkNewNeighbor(heardFrom);
            if (nodePaths.find(heardFrom) == nodePaths.end()) {
                sharePathsToNewNeighbor(heardFrom);
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

            /*
            if (th[1].joinable()) {
                th[1].join();
            }
            th[1] = thread(processLSAMessage, buffContent, bytesRecvd, heardFrom);
            */

            thread process_thread(processLSAMessage, buffContent, bytesRecvd, heardFrom);
            process_thread.detach();   // must make sure the order of processing LSA doesn't matter
        }
    }
    close(mySocketUDP);
}

/* When receiving a data from a neighbor, if it is a new neighbor, add it to myNeighbors set.
   No need to consider this neighbor's impact on nodePaths. In next periodical announceLSA, I will receive this new neighbor's paths.*/
/*
bool checkNewNeighbor(int heardFrom) {
    bool isNew = false;
    if (nodePaths.find(heardFrom) == nodePaths.end())
    {
        if (enableLog) {
            string logContent = "  Saw a new neighbor ";
            logContent += to_string(heardFrom);
            logMessageAndTime(logContent.c_str());
        }

        //paths_lock.lock();
        //nodePaths[heardFrom][heardFrom];  // this will create a record in nodePaths for this new neighbor
        //paths_lock.unlock();

        isNew = true;
    }

    return isNew;
}
*/

/* Check if there is any neighbor link is broken. If so, remove it from myNeighbor set,
   check if any path is changed because of this borken link.
   This is used in announceLSA thread, so we need lock to protect shared data. */
/*
void checkLostNeighbor() {
    //string logContent = "Inside checkLostNeighbor.";
    //logMessageAndTime(logContent.c_str());
    struct timeval now;
    gettimeofday(&now, 0);

    // get the list of lost neighbors
    unordered_set<int> lostNeighbors;
    for (auto i = nodePaths.begin(); i != nodePaths.end(); ) {
        //logContent = "Inside checkLostNeighbor, iterating through neighbor ";
        //logContent += to_string(i->first);
        //logMessageAndTime(logContent.c_str());
        long timeDifference = (now.tv_sec - previousHeartbeat[i->first].tv_sec) * 1000000L
            + now.tv_usec - previousHeartbeat[i->first].tv_usec;

        if (i->first != myNodeId && timeDifference > 800000) { // saw befor in longer than 800 ms
            int lostNb = i->first;

            if (enableLog) {
                string logContent = "I lost neighbor node  ";
                logContent += to_string(lostNb);
                logMessageAndTime(logContent.c_str());
            }

            paths_lock.lock();
            i = nodePaths.erase(i);             // remove this neighbor's entry in my database
            paths_lock.unlock();

            lostNeighbors.insert(lostNb);
        }
        else {
            ++i;
        }
    }

    // check if this broken link causes my paths to chagne, get the list of lost destIds
    for (auto lostNb : lostNeighbors) {
        for (auto pathPtr = nodePaths[myNodeId].begin(); pathPtr != nodePaths[myNodeId].end(); ) {
            if (get<1>(pathPtr->second) == lostNb) { // I lost the path to destId due to broken link to the neighbor
                int lostDestId = pathPtr->first;
                int nextHop = get<1>(pathPtr->second);

                lost_lock.lock();
                paths_lock.lock();
                lostPaths.insert(lostDestId);
                pathPtr = nodePaths[myNodeId].erase(pathPtr);   // only stores destId if I have a path to it
                paths_lock.unlock();
                lost_lock.unlock();

                if (enableLog) {
                    char buff[200];
                    snprintf(buff, sizeof(buff), "Because of this lost neighbor %d, I lost my path to destId %d, becasue the next hop is %d.", lostNb, lostDestId, nextHop);
                    logMessageAndTime(buff);
                }
            }
            else {
                ++pathPtr;
            }
        }
    }
}
*/


/* Check if there is any neighbor link is broken. If so, remove it from nodePaths,
   check if any of my path is changed because of this borken link. If so, choose alternative and send out LSA updates. */
void handleLostNeighbors() {
    //string logContent = "Inside handleLostNeighbors.";
    //logMessageAndTime(logContent.c_str());
    struct timeval now;
    gettimeofday(&now, 0);

    // get the list of lost neighbors
    unordered_set<int> lostNeighbors;
    for (auto i = nodePaths.begin(); i != nodePaths.end(); ) {
        //logContent = "Inside handleLostNeighbors, iterating through neighbor ";
        //logContent += to_string(i->first);
        //logMessageAndTime(logContent.c_str());
        long timeDifference = (now.tv_sec - previousHeartbeat[i->first].tv_sec) * 1000000L
            + now.tv_usec - previousHeartbeat[i->first].tv_usec;

        if (i->first != myNodeId && timeDifference > 600000) { // saw befor in longer than 600 ms
            int lostNb = i->first;

            if (enableLog) {
                string logContent = "I lost neighbor node  ";
                logContent += to_string(lostNb);
                logMessageAndTime(logContent.c_str());
            }

            paths_lock.lock();
            i = nodePaths.erase(i);             // remove this neighbor's entry in my database
            paths_lock.unlock();

            lostNeighbors.insert(lostNb);
        }
        else {
            ++i;
        }
    }

    // check if this broken link causes my paths to change, get the list of lost destIds
    for (auto lostNb : lostNeighbors) {
        for (const auto& destIdPtr : nodePaths[myNodeId]) {
            int destId = pathPtr.first;
            int distance = get<1>(pathPtr.second);
            int nextHop = get<2>(pathPtr.second);

            if (distance != -1 && nextHop == lostNb) {
                if (enableLog) {
                    char buff[200];
                    snprintf(buff, sizeof(buff), "Because of this lost neighbor %d, I lost my path to destId %d, becasue the next hop is %d.", lostNb, lostDestId, nextHop);
                    logMessageAndTime(buff);
                }

                handleLostPath(destId);
            }
        }
    }
}

/* Call this after losing my path to destId. Choose an alternative (might be a withdrawal), and send it to all my neighbors.
   It is used in handleLostNeighbors() and processLSAMessage().*/
void handleLostPath(int destId) {
    int bestNb = -1;
    int myDistToDest = INT_MAX;
    for (const auto& i : nodePaths) {
        int nb = i.first;
        if (nb != myNodeId && nodePaths[nb].find(destId) != nodePaths[nb].end() && get<1>(nodePaths[nb][destId]) != -1) {      // this neibhor has a path to destId
            if (get<3>(nodePaths[nb][destId]).count(myNodeId) == 0) { // this path doesn't go through me
                int newDist = get<1>(nodePaths[nb][destId]) + linkCost[nb];
                if (newDist < myDistToDest || (newDist == myDistToDest && nb < bestNb)) {
                    bestNb = nb;
                    myDistToDest = newDist;
                }
            }
        }
    }

    // Find an alternative path to this destId.
    if (bestNb != -1) {
        int curSeq = get<0>(nodePaths[myNodeId][destId]);
        paths_lock.lock();
        nodePaths[myNodeId][destId] = { curSeq + 1, myDistToDest, bestNb, get<3>(nodePaths[bestNb][destId]) };
        get<3>(nodePaths[myNodeId][destId]).insert(myNodeId);
        paths_lock.unlock();

        if (enableLog) {
            json j_set(get<2>(nodePaths[myNodeId][destId]));
            string setStr = j_set.dump();
            char buff[200];

            snprintf(buff, sizeof(buff), "chooseAlternativeForLostPaths: chose alternative path neighbor %d to destId %d,and nodesInPath %s. Sent this LSA update to all my neighbors.", bestNb, destId, setStr.c_str());
            logMessageAndTime(buff);
        }
    }
    // No alternative path, then I lost path to this destId.
    else {
        int curSeq = get<0>(nodePaths[myNodeId][destId]);
        paths_lock.lock();
        get<0>(nodePaths[myNodeId][destId]) = curSeq + 1;
        get<1>(nodePaths[myNodeId][destId]) = -1;
        paths_lock.unlock();

        if (enableLog) {
            char buff[200];
            snprintf(buff, sizeof(buff), "chooseAlternativeForLostPaths, no alternative path to destId %d. Sent withdrawal LSA to all me neighbors.", destId);
            logMessageAndTime(buff);
        }
    }

    char packetBuffer[BUFFERSIZE];
    memset(packetBuffer, 0, BUFFERSIZE);
    int packetSize = createLSAPacket(packetBuffer, destId);
    sendAlternativePathToAllNeighbors(destId, packetBuffer, packetSize);
}

/* For any desId in the changedPaths, choose an alternative next hop from my existing neighbors to reach destId.
   If there is an alternative path, update in nodePaths; if there none then do nothing, and nodePaths[myNodeId] will not contain destId.
   It is used in checkLostNeighbor() and processLSAMessage() when I lost my path to destId. */
/*
void chooseAlternativeForLostPaths() {
    for (auto const& destId : lostPaths) {
        int bestNb = -1;
        int myDistToDest = INT_MAX;
        for (const auto& i : nodePaths) {
            int nb = i.first;
            if (nb != myNodeId && nodePaths[nb].find(destId) != nodePaths[nb].end()) {      // this neibhor has a path to destId
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
            nodePaths[myNodeId][destId] = { myDistToDest, bestNb, get<2>(nodePaths[bestNb][destId]) };
            get<2>(nodePaths[myNodeId][destId]).insert(myNodeId);
            paths_lock.unlock();

            if (enableLog) {
                json j_set(get<2>(nodePaths[myNodeId][destId]));
                string setStr = j_set.dump();
                char buff[200];

                snprintf(buff, sizeof(buff), "chooseAlternativeForLostPaths: chose alternative path neighbor %d to destId %d,and nodesInPath %s.", bestNb, destId, setStr.c_str());
                logMessageAndTime(buff);
            }
        }
        else {
            paths_lock.lock();
            nodePaths[myNodeId].erase(destId);
            paths_lock.unlock();

            if (enableLog) {
                char buff[200];
                snprintf(buff, sizeof(buff), "chooseAlternativeForLostPaths, no alternative path to destId %d. Erased this destId from nodePaths[myNodeId].", destId);
                logMessageAndTime(buff);
            }
        }
    }
}
*/

/* Send out the alternative path for lostDestId in lostPaths to all neighbors.
   Use this after calling chooseAlternativeForLostPaths(). */
/*
void sendAlternativePathToAllNeighbors() {
    //logMessageAndTime("Inside sendLSAToNeighbors.");
    char packetBuffer[BUFFERSIZE];
    int packetSize;

    // I previous lost the path to destId and I already chose an alternative path, then send it to all my neighbors
    // the alternative path might be a withdraw announcemnet: distance = -1, or a real alternative path
    for (auto const& destId : lostPaths) {
        memset(packetBuffer, 0, BUFFERSIZE);
        packetSize = createLSAPacket(packetBuffer, destId);
        for (auto const& i : nodePaths) {
            int nb = i.first;
            if (nb != myNodeId && nb != destId) {
                sendto(mySocketUDP, packetBuffer, packetSize, 0,
                    (struct sockaddr*)&allNodeSocketAddrs[nb], sizeof(allNodeSocketAddrs[nb]));

                if (enableLog) {
                    char buff[200];
                    snprintf(buff, sizeof(buff), "Inside sendLSAToNeighbors, sending my lost-alternative path fromId %d -> destId %d to my neighbor %d, the nodes are %s.", myNodeId, destId, nb, packetBuffer + 4 + 4 * sizeof(short int) + sizeof(int));
                    logMessageAndTime(buff);
                }
            }
        }
    }

    lost_lock.lock();
    lostPaths.clear();
    lost_lock.unlock();
}
*/


/* After choosing an alternative path for destId, share this to all my neighbors. */
void sendAlternativePathToAllNeighbors(int destId, char *packetBuffer, int packetSize) {
    for (auto const& i : nodePaths) {
        int nb = i.first;
        if (nb != myNodeId && nb != destId) {
            sendto(mySocketUDP, packetBuffer, packetSize, 0,
                (struct sockaddr*)&allNodeSocketAddrs[nb], sizeof(allNodeSocketAddrs[nb]));

            if (enableLog) {
                char buff[200];
                snprintf(buff, sizeof(buff), "Inside sendAlternativePathToAllNeighbors, sending my lost-alternative path fromId %d -> destId %d to my neighbor %d, the nodes are %s.", myNodeId, destId, nb, packetBuffer + 4 + 4 * sizeof(short int) + sizeof(int));
                logMessageAndTime(buff);
            }
        }
    }
}


/* When seeing a new neighbor, share my paths not including the new neighbor to it. */
void sharePathsToNewNeighbor(int newNeighborId) {
    //logMessageAndTime("Inside sharenodePaths[myNodeId]ToNewNeighbor.");
    char packetBuffer[BUFFERSIZE];
    int packetSize;
    for (const auto& destIdPtr : nodePaths[myNodeId]) {
        // the new neighbor is not in the path, then share this to it
        if (destIdPtr.first != newNeighborId && get<1>(destIdPtr.second) != 0 && get<3>(destIdPtr.second).count(newNeighborId) == 0) {
            memset(packetBuffer, 0, BUFFERSIZE);
            packetSize = createLSAPacket(packetBuffer, destIdPtr.first);

            if (enableLog) {
                char buff[200];
                snprintf(buff, sizeof(buff), "Inside sharePathsToNewNeighbor, sharing my path fromId %d -> destId %d to my neighbor %d, the nodes are %s.", myNodeId, destIdPtr.first, newNeighborId, packetBuffer + 4 + 4 * sizeof(short int) + sizeof(int));
                logMessageAndTime(buff);
            }

            sendto(mySocketUDP, packetBuffer, packetSize, 0,
                (struct sockaddr*)&allNodeSocketAddrs[newNeighborId], sizeof(allNodeSocketAddrs[newNeighborId]));
        }
    }
    //logMessageAndTime("Enf of sharenodePaths[myNodeId]ToNewNeighbor.");
}

/* Create LSA packet for my path to destId. Store the restuls in buffer packetBuffer, return the byte size of LSA packet.
   LSA format is ("LSAs", fromId, destId, seq, distance, nextHop, nodesInPath).
   This is used in sendLSAToNeighbors() and sharenodePaths[myNodeId]ToNewNeighbor(). */
int createLSAPacket(char* packetBuffer, int destinationId) {
    //logMessageAndTime("Inside createLSAPacket.");
    strcpy(packetBuffer, "LSAs");
    short int fromId = (short int)myNodeId;
    short int destId = (short int)destinationId;
    int seq = (int)get<0>(nodePaths[myNodeId][destId]);;
    short int distance = (short int)get<1>(nodePaths[myNodeId][destId]);
    short int nextHop = (short int)get<2>(nodePaths[myNodeId][destId]);
    string strSet; // serialized nodesInPath

    // save time and space if this is a lost path message
    if (distance == -1) {
        strSet = "";
    }
    else {
        json j_set(get<3>(nodePaths[myNodeId][destId]));
        strSet = j_set.dump();
    }

    memcpy(packetBuffer + 4, &fromId, sizeof(short int));
    memcpy(packetBuffer + 4 + sizeof(short int), &destId, sizeof(short int));
    memcpy(packetBuffer + 4 + 2 * sizeof(short int), &seq, sizeof(int));
    memcpy(packetBuffer + 4 + 2 * sizeof(short int) + sizeof(int), &distance, sizeof(short int));
    memcpy(packetBuffer + 4 + 3 * sizeof(short int) + sizeof(int), &nextHop, sizeof(short int));
    memcpy(packetBuffer + 4 + 4 * sizeof(short int) + sizeof(int), strSet.c_str(), strSet.length());

    if (enableLog) {
        char buff[200];
        snprintf(buff, sizeof(buff), "Created LSA packet. fromId %d, destId %d, sequence %d, distance %d, nexHop %d, and nodesInPath %s.", fromId, destId, seq, distance, nextHop, strSet.c_str());
        logMessageAndTime(buff);
    }

    return 4 + 4 * sizeof(short int) + sizeof(int) + strSet.length();
}


/* I got a new path, send it to only relative neighbors,
   the path will not be a withdraw announcement: distance = -1. */
void sendNewPathToRelativeNeighbors(int destId, char* packetBuffer, int packetSize) {
    for (auto const& i : nodePaths) {
        int nb = i.first;
        if (nb != myNodeId && nb != destId && get<3>(nodePaths[myNodeId][destId]).count(nb) == 0) {

            sendto(mySocketUDP, packetBuffer, packetSize, 0,
                (struct sockaddr*)&allNodeSocketAddrs[nb], sizeof(allNodeSocketAddrs[nb]));

            if (enableLog) {
                char buff[200];
                snprintf(buff, sizeof(buff), "Inside sendLSAToNeighbors, sending my newer path fromId %d -> destId %d to my neighbor %d, the nodes are %s.", myNodeId, destId, nb, packetBuffer + 4 + 4 * sizeof(short int) + sizeof(int));
                logMessageAndTime(buff);
            }
        }
    }
}

/*  Process the received LSA message, update nodePaths[myNodeId].
    LSA format is ("LSAs", fromId, destId, distance, nextHop, nodesInPath).*/
void processLSAMessage(string buffContent, int bytesRecvd, int neighborId)
{
    //logMessageAndTime("Inside processLSAMessage.");
    const char* recvBuf = buffContent.c_str();
    short int fromId, destId, distance, nextHop;
    int seq;
    string otherNodesInPathStr;

    memcpy(&fromId, recvBuf + 4, sizeof(short int));
    memcpy(&destId, recvBuf + 4 + sizeof(short int), sizeof(short int));
    memcpy(&seq, recvBuf + 4 + 2 * sizeof(short int), sizeof(int));

    if (destId == myNodeId || seq <= get<0>(nodePaths[fromId][destId])) {
        return;
    }

    memcpy(&distance, recvBuf + 4 + 2 * sizeof(short int) + sizeof(int), sizeof(short int));
    memcpy(&nextHop, recvBuf + 4 + 3 * sizeof(short int) + sizeof(int), sizeof(short int));
    otherNodesInPathStr.assign(recvBuf + 4 + 4 * sizeof(short int) + sizeof(int), recvBuf + bytesRecvd);

    paths_lock.lock();
    get<0>(nodePaths[fromId][destId]) = seq;
    get<1>(nodePaths[fromId][destId]) = distance;
    paths_lock.unlock();

    unordered_set<int> otherNodesInPath;
    if (distance != -1) {
        json j_path = json::parse(otherNodesInPathStr);
        otherNodesInPath = j_path.get<unordered_set<int>>();
        //otherNodesInPath = (unordered_set<int>)json::parse(otherNodesInPathStr);
    }

    if (enableLog) {
        char buff[200];
        snprintf(buff, sizeof(buff), "Received LSA from neighbor %d, for destId %d, the distance is %d, the nexHop is %d, and its PATH is: %s.", fromId, destId, distance, nextHop, otherNodesInPathStr.c_str());
        logMessageAndTime(buff);
    }

    // neighbor lost a path 
    if (distance == -1) {
        // check if my paths are affected
        bool pathExistAndAffected = nodePaths[myNodeId].find(destId) != nodePaths[myNodeId].end() && get<2>(nodePaths[myNodeId][destId]) == fromId;
        if (pathExistAndAffected) {  // my path to destId becomes invalid
            if (enableLog) {
                char buff[200];
                snprintf(buff, sizeof(buff), "Neighbor %d lost it path to destId %d, because of that I lost my path to this destId %d.", fromId, destId, destId);
                logMessageAndTime(buff);
            }

            handleLostPath(destId);
        }
    }
    // neighbor shares to me his path fromId -> destId
    else {
        // update in nodePaths[fromId]
        if (otherNodesInPath.count(myNodeId) == 0) {
            paths_lock.lock();
            get<2>nodePaths[fromId][destId] = nextHop;
            get<3>nodePaths[fromId][destId] = otherNodesInPath;
            paths_lock.unlock();
        }
        else {
            paths_lock.lock();
            get<1>(nodePaths[fromId][destId]) = -1;  // for me, this neighbor's path is not valid
            paths_lock.unlock();
        }

        // I do not have a path to this destId
        if (nodePaths[myNodeId].find(destId) == nodePaths[myNodeId].end()) {
            if (otherNodesInPath.count(myNodeId) == 0) { //neighbpr path doesn't go throught me, then use the neighbor's path
                if (enableLog) {
                    char buff[200];
                    snprintf(buff, sizeof(buff), "I do not have a path to desId %d, but neighbor %d has, I will use it.", destId, fromId);
                    logMessageAndTime(buff);
                }

                int curSeq = get<0>(nodePaths[myNodeId][destId]);
                paths_lock.lock();
                nodePaths[myNodeId][destId] = { curSeq + 1, distance + linkCost[fromId], fromId, otherNodesInPath };
                get<3>(nodePaths[myNodeId][destId]).insert(myNodeId);
                paths_lock.unlock();

                char packetBuffer[BUFFERSIZE];
                memset(packetBuffer, 0, BUFFERSIZE);
                int packetSize = createLSAPacket(packetBuffer, destId);
                sendNewLSAToValidNeighbors(destId, packetBuffer, packetSize);
            }
        }
        // I have a path to this destId, neighbor has a path to this destId
        else {
            int curDistance = get<1>(nodePaths[myNodeId][destId]);
            int newDistance = distance + linkCost[fromId];
            int curNextHop = get<2>(nodePaths[myNodeId][destId]);
            // if neighbor path is valid to me
            if (otherNodesInPath.count(myNodeId) == 0) {
                if ((newDistance < curDistance || (newDistance == curDistance && fromId <= curNextHop))) { //it is better, use it
                    if (enableLog) {
                        char buff[200];
                        snprintf(buff, sizeof(buff), "We both have paths. Neighbor %d has a better path to desId %d, I will use it.", fromId, destId);
                        logMessageAndTime(buff);
                    }

                    int curSeq = get<0>(nodePaths[myNodeId][destId]);
                    paths_lock.lock();
                    nodePaths[myNodeId][destId] = { curSeq + 1, distance + linkCost[fromId], fromId, otherNodesInPath };
                    get<3>(nodePaths[myNodeId][destId]).insert(myNodeId);
                    paths_lock.unlock();

                    char packetBuffer[BUFFERSIZE];
                    memset(packetBuffer, 0, BUFFERSIZE);
                    int packetSize = createLSAPacket(packetBuffer, destId);
                    sendNewLSAToValidNeighbors(destId, packetBuffer, packetSize);
                }
                else { // neighbor path is not better than my current path
                    if (fromId == curNextHop) { // message is from my current nextHop, then the my previous path became invalid, I lost my path
                        if (enableLog) {
                            char buff[200];
                            snprintf(buff, sizeof(buff), "We both have paths. Neighbor %d is my current nextHop and it got a worst path to desId %d, then I lost my path to this destId.", fromId, destId);
                            logMessageAndTime(buff);
                        }

                        handleLostPath(destId);
                    }
                }
            }
            else {  // neighbpr path is not valid to me, it is using me in its path
                if (fromId == curNextHop) { // message is from my current nextHop, then I lost my path
                    if (enableLog) {
                        char buff[200];
                        snprintf(buff, sizeof(buff), "We both have paths. Neighbor %d is my current nextHop and it is now using me in his path to destId %d, then I lost my path to this destId.", fromId, destId);
                        logMessageAndTime(buff);
                    }

                    handleLostPath(destId);
                }
            }
        }
    }
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

    if (enableLog) {
        string logContent = "Received send or fowd message.";
        logMessageAndTime(logContent.c_str());
    }

    if (myNodeId == destNodeId) {
        sprintf(logLine, "receive packet message %s\n", recvBuf + 6);
    }
    else {
        int nextHop = -1;
        if (nodePaths[myNodeId].find(destNodeId) != nodePaths[myNodeId].end()) {
            nextHop = get<1>(nodePaths[myNodeId][destNodeId]);
        }

        if (nextHop != -1) {
            if (!strncmp(recvBuf, "send", 4)) {
                char fowdMessage[bytesRecvd];
                strcpy(fowdMessage, "fowd");
                memcpy(fowdMessage + 4, recvBuf + 4, bytesRecvd - 4);

                sendto(mySocketUDP, fowdMessage, bytesRecvd, 0,
                    (struct sockaddr*)&allNodeSocketAddrs[nextHop], sizeof(allNodeSocketAddrs[nextHop]));
                sprintf(logLine, "sending packet dest %d nexthop %d message %s\n", destNodeId, nextHop, recvBuf + 6);
            }
            else if (!strncmp(recvBuf, "fowd", 4)) {
                sendto(mySocketUDP, recvBuf, bytesRecvd, 0,
                    (struct sockaddr*)&allNodeSocketAddrs[nextHop], sizeof(allNodeSocketAddrs[nextHop]));
                sprintf(logLine, "forward packet dest %d nexthop %d message %s\n", destNodeId, nextHop, recvBuf + 6);
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