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

typedef tuple<int, int, unordered_set<int>> PATH; // distance, nextHop, nodesInPath

int BUFFERSIZE = 2000;
int myNodeId, mySocketUDP;
int linkCost[256];
unordered_set<int> myNeighbors;
std::mutex change_lock;
std::mutex paths_lock;
bool isChanged[256];      // has myPath[i] changed since last time 
map<int, PATH> myPaths; // myPaths[i] means my path to destNode i: (distance, nextHop, nodesInPath)

struct timeval previousHeartbeat[256];      // track last time a neighbor is seen to figure out who is lost
struct sockaddr_in allNodeSocketAddrs[256];
FILE* flog;

void init(int inputId, string costFile, string logFile);
void readCostFile(const char* costFile);

void* announceHeartbeat(void* unusedParam);
void* announceLSA(void* unusedParam);
void sendSinlgeLSAToNeighbors(int destId);
void sendLSAsToNeighbors();

void listenForNeighbors();
void checkNewNeighbor(int heardFrom);
void checkLostNeighbor();
void handleBrokenLink(int neighborId);

void syncSeq(int newNeighborId);
void processSeqShare(string buffContent, int bytesRecvd, int newNeighborId);
void processSeqShareWrapper(string buffContent, int bytesRecvd, int newNeighborId);


void processLSAMessage(string buffContent, int bytesRecvd, int heardFrom);
void processSingleLSA(int neighborId, int destId, int distance, int nextHop, unordered_set<int> nodesInPath);

void directMessage(string buffContent, int bytesRecvd);
int getNextHop(int destId);

void sharePathsToNewNeighbor(int newNeighborId);

void setupNodeSockets();
void logMessageAndTime(const char* message);
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

        isChanged[i] = false;

        if (i == myNodeId)
        {
            linkCost[i] = 0;
            myPaths[i] = { 0, myNodeId, unordered_set<int>{myNodeId} }; // for example of node 3: (0, (3,  {3})), the path includes the node itself
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

    // cout << "Finished reading cost file" << endl;
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
        nanosleep(&sleepFor, 0);
    }
}

void* announceLSA(void* unusedParam)
{
    struct timespec sleepFor;
    sleepFor.tv_sec = 0;
    sleepFor.tv_nsec = 500 * 1000 * 1000; // 500 ms

    while (1) {

        sendLSAsToNeighbors();

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
            (struct sockaddr*)&theirAddr, &theirAddrLen)) == -1)
        {
            perror("connectivity listener: recvfrom failed");
            exit(1);
        }
        inet_ntop(AF_INET, &theirAddr.sin_addr, fromAddr, 100);

        std::string buffContent;
        buffContent.assign(recvBuf, recvBuf + bytesRecvd);

        short int heardFrom = -1;
        if (strstr(fromAddr, "10.1.1."))
        {
            heardFrom = atoi(strchr(strchr(strchr(fromAddr, '.') + 1, '.') + 1, '.') + 1);
            // string logContent = "Received message(hello or LSA or send) from neighbor node ";
            // logContent += to_string(heardFrom);
            // logMessageAndTime(logContent.c_str());
            // cout << logContent << endl;

            checkNewNeighbor(heardFrom);
            checkLostNeighbor();
        }

        if (!strncmp(recvBuf, "send", 4) || !strncmp(recvBuf, "fowd", 4)) // send/forward message
        {
            directMessage(buffContent, bytesRecvd);
        }
        else if (!strncmp(recvBuf, "LSAs", 4)) // LSA message
        {
            processLSAMessage(buffContent, bytesRecvd, heardFrom);
        }
        else if (!strncmp(recvBuf, "seqs", 4)) // new neighbor share path message
        {
            processSeqShare(buffContent, heardFrom);
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

        unordered_set<int> selfPath{ heardFrom };
        processSingleLSA(heardFrom, heardFrom, 0, heardFrom, selfPath);  // later this will not be needed since neighbor will share this to me
        sharePathsToNewNeighbor(heardFrom);

        logContent = "  Finished processing seeing new neighbor ";
        logContent += to_string(heardFrom);
        logMessageAndTime(logContent.c_str());
        // cout << logContent << endl;
    }
    string logContent = "  Finished processNeighborHeartbeat.";
    logMessageAndTime(logContent.c_str());
}

