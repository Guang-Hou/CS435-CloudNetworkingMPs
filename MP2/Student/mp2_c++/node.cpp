#include "json.hpp"
#include "node.hpp"
#include <iostream>
#include <sstream>
#include <stdio.h>
#include <netdb.h>
#include <sys/time.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <vector>
#include <climits>
#include <string>
//#include <fstream>
//#include <queue>
//#include <errno.h>
//#include <sys/types.h>
//#include <netinet/in.h>
//#include <sys/socket.h>

using json = nlohmann::json;

int BUFFERSIZE = 5000;

Node::Node(int inputId, string costFile, string logFile)
{
    myNodeId = inputId;

    // initailize linkCost and paths
    for (int i = 0; i < 256; i += 1)
    {
        previousHeartbeat[i].tv_sec = 0;
        previousHeartbeat[i].tv_usec = 0;

        if (i == myNodeId)
        {

            linkCost[i] = 0;
        }
        else
        {
            linkCost[i] = 1;
        }

        for (int destId = 0; destId < 256; destId += 1)
        {
            get<0>(myPathRecords[i][destId]) = -1; // at beginning in my record, all my neighbors have no path, and I do not have path to them
        }
    }

    get<0>(myPathRecords[myNodeId][myNodeId]) = 0;
    get<1>(myPathRecords[myNodeId][myNodeId]) = myNodeId;
    get<2>(myPathRecords[myNodeId][myNodeId]).insert(myNodeId); // for example of node 3: (0, (3,  {3})), the path includes the node itself

    setupNodeSockets();
    readCostFile(costFile);

    flog = fopen(logFile.c_str(), "a");

    // cout << "End of constructor." << endl;
};

void Node::readCostFile(string costFile)
{
    FILE *fcost = fopen(costFile.c_str(), "r");

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
}

void Node::sendHeartbeats()
{
    // cout << "Beginning of sending heartbeats." << endl;

    struct timespec sleepFor;
    sleepFor.tv_sec = 0;
    sleepFor.tv_nsec = 300 * 1000 * 1000; // 300 ms

    const char *heartBeats = "HEREIAM";

    while (1)
    {
        broadcastMessage(heartBeats);
        nanosleep(&sleepFor, 0);
    }

    // cout << "Just sent heartbeats." << endl;
}

void Node::broadcastMessage(const char *message)
{
    // cout << "insider broadcastMessage function." << endl;
    for (int i = 0; i < 256; i += 1)
    {
        if (i != myNodeId)
        {
            sendto(mySocketUDP, message, BUFFERSIZE, 0,
                   (struct sockaddr *)&allNodeSocketAddrs[i], sizeof(allNodeSocketAddrs[i]));
        }
    }
    // cout << "End of broadcastMessage." << endl;
}

/**
 * @brief Convert my path to destId into string, append LSAs at the biginning.
 * Make sure there is a path to destId befor calling this function.
 *
 * @param destId
 * @return string
 */
string Node::generateStrPath(int destId) //
{
    // cout << "insider generateStrPaths function." << endl;

    char payload[BUFFERSIZE];
    memset(payload, 0, BUFFERSIZE);

    strcpy(payload, "LSAs");

    json LSA = {
        {"fromID", myNodeId},
        {"destID", destId},
        {"dist", get<0>(myPathRecords[myNodeId][destId])},
        {"nextHop", get<1>(myPathRecords[myNodeId][destId])},
        {"nodesInPath", get<2>(myPathRecords[myNodeId][destId])}};

    string strLSA = LSA.dump();

    // cout << "pathStr length: " << pathStr.length() << endl;
    // cout << "pathStr: " << pathStr << endl;

    memcpy(payload + 4, strLSA.c_str(), strLSA.length());
    // int byteSize = 16 + sPath.length() + 1;
    //  payload[16 + sPath.length()] = 0;

    // string content(payload + 16);

    // cout << "The path in payload: " << content << endl;

    // json j_vector = json::parse(content);
    // auto path = j_vector.get<vector<int>>();
    //  string outStr;
    //  outStr.assign(payload + 16, payload + 32);
    //  cout << "Node " << myNodeId << " is broadcasting LSA Message: " << outStr << endl;

    // cout << "Length: " << 8 + pathStr.length() << endl;
    string payloadStr(payload, payload + 4 + strLSA.length() + 1);

    // cout << "end of generateStrPaths function." << endl;

    return payloadStr;
}

