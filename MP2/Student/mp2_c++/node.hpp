
#pragma once

#include <stdio.h>
#include <netdb.h>
#include <map>
#include <set>
#include <string.h>
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

typedef tuple<int, int, set<int>> PATH;

class Node
{
public:
    int myNodeId, mySocketUDP;
    struct timeval previousHeartbeat[256];      // track last time a neighbor is seen to figure out who is lost
    struct sockaddr_in allNodeSocketAddrs[256]; //
    // set<int> neighbors;
    int linkCost[256];
    map<int, map<int, PATH>> myPathRecords; // myPathRecords[i][j] means neighborNode i's best path to destNode j. inside the array each element is the best path (distance, (nextHop, nodesInPath))
    FILE *flog;
    Node(int inputId, string costFile, string logFile);
    void readCostFile(const char *costFile);
    void saveLog();
    void sendHeartbeats();
    // void announceToNeighbors();
    string generateStrPath(int destId);
    void sendPathToNeighbors(int destId);
    void handleNewNeighbor(int neighborId, int destId, int distance, int nextHop, set<int> nodesInPath);
    // void broadcastMessage(const char *message);
    //  void processLSAMessage(map<int, pair<int, vector<int>>> otherNodePathVector);
    void listenForNeighbors();
    // void updatePathsFromNeighbor(int neighborId, string strNeighborPaths);
    void processNeighborLSA(int neighborId, int destId, int distance, int nextHop, set<int> nodesInPath);
    bool isNeighborPathBetter(int neighborId, int destId, int distance, set<int> nodesInPath);
    void useNeighborPath(int neighborId, int destId, int distance, set<int> nodesInPath);
    void directMessage(int destNodeId, string message, int messageByte);
    void selectBestPath(int destId);
    void sharePathsToNewNeighbor(int newNeighborId);
    void handleBrokenLink(int brokenNeighborId);
    void setupNodeSockets();
    void logMessage(const char *message);
    void logPath();
    void logTime();
};