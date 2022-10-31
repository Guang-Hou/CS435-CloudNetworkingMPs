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
#include <mutex>

using json = nlohmann::json;

int BUFFERSIZE = 1000;
mutex m;

void init(int inputId, string costFile, string logFile)
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

    myPathRecords[myNodeId][myNodeId] = {0, myNodeId, set<int>{myNodeId}}; // for example of node 3: (0, (3,  {3})), the path includes the node itself

    setupNodeSockets();
    readCostFile(costFile.c_str());

    flog = fopen(logFile.c_str(), "a");

    string logContent = "Initialization.";
    logMessage(logContent.c_str());
    logTime();
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
}

void sendHeartbeats()
{
    // cout << "Beginning of sending heartbeats." << endl;

    struct timespec sleepFor;
    sleepFor.tv_sec = 0;
    sleepFor.tv_nsec = 300 * 1000 * 1000; // 100 ms

    const char *heartBeats = "HEREIAM";

    while (1)
    {
        for (int i = 0; i < 256; i += 1)
        {
            if (i != myNodeId)
            {
                // string logContent = "Inside broadcastMessage, will broadcast to node ";
                // logContent += to_string(i);
                // logMessage(logContent.c_str());

                sendto(mySocketUDP, heartBeats, 8, 0,
                       (struct sockaddr *)&allNodeSocketAddrs[i], sizeof(allNodeSocketAddrs[i]));
            }
        }
        nanosleep(&sleepFor, 0);
    }
    // cout << "Just sent heartbeats." << endl;
}

string generateStrPath(int destId) //
{
    // cout << "insider generateStrPaths function." << endl;

    char payload[BUFFERSIZE];
    memset(payload, 0, BUFFERSIZE);

    strcpy(payload, "LSAs");

    // cout << "from value: " << from << " length of from: " << sizeof(from) << endl;
    json LSA = {
        {"fromID", myNodeId},
        {"destID", destId},
        {"dist", get<0>(myPathRecords[myNodeId][destId])},
        {"nextHop", get<1>(myPathRecords[myNodeId][destId])},
        {"nodesInPath", get<2>(myPathRecords[myNodeId][destId])}};

    string strLSA = LSA.dump();
    memcpy(payload + 4, strLSA.c_str(), strLSA.length());

    string payloadStr(payload, payload + 4 + strLSA.length() + 1);

    // cout << "end of generateStrPaths function." << endl;

    return payloadStr;
}

void sendPathToNeighbors(int destId)
{
    // cout << "insider broadcastMyPaths function." << endl;
    for (int i = 0; i < 256; i += 1)
    {
        if (i != myNodeId && i != destId && get<0>(myPathRecords[i][i]) != -1 && get<2>(myPathRecords[myNodeId][destId]).count(i) == 0)
        {
            string strLSA = generateStrPath(destId);
            sendto(mySocketUDP, strLSA.c_str(), strLSA.length(), 0,
                   (struct sockaddr *)&allNodeSocketAddrs[i], sizeof(allNodeSocketAddrs[i]));

            string logContent = "Sent out my path update of destId ";
            logContent += to_string(destId);
            logContent += " to neighbor ";
            logContent += to_string(i);
            logContent += " : \n";
            logContent += strLSA;
            logMessage(logContent.c_str());
            logTime();
            //  cout << "end of broadcastMyPaths function." << endl;
        }
    }
}

void checkNewAndLostNeighbor(int heardFrom)
{
    struct timeval now;
    gettimeofday(&now, 0);

    long usecondDifference = (now.tv_sec - previousHeartbeat[heardFrom].tv_sec) * 1000000L + now.tv_usec - previousHeartbeat[heardFrom].tv_usec;

    if (previousHeartbeat[heardFrom].tv_sec == 0 || usecondDifference > 600000) // timeout 0.6 second
    {
        string logContent = "Saw new neighbor ";
        logContent += to_string(heardFrom);
        logMessage(logContent.c_str());
        logTime();

        // string timeStr = "Time difference in usecond: ";
        // timeStr += to_string(usecondDifference);
        // logMessage(timeStr.c_str());

        processSingleLSA(heardFrom, heardFrom, 0, heardFrom, set<int>{heardFrom});
        sharePathsToNewNeighbor(heardFrom);
    }

    //  record that we heard from heardFrom just now.
    gettimeofday(&previousHeartbeat[heardFrom], 0);

    // check if there is any neighbor link is broken, if so update  pathRecords and broadcast LSA
    for (int i = 0; i < 256; i += 1)
    {
        if (i != myNodeId)
        {
            long timeDifference = (now.tv_sec - previousHeartbeat[i].tv_sec) * 1000000L + now.tv_usec - previousHeartbeat[i].tv_usec;
            if (previousHeartbeat[i].tv_sec != 0 && timeDifference > 600000) // missed two hearbeats
            {
                if (get<0>(myPathRecords[i][i]) != -1)
                {
                    string logContent = "Link broken to node: ";
                    logContent += to_string(i);
                    logMessage(logContent.c_str());
                    logTime();
                    handleBrokenLink(i);
                }
            }
        }
    }
}