void Node::broadcastPath(int destId)
{
    // cout << "insider broadcastMyPaths function." << endl;
    string strLSA = generateStrPath(destId);
    broadcastMessage(strLSA.c_str());
    // cout << "end of broadcastMyPaths function." << endl;
}

void Node::sharePathsToNewNeighbor(int newNeighborId)
{
    for (int destId = 0; destId < 256; destId += 1)
    {
        int distance = get<0>(myPathRecords[myNodeId][destId]);
        if (destId != newNeighborId && distance != -1)
        {
            string strLSA = generateStrPath(destId);
            sendto(mySocketUDP, strLSA.c_str(), BUFFERSIZE, 0,
                   (struct sockaddr *)&allNodeSocketAddrs[newNeighborId], sizeof(allNodeSocketAddrs[newNeighborId]));
        }
    }
}

void Node::listenForNeighbors()
{

    // cout << "Beginning of listenForNeighbors." << endl;

    char fromAddr[100];
    struct sockaddr_in theirAddr;
    socklen_t theirAddrLen = sizeof(theirAddr);
    char recvBuf[BUFFERSIZE];
    int bytesRecvd;

    while (1)
    {
        memset(recvBuf, 0, sizeof(recvBuf));
        if ((bytesRecvd = recvfrom(mySocketUDP, recvBuf, BUFFERSIZE, 0,
                                   (struct sockaddr *)&theirAddr, &theirAddrLen)) == -1)
        {
            perror("connectivity listener: recvfrom failed");
            exit(1);
        }

        inet_ntop(AF_INET, &theirAddr.sin_addr, fromAddr, 100);

        // string s(recvBuf);
        //  std::cout << "Received number of bytes: " << bytesRecvd << std::endl;

        struct timeval now;
        gettimeofday(&now, 0);
        // printf("Now in seconds : %ld\tmicro seconds : %ld\n", now.tv_sec, now.tv_usec);

        // heartbeat message to identify new neighbors
        short int heardFrom = -1;
        if (strstr(fromAddr, "10.1.1."))
        {
            heardFrom = atoi(
                strchr(strchr(strchr(fromAddr, '.') + 1, '.') + 1, '.') + 1);

            // std::cout << "Received from Node: " << heardFrom << std::endl;

            // TODO: this node can consider heardFrom to be directly connected to it; do any such logic now.
            // printf("HeardFrom Node previous heartbeat in seconds : %ld\tmicro seconds : %ld\n", previousHeartbeat[heardFrom].tv_sec, previousHeartbeat[heardFrom].tv_usec);
            // long timeGap = (now.tv_sec - previousHeartbeat[heardFrom].tv_sec) * 1000000L + now.tv_usec - previousHeartbeat[heardFrom].tv_usec;
            // printf("Time gap from last time seen this node: %ld\n", timeGap);

            if (previousHeartbeat[heardFrom].tv_sec == 0 ||
                ((now.tv_sec - previousHeartbeat[heardFrom].tv_sec) * 1000000L + now.tv_usec - previousHeartbeat[heardFrom].tv_usec) > 600000) // timeout 0.6 second
            {
                // dp distPath = pair<0, fromToDestPath>;
                // updateDistanceAndPath(heardFrom, heardFrom, 0, &fromToDestPath);
                // myNeighbors.add(heardFrom)
                // broadcastLSA();
                // cout << "After updating, is the path empty? " << pathVector[heardFrom].empty() << endl;
                // cout << "Latest myPathToNode: " << pathStr << endl;

                processNeighborLSA(heardFrom, heardFrom, 0, heardFrom, set<int>{heardFrom});

                sharePathsToNewNeighbor(heardFrom);
            }

            // neighbors.insert(heardFrom);
            //  record that we heard from heardFrom just now.
            gettimeofday(&previousHeartbeat[heardFrom], 0);
        }

        // check if there is any neighbor link is broken, if so update myPathToNode, pathRecords and broadcast LSA
        for (int i = 0; i < 256; i += 1)
        {
            if (i != myNodeId)
            {
                if (previousHeartbeat[i].tv_sec != 0 &&
                    ((now.tv_sec - previousHeartbeat[i].tv_sec) * 1000000L + now.tv_usec - previousHeartbeat[i].tv_usec) > 600000) // missed two hearbeats
                {
                    // updatePathsFromNeighbor(i, NULL);
                    //  std::cout << "Link broken to node: " << i << std::endl;
                    get<0>(myPathRecords[i][i]) = -1;
                    handleBrokenLink(i);
                }
            }
        }

        if (!strncmp(recvBuf, "send", 4) || !strncmp(recvBuf, "fowd", 4)) // send message
        {
            short int destNodeId;
            memcpy(&destNodeId, recvBuf + 4, 2);
            destNodeId = ntohs(destNodeId);
            recvBuf[bytesRecvd] = 0;

            // std::cout << "Dest Node ID: " << destNodeId << std::endl;

            // char message[100];
            // strcpy(message, "fowd");
            // memcpy(message + 4, recvBuf + 4, bytesRecvd - 3);

            // string rmessage(message + 6);
            // std::cout << "Message received: " << (message + 6) << std::endl;

            directMessage(destNodeId, recvBuf, bytesRecvd + 1);
        }
        else if (!strncmp(recvBuf, "LSAs", 4)) // LSA message
        {
            // cout << "Received LSAs message." << endl;
            //  broadcastLSA(int fromNodeId, int destNodeId, dp distancePathPair)
            string strLSA(recvBuf + 4);
            json LSA = json::parse(strLSA);

            int neighborId = LSA["fromID"];
            int destId = LSA["destID"];
            int distance = LSA["dist"];
            int nextHop = LSA["nextHop"];
            set<int> nodesInPath = LSA["nodesInPath"];

            // std::cout << "fromId: " << from << " destId: " << dest << " dist: " << dist << std::endl;
            //  std::cout << "message_type: " << message_type << std::endl;
            //  std::cout << "contentLength: " << contentLength << std::endl;
            // std::cout << "content of received LSA message: " << content << std::endl;

            // json j_array = json::parse(content);
            // auto neighborPaths = j_array.get<pair<int, pair<int, set<int>>>>();

            processNeighborLSA(neighborId, destId, distance, nextHop, nodesInPath);
        }
    }

    //(should never reach here)
    close(mySocketUDP);
}