void syncSeq(int newNeighborId) // share my seq records to new neighbor
{
    char payload[BUFFERSIZE];
    memset(payload, 0, BUFFERSIZE);

    strcpy(payload, "seqs");

    //seq_lock.lock();
    nlohmann::json LSA = {
        {"seqRecord", seqRecord},
    };
    //seq_lock.unlock();

    std::string strLSA = LSA.dump();

    // cout << "strLSA length " << strLSA.length() << endl;
    memcpy(payload + 4, strLSA.c_str(), strLSA.length());

    sendto(mySocketUDP, payload, strLSA.length() + 4, 0,
        (struct sockaddr*)&allNodeSocketAddrs[newNeighborId], sizeof(allNodeSocketAddrs[newNeighborId]));

    std::string logContent = "        Shared my seqRecord to neighbor";
    logContent += std::to_string(newNeighborId);
    logContent += " : ";
    logContent += string(strLSA);
    logMessageAndTime(logContent.c_str());
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

    string logContent = "        Received new neighbor seq list from neighbor  ";
    logContent += to_string(newNeighborId);
    logContent += strSeq;
    logMessageAndTime(logContent.c_str());

    //cout << logContent << endl;
    nlohmann::json LSA = nlohmann::json::parse(strSeq);
    map<int, int> otherSeq = LSA["seqRecord"];

    //graph_lock.lock();
    //map<int, map<int, int>> graphCopy = graph;
    //graph_lock.unlock();

    //seq_lock.lock();
    //map<int, int> seqCopy = seq;
    //seq_lock.unlock();

    char payload[BUFFERSIZE];
    for (int i = 0; i < 256; i += 1)
    {
        if (i != newNeighborId && i != myNodeId && seqRecord[i] > 0 && seqRecord[i] > otherSeq[i]) // do not need to share mine, a new neibhbor is an enent triggering a LSA already
        {
            struct timeval now;
            gettimeofday(&now, 0);

            memset(payload, 0, BUFFERSIZE);
            strcpy(payload, "LSAs");

            nlohmann::json LSA = {
                {"sourceId", i},
                {"seq", seqRecord[i]},
                {"links", graph[i]},
                //{"ttl", (now.tv_sec * 1000000L + now.tv_usec + 500000) / 1000} }; // 500 ms
                {"ttl", 0 } // mark this that it is repacked by me, not from soureId
            };

            std::string strLSA = LSA.dump();

            memcpy(payload + 4, strLSA.c_str(), strLSA.length());

            sendto(mySocketUDP, payload, strLSA.length() + 5, 0,
                (struct sockaddr*)&allNodeSocketAddrs[newNeighborId], sizeof(allNodeSocketAddrs[newNeighborId]));

            std::string logContent = "        Shared my newer path of sourceId ";
            logContent += to_string(i);
            logContent += " to new neighbor ";
            logContent += std::to_string(newNeighborId);
            logContent += " : ";
            logContent += string(strLSA);
            logMessageAndTime(logContent.c_str());
            //  cout << logContent << endl;

        }
    }
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
                snprintf(buff, sizeof(buff), "  Link broken to node %d. The node was previously seen at %ld s, and %ld us.\n   Now the time is %ld s, and %ld us. \n The time difference is %ld.", i, previousHeartbeat[i].tv_sec, previousHeartbeat[i].tv_usec, now.tv_sec, now.tv_usec, timeDifference);
                logMessageAndTime(buff);

                // cout << logContent << endl;

                previousHeartbeat[i].tv_sec = 0;
                myNeighbors.erase(i);

                handleBrokenLink(i);
            }
        }
    }

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

            change_lock.lock();
            isChanged[destId] = true;
            change_lock.unlock();
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
            {"nodesInPath", unordered_set<int>{}}
        };
        strLSA = LSA.dump();
    }
    else
    {
        json LSA = {
            {"fromID", myNodeId},
            {"destID", destId},
            {"dist", distance},
            {"nextHop", get<1>(myPaths[destId])},
            {"nodesInPath", get<2>(myPaths[destId])}
        };
        strLSA = LSA.dump();
    }
    memcpy(payload + 4, strLSA.c_str(), strLSA.length());

    string payloadStr(payload, payload + 4 + strLSA.length() + 1);
    return payloadStr;
}

