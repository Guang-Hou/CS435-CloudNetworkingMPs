
#pragma once

#include <stdio.h>
#include <netdb.h>
#include <map>
#include <unordered_set>
#include <string.h>
#include <atomic>
//#include <stdlib.h>
//#include <unistd.h>
//#include <errno.h>
//#include <sys/types.h>
//#include <netinet/in.h>
//#include <sys/socket.h>
//#include <arpa/inet.h>

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

using namespace std;

typedef tuple<int, int, unordered_set<int>> PATH; // distance, nextHop, nodesInPath

int myNodeId, mySocketUDP;
int linkCost[256];
unordered_set<int> myNeighbors;
map<int, PATH> myPaths; // myPaths[i] means my path to destNode i: (distance, nextHop, nodesInPath)

struct timeval previousHeartbeat[256]; // track last time a neighbor is seen to figure out who is lost
struct sockaddr_in allNodeSocketAddrs[256];
FILE *flog;

void init(int inputId, string costFile, string logFile);
void readCostFile(const char *costFile);

void broadcastLSA();
void checkNewNeighbor(int heardFrom);
void checkLostNeighbor();
void handleBrokenLink(int neighborId);

// void sharePathsToNewNeighbor(int newNeighborId);
// void sendPathToNeighbors(int destId);
string generateStrPaths();

void processLSAMessage(string buffContent, int neighborId);
void processSingleLSA(int neighborId, int destId, PATH neighborPath);

void sendOrFowdMessage(string recvBuf, int bytesRecvd);
void directMessage(int destNodeId, string message, int messageByte);

void setupNodeSockets();
void logMessageAndTime(const char *message);
void logTime();
void logMyPaths();