void Node::processNeighborLSA(int neighborId, int destId, int distance, int nextHop, set<int> nodesInPath)
{
    /*
    json LSA = {
        {"fromID", neighborId},
        {"destID", destId},
        {"dist", distance},
        {"nextHop", nextHop},
        {"nodesInPath", nodesInPath}};

    string strLSA = LSA.dump();

    cout << "Received LSA: " << endl;
    cout << strLSA << endl;
    */

    // update this neighbor and LSA in myPathRecords
    get<0>(myPathRecords[neighborId][destId]) = distance;
    get<1>(myPathRecords[neighborId][destId]) = nextHop;
    get<2>(myPathRecords[neighborId][destId]) = nodesInPath;
    // get<0>(myPathRecords[myNodeId][neighborId]) = linkCost[neighborId];

    // if neighbor LSA affects my path to destId
    bool containsMyNodeId = (nodesInPath.count(myNodeId)) != 0;
    int myDistance = get<0>(myPathRecords[myNodeId][destId]);
    int myNexthop = get<1>(myPathRecords[myNodeId][destId]);

    if ((destId == myNodeId) || containsMyNodeId)
    {
        return;
    }

    if (distance != -1 && myDistance == -1)
    { // I do not have path to dest, but my neighbor has
      // use this neighbor path to destId
        // usePath(neighborId, destId, distance, nodesInPath);
        get<0>(myPathRecords[myNodeId][destId]) = linkCost[neighborId];
        get<1>(myPathRecords[myNodeId][destId]) = neighborId;
        get<2>(myPathRecords[myNodeId][destId]) = set<int>{myNodeId, neighborId};
        broadcastPath(destId);
    }
    else if (distance != -1 && myDistance != -1)
    { // I have path to dest, and my neighbor has
      // compare two paths, if neighbor path is better then update it in myPathToNode[destId]
        int newDistance = distance + linkCost[neighborId];
        if ((myDistance > newDistance) || ((myDistance == newDistance) && (neighborId < myNexthop)))
        {
            get<0>(myPathRecords[myNodeId][destId]) = linkCost[neighborId] + distance;
            get<1>(myPathRecords[myNodeId][destId]) = neighborId;
            get<2>(myPathRecords[myNodeId][destId]) = nodesInPath;
            get<2>(myPathRecords[myNodeId][destId]).insert(myNodeId);
            broadcastPath(destId);
        }
    }
    /*

    json mLSA = {
        {"fromID", myNodeId},
        {"destID", destId},
        {"dist", get<0>(myPathRecords[myNodeId][destId])},
        {"nextHop", get<1>(myPathRecords[myNodeId][destId])},
        {"nodesInPath", get<2>(myPathRecords[myNodeId][destId])}};

    string strmLSA = mLSA.dump();

    cout << "After receiving LSA, my LSA: " << endl;
    cout << strmLSA << endl;
    */
}

