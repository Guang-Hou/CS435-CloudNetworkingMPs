
#pragma once
#include <stdio.h>
#include <netdb.h>
#include <queue>
#include <set>
//#include <stdlib.h>
//#include <unistd.h>
//#include <errno.h>
//#include <string.h>
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

typedef pair<int, vector<int>> dp; // distance, path pair

class Compare
{
public:
    bool operator()(dp distPath1, dp distPath2)
    {
        return distPath1.first > distPath2.first || (distPath1.first == distPath2.first && distPath1.second[0] > distPath2.second[0]);
    }
};

typedef priority_queue<dp, vector<dp>, Compare> pqOfPaths; // min priority queue of (distance, path) pairs
// typedef priority_queue<dp> pqOfPaths; // min priority queue of (distance, path) pairs

class Node
{
public:
    int myNodeId, mySocketUDP;
    struct timeval previousHeartbeat[256];      // track last time a neighbor is seen to figure out who is lost
    struct sockaddr_in allNodeSocketAddrs[256]; //
    set<int> neighbors;
    int linkCost[256];
    pqOfPaths pathVector[256]; // path vector to all other nodes. for example pathVector[2] stores a priority queue of all paths from this node to node 2
    FILE *fcost, *flog;

    Node(int inputId, string costFile, string logFile);

    void readCostFile();
    void saveLog();
    void sendHeartbeats();
    void announceToNeighbors();
    void broadcastLSA(int fromNodeId, int destNodeId, int distance, const vector<int> *path, int skipId);
    void broadcastMessage(const char *message, int skipId);
    void processLSAMessage(map<int, pair<int, vector<int>>> otherNodePathVector);
    void listenForNeighbors();
    void updateDistanceAndPath(int fromNodeId, int destNodeId, int fromToDestDistance, const vector<int> *fromToDestPath);
    void setupNodeSockets();
    void directMessage(int destNodeId, char *message, int messageByte);
    void sharePathsToNewNeighbor(int newNeighborId);
    string generateStrLSA(int fromNodeId, int destNodeId, int distance, const vector<int> *path);
};