void sendOrFowdMessage(string buffContent, int bytesRecvd)
{
    const char *recvBuf = buffContent.c_str();

    short int destNodeId;
    memcpy(&destNodeId, recvBuf + 4, 2);
    destNodeId = ntohs(destNodeId);

    // string content;
    // content.assign(recvBuf, bytesRecvd + 1);

    string logContent = "Received send or forward message.";
    logMessage(logContent.c_str());
    logTime();

    directMessage(destNodeId, buffContent, bytesRecvd + 1);
}

void processLSAMessage(string buffContent)
{
    // const char *recvBuf = buffContent.c_str();
    //  cout << "Received LSAs message." << endl;
    string strLSA;
    strLSA.assign(buffContent.begin() + 4, buffContent.end() - 0);
    json LSA = json::parse(strLSA);

    int neighborId = LSA["fromID"];
    int destId = LSA["destID"];
    int distance = LSA["dist"];
    int nextHop = LSA["nextHop"];
    set<int> nodesInPath = LSA["nodesInPath"];

    string logContent = "Received path : \n";
    logContent += strLSA;
    logMessage(logContent.c_str());
    logTime();

    processSingleLSA(neighborId, destId, distance, nextHop, nodesInPath);
    logContent = "Finished processing path.";
    logMessage(logContent.c_str());
    logTime();
}

void processSingleLSA(int neighborId, int destId, int distance, int nextHop, set<int> nodesInPath)
{
    // string logContent = "Inside processSigleLSA function.";
    // logMessage(logContent.c_str());

    m.lock();
    myPathRecords[neighborId][destId] = {distance, nextHop, nodesInPath};
    m.unlock();

    // if neighbor LSA affects my path to destId
    bool containsMyNodeId = nodesInPath.count(myNodeId) != 0;
    int myDistance = get<0>(myPathRecords[myNodeId][destId]);
    int myNexthop = get<1>(myPathRecords[myNodeId][destId]);

    if ((destId == myNodeId) || containsMyNodeId)
    {
        return;
    }

    if (distance != -1 && myDistance == -1)
    { // I do not have path to dest, but my neighbor has
      // use this neighbor path to destId

        m.lock();
        myPathRecords[myNodeId][destId] = {linkCost[neighborId] + distance, neighborId, nodesInPath};
        get<2>(myPathRecords[myNodeId][destId]).insert(myNodeId);
        m.unlock();
        sendPathToNeighbors(destId);
    }
    else if (distance != -1 && myDistance != -1)
    { // I have path to dest, and my neighbor has
      // compare two paths, if neighbor path is better then update it in myPathToNode[destId]
        int newDistance = distance + linkCost[neighborId];
        if ((myDistance > newDistance) || (myDistance == newDistance && neighborId < myNexthop))
        {
            m.lock();
            myPathRecords[myNodeId][destId] = {linkCost[neighborId] + distance, neighborId, nodesInPath};
            get<2>(myPathRecords[myNodeId][destId]).insert(myNodeId);
            m.unlock();
            sendPathToNeighbors(destId);
        }
    }
    else if (distance == -1 && myDistance != -1 && get<1>(myPathRecords[myNodeId][destId]) == neighborId)
    { // neighbor lost a path to destId, and I am using that neighbor as next hop, then we need a new neighbor as nextHop
        handleLostPath(neighborId, destId);
    }
}

void sharePathsToNewNeighbor(int newNeighborId)
{
    for (int destId = 0; destId < 256; destId += 1)
    {
        if (destId != myNodeId && destId != newNeighborId && get<0>(myPathRecords[myNodeId][destId]) != -1 && get<2>(myPathRecords[myNodeId][destId]).count(newNeighborId) == 0)
        {
            string strLSA = generateStrPath(destId);
            sendto(mySocketUDP, strLSA.c_str(), strLSA.length(), 0,
                   (struct sockaddr *)&allNodeSocketAddrs[newNeighborId], sizeof(allNodeSocketAddrs[newNeighborId]));

            string logContent = "Shared my path of destId ";
            logContent += to_string(destId);
            logContent += " to new neighbor ";
            logContent += to_string(newNeighborId);
            logContent += " : \n";
            logContent += string(strLSA);
            logMessage(logContent.c_str());
            logTime();
            //  cout << "end of broadcastMyPaths function." << endl;
        }
    }
}