void Node::selectBestPath(int destId)
{
    int smallestDistance = INT_MAX;
    int bestMiddleNodeId = -1;
    int bestNextHop = -1;

    for (int middleNode = 0; middleNode < 256; middleNode += 1)
    {
        if (middleNode != myNodeId)
        {
            int neighborDistanceToDest = get<0>(myPathRecords[middleNode][destId]);
            int myDistanceToNeighbor = get<0>(myPathRecords[myNodeId][destId]);
            int nextHop = get<1>(myPathRecords[myNodeId][destId]);
            int totalDistance = myDistanceToNeighbor + neighborDistanceToDest;

            if (neighborDistanceToDest != -1 && myDistanceToNeighbor != -1)
            {
                if ((totalDistance < smallestDistance) || (totalDistance == smallestDistance && nextHop < bestNextHop))
                {
                    smallestDistance = totalDistance;
                    bestMiddleNodeId = middleNode;
                    bestNextHop = nextHop;
                }
            }
        }
    }

    if (bestMiddleNodeId != -1)
    {
        // usePath(bestNeighborId, destId, get<0>(myPathRecords[bestNeighborId][destId]), get<2>(myPathRecords[bestNeighborId][destId]));
        get<0>(myPathRecords[myNodeId][destId]) = get<0>(myPathRecords[myNodeId][bestMiddleNodeId]) + get<0>(myPathRecords[bestMiddleNodeId][destId]);
        get<1>(myPathRecords[myNodeId][destId]) = get<1>(myPathRecords[myNodeId][bestMiddleNodeId]);
        get<2>(myPathRecords[myNodeId][destId]) = get<2>(myPathRecords[myNodeId][bestMiddleNodeId]);
        get<2>(myPathRecords[myNodeId][destId]).insert(get<2>(myPathRecords[bestMiddleNodeId][destId]).begin(), get<2>(myPathRecords[bestMiddleNodeId][destId]).end());
    }
    else
    {
        get<0>(myPathRecords[myNodeId][destId]) = -1;
    }

    broadcastPath(destId);
}

void Node::handleBrokenLink(int brokenNeighborId)
{
    for (int destId = 0; destId < 256; destId += 1)
    {
        if (get<1>(myPathRecords[myNodeId][destId]) == brokenNeighborId)
        {
            selectBestPath(destId);
        }
    }
}

void Node::directMessage(int destNodeId, char *message, int messageByte)
{
    char logLine[BUFFERSIZE];

    if (myNodeId == destNodeId)
    {
        sprintf(logLine, "receive packet message %s\n", message + 6);
    }
    else
    {
        // std::cout << "Entered directMessage function." << std::endl;
        int distance = get<0>(myPathRecords[myNodeId][destNodeId]);
        if (distance != -1)
        {
            int nexthop = get<1>(myPathRecords[myNodeId][destNodeId]);

            if (!strncmp(message, "send", 4))
            {
                char fowdMessage[messageByte];
                strcpy(fowdMessage, "fowd");
                memcpy(fowdMessage + 4, message + 4, messageByte - 4);

                // string rmessage(message + 6);
                //  std::cout << "Message received: " << (message + 6) << std::endl;

                sendto(mySocketUDP, fowdMessage, messageByte, 0,
                       (struct sockaddr *)&allNodeSocketAddrs[nexthop], sizeof(allNodeSocketAddrs[nexthop]));

                sprintf(logLine, "sending packet dest %d nexthop %d message %s\n", destNodeId, nexthop, message + 6);
            }
            else if (!strncmp(message, "fowd", 4))
            {
                sendto(mySocketUDP, message, messageByte, 0,
                       (struct sockaddr *)&allNodeSocketAddrs[nexthop], sizeof(allNodeSocketAddrs[nexthop]));

                sprintf(logLine, "forward packet dest %d nexthop %d message %s\n", destNodeId, nexthop, message + 6);
            }
        }
        else
        {
            sprintf(logLine, "unreachable dest %d\n", destNodeId);
            // std::cout << "logLine: " << logLine << std::endl;
        }
    }

    fwrite(logLine, 1, strlen(logLine), flog);
    // std::cout << "Bytes written to the log file: " << bytesWritten << std::endl;
    fflush(flog);
}

void Node::setupNodeSockets()
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
}
