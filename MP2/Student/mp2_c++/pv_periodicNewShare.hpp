/* Path Vector Routing
 - Periodically send only new, changed paths to existing neighbors. This avoids overflooding the network if dynamically send every new path.
 - Upon receiving a neighbor LSA (fromID-it is for sure my neighbor, destID, dist, nextHop, nodesInPath)
    - use it to update my paths but do not immediately send my updated paths out.
 - During the whole process, check if a path to destId is changed since previous periodical sending out. This changed[] will be used in periodical send out.
    - New link or lost link may change some of my path to some destID
    - processing LSA may change some of my path to some destID
- For a new neighbor
    - Immediately share all my valid paths to a new neighbor, either old or new paths. To accelerate the convergence.
*/

//#pragma once
#include "json.hpp"
#include <iostream>
#include <sstream>
#include <sys/time.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <climits>
#include <string>
#include <thread>
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

//using json = nlohmann::json;

typedef tuple<int, int, unordered_set<int>> PATH; // distance, nextHop, nodesInPath

int BUFFERSIZE = 2000;
int myNodeId, mySocketUDP;
int linkCost[256];
unordered_set<int> myNeighbors;
mutex m;
bool changed[256];  // has myPath[i] changed since last time 
map<int, PATH> myPaths; // myPaths[i] means my path to destNode i: (distance, nextHop, nodesInPath)

struct timeval previousHeartbeat[256];      // track last time a neighbor is seen to figure out who is lost
struct sockaddr_in allNodeSocketAddrs[256];
FILE *flog;

void init(int inputId, string costFile, string logFile);
void readCostFile(const char *costFile);

void *announceHeartbeat(void *unusedParam);
void *announceLSA(void *unusedParam);

void listenForNeighbors();
void checkNewNeighbor(int heardFrom);
void checkLostNeighbor();

void sendMyLSAToNeighbors();
void processLSAMessage(char *recvBuf, int bytesRecvd, int heardFrom);
void sendReceivedLSAToOtherNeighbors(char *recvBuf, int bytesRecvd, int neighborId);
void directMessage(char *recvBuf, int bytesRecvd);
int getNextHop(int destId);

void syncSeq(int newNeighborId);
void processSeqShare(string content, int heardFrom);

void setupNodeSockets();
void logMessageAndTime(const char *message);
void logTime();

//void testDij(int MyNodeId, int destId);