// The neighborId lost path to destId and I am using this neighbor as my next Hop to reach destId.
// Basically we need to find a new neighbor as nextHop for destId, the path could not contain this neighbor.
// This can handle the special case that neighborId == myNodeId, i.e. I lost my path to desId
void handleLostPath(int neighborId, int destId)
{
    int shortestDistance = INT_MAX;
    int bestNeighborId = -1;

    for (int node = 0; node < 256; node += 1)
    {   // check if node is a neighbor
        if (node != myNodeId && node != neighborId && get<0>(myPathRecords[node][node]) != -1)
        {   
            int nodeDistanceToDest = get<0>(myPathRecords[node][destId]);
            int myDistanceToNode = linkCost[node];  // assume we go through this neighbor
            int totalDistance = myDistanceToNode + nodeDistanceToDest;

            if (nodeDistanceToDest != -1 && get<2>(myPathRecords[node][destId]).count(neighborId) == 0 
                && get<2>(myPathRecords[node][destId]).count(myNodeId) == 0 ) // node -> destId could not go through me, otherwise there is a better neighbor
            {
                if ((totalDistance < shortestDistance) || (totalDistance == shortestDistance && node < bestNeighborId))
                {
                    shortestDistance = totalDistance;
                    bestNeighborId = node;
                }
            }
        }
    }

    if (bestNeighborId != -1)
    {
        // usePath(bestNeighborId, destId, get<0>(myPathRecords[bestNeighborId][destId]), get<2>(myPathRecords[bestNeighborId][destId]));
        m.lock();
        get<0>(myPathRecords[myNodeId][destId]) = get<0>(myPathRecords[myNodeId][bestNeighborId]) + get<0>(myPathRecords[bestNeighborId][destId]);
        get<1>(myPathRecords[myNodeId][destId]) = bestNeighborId;
        get<2>(myPathRecords[myNodeId][destId]) = get<2>(myPathRecords[myNodeId][bestNeighborId]);
        get<2>(myPathRecords[myNodeId][destId]).insert(get<2>(myPathRecords[bestNeighborId][destId]).begin(), get<2>(myPathRecords[bestNeighborId][destId]).end());
        m.unlock();
        string logContent = "For brokenLink selection to desId ";
        logContent += to_string(destId);
        logContent += ", selected nextHop to be neighbor node ";
        logContent += to_string(bestNeighborId);
        logMessage(logContent.c_str());
        logTime();
    }
    else
    {
        m.lock();
        get<0>(myPathRecords[myNodeId][destId]) = -1;
        m.unlock();

        string logContent = "For brokenLink selection to desId ";
        logContent += to_string(destId);
        logContent += "For brokenLink selection, No alternative middle node, will broadcast -1 path to my neighbors.";
        logMessage(logContent.c_str());
        logTime();
    }

    sendPathToNeighbors(destId);
}

void handleBrokenLink(int brokenNeighborId)
{
    m.lock();
    get<0>(myPathRecords[brokenNeighborId][brokenNeighborId]) = -1;
    m.unlock();

    for (int destId = 0; destId < 256; destId += 1)
    {
        if (destId != myNodeId && get<0>(myPathRecords[myNodeId][destId]) != -1 && get<1>(myPathRecords[myNodeId][destId]) == brokenNeighborId)
        {
            handleLostPath(myNodeId, destId);
        }
    }
}

void logMessage(const char *message)
{

    char logLine[BUFFERSIZE];
    sprintf(logLine, "Log: %s\n", message);
    fwrite(logLine, 1, strlen(logLine), flog);
    fflush(flog);
}

void logTime()
{

    struct timeval now;
    gettimeofday(&now, 0);

    char logLine[100];
    sprintf(logLine, "Processed at <%ld.%06ld>\n", (long int)(now.tv_sec), (long int)(now.tv_usec));
    fwrite(logLine, 1, strlen(logLine), flog);
    fflush(flog);
}

void logPath()
{
    char logLine[BUFFERSIZE];
    json j_map(myPathRecords[myNodeId]);
    string path = j_map.dump();
    sprintf(logLine, "%s\n", path.c_str());
    fwrite(logLine, 1, strlen(logLine), flog);
    fflush(flog);
}

void directMessage(int destNodeId, string message, int messageByte)
{
    char logLine[BUFFERSIZE];

    if (myNodeId == destNodeId)
    {
        sprintf(logLine, "receive packet message %s\n", message.c_str() + 6);
    }
    else
    {
        // std::cout << "Entered directMessage function." << std::endl;
        int distance = get<0>(myPathRecords[myNodeId][destNodeId]);
        if (distance != -1)
        {
            int nexthop = get<1>(myPathRecords[myNodeId][destNodeId]);

            if (!strncmp(message.c_str(), "send", 4))
            {
                char fowdMessage[messageByte];
                strcpy(fowdMessage, "fowd");
                memcpy(fowdMessage + 4, message.c_str() + 4, messageByte - 4);

                // string rmessage(message + 6);
                //  std::cout << "Message received: " << (message + 6) << std::endl;

                sendto(mySocketUDP, fowdMessage, messageByte, 0,
                       (struct sockaddr *)&allNodeSocketAddrs[nexthop], sizeof(allNodeSocketAddrs[nexthop]));

                sprintf(logLine, "sending packet dest %d nexthop %d message %s\n", destNodeId, nexthop, message.c_str() + 6);
            }
            else if (!strncmp(message.c_str(), "fowd", 4))
            {
                sendto(mySocketUDP, message.c_str(), messageByte, 0,
                       (struct sockaddr *)&allNodeSocketAddrs[nexthop], sizeof(allNodeSocketAddrs[nexthop]));

                sprintf(logLine, "forward packet dest %d nexthop %d message %s\n", destNodeId, nexthop, message.c_str() + 6);
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
}