void processLSAMessage(string buffContent, int bytesRecvd, int heardFrom)
{
    std::string strLSA;
    strLSA.assign(buffContent.c_str() + 4, buffContent.c_str() + bytesRecvd);
    nlohmann::json LSA = nlohmann::json::parse(strLSA);

    int neighborId = LSA["fromID"];
    int destId = LSA["destID"];
    int distance = LSA["dist"];
    int nextHop = LSA["nextHop"];
    unordered_set<int> nodesInPath = LSA["nodesInPath"];

    processSingleLSA(neighborId, destId, distance, nextHop, nodesInPath);
}

void sendSinlgeLSAToNeighbors(int destId)
{
    int myDistance = get<0>(myPaths[destId]);
    if (myDistance != -1 && destId != myNodeId) {
        for (const auto& nb : myNeighbors) {
            bool isValidPath = get<2>(myPaths[destId]).count(nb) == 0;  // empty path or non-empty path without nb inside

            if (isValidPath) {
                string path = generateStrPath(destId);
                sendto(mySocketUDP, path.c_str(), path.length(), 0,
                    (struct sockaddr*)&allNodeSocketAddrs[nb], sizeof(allNodeSocketAddrs[nb]));
            }
        }
    }
}

// Send out all my new and valid path updates to current neighbors
void sendLSAsToNeighbors()
{
    for (int destId = 0; destId < 256; destId += 1)
    {
        if (isChanged[destId]) {
            sendSinlgeLSAToNeighbors(destId);
            change_lock.lock();
            isChanged[destId] = false;
            change_lock.unlock();
        }
    }

    string logContent = "Sent out my updated LSA. ";
    logMessageAndTime(logContent.c_str());
    // cout << logContent << endl;
}

void processSingleLSA(int neighborId, int destId, int distance, int nextHop, unordered_set<int> nodesInPath)
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
        get<2>(myPaths[destId]) = nodesInPath;
        get<2>(myPaths[destId]).insert(myNodeId);

        change_lock.lock();
        isChanged[destId] = true;
        change_lock.unlock();
    }
    else if (distance != -1 && myDistance != -1)
    { // I have a path to dest, and my neighbor has one.
      // compare two paths, if neighbor path is better then update it in myPathToNode[destId]
        int newDistance = distance + linkCost[neighborId];
        if ((myDistance > newDistance) || (myDistance == newDistance && neighborId < myNexthop))
        {

            get<0>(myPaths[destId]) = linkCost[neighborId] + distance;
            get<1>(myPaths[destId]) = neighborId;
            get<2>(myPaths[destId]) = nodesInPath;
            get<2>(myPaths[destId]).insert(myNodeId);

            change_lock.lock();
            isChanged[destId] = true;
            change_lock.unlock();
        }
    }
    else if (distance == -1 && myDistance != -1) // neighbor lost a path to destId
    {
        if (get<2>(myPaths[destId]).count(neighborId) == 1)
        {
            // The neighbor is in my path to destId, then I lost the path to destId
            get<0>(myPaths[destId]) = -1;
            get<2>(myPaths[destId]).clear();

            change_lock.lock();
            isChanged[destId] = true;
            change_lock.unlock();
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
                (struct sockaddr*)&allNodeSocketAddrs[newNeighborId], sizeof(allNodeSocketAddrs[newNeighborId]));

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


void directMessage(string buffContent, int bytesRecvd)
{
    short int destNodeId;
    memcpy(&destNodeId, buffContent.c_str() + 4, 2);
    destNodeId = ntohs(destNodeId);

    // string logContent = "Received send or forward message.";
    // logMessageAndTime(logContent.c_str());
    // cout << logContent << endl;

    char logLine[BUFFERSIZE];

    if (myNodeId == destNodeId)
    {
        sprintf(logLine, "receive packet message %s\n", buffContent.c_str() + 6);
    }
    else
    {
        int nexthop = get<1>(myPaths[destNodeId]);
        if (nexthop != -1)
        {
            if (!strncmp(buffContent.c_str(), "send", 4))
            {
                char fowdMessage[bytesRecvd + 1];
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

    char myAddr[100];
    struct sockaddr_in bindAddr;
    sprintf(myAddr, "10.1.1.%d", myNodeId);
    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(7777);
    inet_pton(AF_INET, myAddr, &bindAddr.sin_addr);
    if (bind(mySocketUDP, (struct sockaddr*)&bindAddr, sizeof(struct sockaddr_in)) < 0)
    {
        perror("bind");
        close(mySocketUDP);
        exit(1);
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