void init(int inputId, string costFile, string logFile)
{
    myNodeId = inputId;

    // initailize linkCost and myPaths
    for (int i = 0; i < 256; i += 1)
    {
        previousHeartbeat[i].tv_sec = 0;
        previousHeartbeat[i].tv_usec = 0;

        changed[i] = false;

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

void *announceHeartbeat(void *unusedParam)
{
    // cout << "Inside announceHeartbeat function." << endl;

    struct timespec sleepFor;
    sleepFor.tv_sec = 0;
    sleepFor.tv_nsec = 300 * 1000 * 1000; // 300 ms
    const char *heartBeats = "H";

    while (1)
    {
        for (int i = 0; i < 256; i += 1)
        {
            if (i != myNodeId)
            {
                sendto(mySocketUDP, heartBeats, 2, 0,
                       (struct sockaddr *)&allNodeSocketAddrs[i], sizeof(allNodeSocketAddrs[i]));
            }
        }
        nanosleep(&sleepFor, 0);
    }
}

void *announceLSA(void *unusedParam)
{
    struct timespec sleepFor;
    sleepFor.tv_sec = 0;
    sleepFor.tv_nsec = 500 * 1000 * 1000; // 500 ms

    if (isChanged)
    {
        sendMyLSAToNeighbors();

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
        //memset(recvBuf, 0, sizeof(recvBuf));
        if ((bytesRecvd = recvfrom(mySocketUDP, recvBuf, BUFFERSIZE, 0,
                                   (struct sockaddr *)&theirAddr, &theirAddrLen)) == -1)
        {
            perror("connectivity listener: recvfrom failed");
            exit(1);
        }
        recvBuf[bytesRecvd] = '\0';

        inet_ntop(AF_INET, &theirAddr.sin_addr, fromAddr, 100);

        if (bytesRecvd > BUFFERSIZE)
        {
            string logContent = "Buffer size is not large enough!!!!";
            logMessageAndTime(logContent.c_str());
        }

        short int heardFrom = -1;

        if (strstr(fromAddr, "10.1.1."))
        {   heardFrom = atoi(strchr(strchr(strchr(fromAddr, '.') + 1, '.') + 1, '.') + 1);
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
            processSeqShare(recvBuf, heardFrom);
        }
    }
    close(mySocketUDP);
}

void checkNewNeighbor(int heardFrom)
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

        myNeighbors.insert(heardFrom);

        processSingleLSA(heardFrom, heardFrom, 0, heardFrom, unordered_set<int>{heardFrom}); 
        sharePathsToNewNeighbor(heardFrom);

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

                char buff[200]; 
                snprintf(buff, sizeof(buff), "  Link broken to node %d. The node was previously seen at %ld s, and ld% us.\n   Now the time is ld% s, and ld% us. \n The time difference is ls%.", i, previousHeartbeat[i].tv_sec, previousHeartbeat[i].tv_usec, now.tv_sec, now.tv_usec, timeDifference );
                logMessageAndTime(buff);

                // cout << logContent << endl;

                previousHeartbeat[i].tv_sec = 0;
                myNeighbors.erase(i);

                handleBrokenLink(i);
            }
        }
    }

    sendLSAsToNeighbors();  // if the lost neighbor changed any path, send these updated paths to neighbor

    string logContent = "  Finished checkLostNeighbor.";
    logMessageAndTime(logContent.c_str());
}

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

string generateStrPath(int destId) //
{
    char payload[BUFFERSIZE];
    memset(payload, 0, BUFFERSIZE);
    strcpy(payload, "LSAs");
    int distance = get<0>(myPaths[destId]);
    string strLSA;
    if (distance == -1)
    {
        json LSA = {
            {"fromID", myNodeId},
            {"destID", destId},
            {"dist", -1},
            {"nextHop", -1},
            {"nodesInPath", set<int>{}}};
        strLSA = LSA.dump();
    }
    else
    {
        json LSA = {
            {"fromID", myNodeId},
            {"destID", destId},
            {"dist", distance},
            {"nextHop", get<1>(myPaths[destId])},
            {"nodesInPath", get<2>(myPaths[destId])}};
        strLSA = LSA.dump();
    }
    memcpy(payload + 4, strLSA.c_str(), strLSA.length());
    string payloadStr(payload, payload + 4 + strLSA.length() + 1);
    return payloadStr;
}


// Periodically send out new and valid path updates to existing neighbors
void sendLSAsToNeighbors() 
{
    for (int destId = 0; destId < 256; destId += 1)
    {   
        int myDistance = get<0>(myPaths[destId]);

        for (const auto& nb: myNeighbors) {
            bool isValidPath = get<2>(myPaths[destId]).count(nb) == 0;  // empty path or non-empty path without nb inside

            if (changed[destId] && isValidPath) {
                string path = generateStrPath(destId);
                sendto(mySocketUDP, path.c_str(), path.length(), 0,
                    (struct sockaddr *)&allNodeSocketAddrs[nb], sizeof(allNodeSocketAddrs[nb]));
            }

            m.lock();
            changed[destId] = false;
            m.unlock();
        }
    }

    string logContent = "Sent out my LSA. ";
    logMessageAndTime(logContent.c_str());
    // cout << logContent << endl;
}

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

void sharePathsToNewNeighbor(int newNeighborId) // this will send all valid paths out, except my self path which will be handled by neighbor's dection of me as new neighbor
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

}
