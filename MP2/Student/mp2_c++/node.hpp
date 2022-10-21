
#pragma once
#include <stdio.h>
#include <netdb.h>
//#include <set>
//#include <stdlib.h>
//#include <unistd.h>
//#include <errno.h>
//#include <string.h>
//#include <sys/types.h>
//#include <netinet/in.h>
//#include <sys/socket.h>
//#include <arpa/inet.h>

using namespace std;

class Node
{
public:
    int myNodeId, mySocketUDP;
    struct timeval previousHeartbeat[256];      // track last time a neighbor is seen to figure out who is lost
    struct sockaddr_in allNodeSocketAddrs[256]; //
    // set<int> neighbors;
    int linkCost[256];
    map<int, pair<int, vector<int>>> distanceAndPath; // vector first value is distance, then from the 2nd onwards are the paths.
    // map<int, int> distance;

    Node(int inputId);

    void readCostFile(string costFile);
    void saveLog();
    void sendHeartbeats();
    void announceToNeighbors();
    void broadcastLSA();
    void broadcastMessage(const char *message);
    void forwardMessage(int destNodeId, char *message);
    void processLSAMessage(map<int, pair<int, vector<int>>> otherNodePathVector);
    void listenForNeighbors();
    void updateDistanceAndPath(int fromNodeId, int destNodeId, int fromToDestDistance, vector<int> fromToDestPath);
    void setupNodeSockets();
};