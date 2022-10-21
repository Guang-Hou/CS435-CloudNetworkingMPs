#include "json.hpp"
#include "node.hpp"
#include <iostream>
#include <string>
#include <sstream>
#include <fstream>
#include <stdio.h>
#include <netdb.h>
#include <sys/time.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <vector>
//#include <errno.h>
//#include <sys/types.h>
//#include <netinet/in.h>
//#include <sys/socket.h>

using json = nlohmann::json;

int BUFFERSIZE = 5000;

Node::Node(int inputId)
{
    myNodeId = inputId;
    // initailize linkCost and distanceAndPath
    for (int i = 0; i < 256; i += 1)
    {
        previousHeartbeat[i] = (struct timeval){0};
        if (i == myNodeId)
        {
            distanceAndPath[i].first = 0;
            distanceAndPath[i].second.push_back(i);
            linkCost[i] = 0;
        }
        else
        {
            distanceAndPath[i].first = -1;
            linkCost[i] = 1;
        }
    }

    distanceAndPath[256].first = myNodeId; // We are not storing the node 256's distance,
                                           // instead this stores this node's ID for anouncing to other nodes

    setupNodeSockets();
};

void Node::readCostFile(string costFile)
{
    fstream f;
    f.open(costFile, ios::in);
    if (f.is_open()) // checking whether the file is open
    {

        if (f.peek() == std::ifstream::traits_type::eof())
        {
            return;
        }

        string lineContent;

        while (getline(f, lineContent))
        {
            std::cout << lineContent << std::endl;
            int destNode, cost;
            stringstream ss(lineContent);
            ss >> destNode;
            ss >> cost;
            this->linkCost[destNode] = cost;
        }
        f.close(); // close the file object.
    }

    /*
        for (auto const &value : this->linkCost)
        {
            std::cout << value << ",";
        }
        std::cout << "\n";
        */
}

void Node::saveLog()
{
}

void Node::sendHeartbeats()
{
    struct timespec sleepFor;
    sleepFor.tv_sec = 0;
    sleepFor.tv_nsec = 300 * 1000 * 1000; // 300 ms

    char *heartBeats = "HEREIAM";

    while (1)
    {
        broadcastMessage(heartBeats);
        nanosleep(&sleepFor, 0);
    }
}

void Node::broadcastMessage(const char *message)
{
    for (int i = 0; i < 256; i += 1)
    {
        if (i != myNodeId)
        {
            sendto(mySocketUDP, payload, sizeof(payload), 0,
                   (struct sockaddr *)&allNodeSocketAddrs[i], sizeof(allNodeSocketAddrs[i]));
        }
    }
}

void Node::broadcastLSA()
{
    json j_map(distanceAndPath);
    string strContent = j_map.dump();

    char payload[BUFFERSIZE];

    strcpy(payload, "LSAs");

    uint32_t contentLength = strContent.length();
    memcpy(&payload[4], &contentLength, sizeof(contentLength));

    memcpy(&payload[4 + sizeof(contentLength)], strContent.c_str(), strContent.length() + 1);

    broadcastMessage(payload);
}

void Node::listenForNeighbors()
{
    char fromAddr[100];
    struct sockaddr_in theirAddr;
    socklen_t theirAddrLen = sizeof(theirAddr);
    char recvBuf[BUFFERSIZE];
    int bytesRecvd;

    while (1)
    {
        if ((bytesRecvd = recvfrom(mySocketUDP, recvBuf, BUFFERSIZE, 0,
                                   (struct sockaddr *)&theirAddr, &theirAddrLen)) == -1)
        {
            perror("connectivity listener: recvfrom failed");
            exit(1);
        }

        inet_ntop(AF_INET, &theirAddr.sin_addr, fromAddr, 100);

        string s(recvBuf);
        std::cout << "Received number of bytes: " << bytesRecvd << std::endl;
        std::cout << "Content received: " << s << std::endl;

        // heartbeat message to identify new neighbors
        short int heardFrom = -1;
        if (strstr(fromAddr, "10.1.1."))
        {
            heardFrom = atoi(
                strchr(strchr(strchr(fromAddr, '.') + 1, '.') + 1, '.') + 1);

            // TODO: this node can consider heardFrom to be directly connected to it; do any such logic now.
            struct timeval now;
            gettimeofday(&now, 0);

            if (previousHeartbeat[heardFrom].tv_sec == 0 ||
                ((now.tv_sec - previousHeartbeat[heardFrom].tv_sec) * 1000000L + now.tv_usec - previousHeartbeat[heardFrom].tv_usec) > 600) // missed two hearbeats or this is the first heartbeat
            {
                vector<int> fromToDestPath{heardFrom};
                updateDistanceAndPath(heardFrom, heardFrom, 0, fromToDestPath);
                // myNeighbors.add(heardFrom)
                broadcastLSA();
            }

            // record that we heard from heardFrom just now.
            gettimeofday(&previousHeartbeat[heardFrom], 0);
        }

        // check if there is any neighbor link is broken, if so update distanceAndPath and broadcast LSA
        for (int i = 0; i < 256; i += 1)
        {
            if (i != myNodeId)
            {
                if (previousHeartbeat[i].tv_sec != 0 &&
                    ((now.tv_sec - previousHeartbeat[i].tv_sec) * 1000000L + now.tv_usec - previousHeartbeat[i].tv_usec) > 600) // missed two hearbeats
                {
                    // myNeighbors.pop(heardFrom)

                    distanceAndPath[i].first = -1;
                    distanceAndPath[i].second.clear;

                    // additional check if other paths are using this broken link?
                    broadcastLSA();
                }
            }
        }

        if (!strncmp(recvBuf, "send", 4)) // forwarding message
        {
            short int destNodeId;
            memcpy(&destNodeId, recvBuf + 4, 2);
            destNodeId = ntohs(destNodeId);

            std::cout << "Node ID: " << destNodeId << std::endl;

            char message[100];
            memcpy(&message, recvBuf + 6, bytesRecvd - 5);

            string rmessage(message);
            std::cout << "Message received: " << rmessage << std::endl;

            forwardMessage(destNodeId, message);
        }

        else if (!strncmp(recvBuf, "LSAs", 4)) // LSA message
        {
            uint32_t contentLength;
            memcpy(&contentLength, recvBuf + 4, sizeof(contentLength));

            string content(recvBuf + 4 + sizeof(contentLength));

            // std::cout << "Received number of bytes: " << bytesRecvd << std::endl;
            // std::cout << "message_type: " << message_type << std::endl;
            // std::cout << "contentLength: " << contentLength << std::endl;
            // std::cout << "content: " << content << std::endl;

            json j_map = json::parse(content);
            auto otherNodePathVector = j_map.get<map<int, pair<int, vector<int>>>>();

            processLSAMessage(otherNodePathVector);
        }
    }

    //(should never reach here)
    close(mySocketUDP);
}

void Node::processLSAMessage(map<int, pair<int, vector<int>>> otherNodePathVector)
{
    int fromNodeId = otherNodePathVector[256].first;

    for (map<int, pair<int, vector<int>>>::iterator iterator = otherNodePathVector.begin(); iterator != otherNodePathVector.end(); iterator++)
    {
        int destNodeId = iterator->first;
        int fromToDestDistance = iterator->second.first;
        vector<int> fromToDestPath = iterator->second.second;

        updateDistanceAndPath(fromNodeId, destNodeId, fromToDestDistance, fromToDestPath);
    }
}

void Node::updateDistanceAndPath(int fromNodeId, int destNodeId, int fromToDestDistance, vector<int> fromToDestPath)
{
    int thisToFromDistance = linkCost[fromNodeId];
    int thisToDestDistance = distanceAndPath[destNodeId].first;
    vector<int> thisToDestPath = distanceAndPath[destNodeId].second;

    // link broken
    if (fromToDestDistance == -1)
    {
        if (std::find(thisToDestPath.begin(), thisToDestPath.end(), fromNodeId) != thisToDestPath.end())
        {
            // pick a different route to destNode
            // if no other route available, update my distanceAndPath to [-1, []]
            broadcastLSA();
        }
    }
    else // find shorter path
    {
        if (thisToDestDistance == -1 || (thisToDestDistance > thisToFromDistance + fromToDestDistance) || ((thisToDestDistance == thisToFromDistance + fromToDestDistance) && distanceAndPath[destNodeId].second[0] > fromNodeId))
        {
            distanceAndPath[destNodeId].first = thisToFromDistance + fromToDestDistance;
            vector<int> newPath(fromToDestPath);
            if (fromNodeId != destNodeId)
            {
                newPath.insert(newPath.begin(), fromNodeId);
            }
            distanceAndPath[destNodeId].second = newPath;
            broadcastLSA();
        }
    }
}

void Node::forwardMessage(int destNodeId, char *message)
{
    if (distanceAndPath[destNodeId].first != -1)
    {
        int nextHop = distanceAndPath[destNodeId].second[0];
        sendto(mySocketUDP, message, sizeof(message), 0,
               (struct sockaddr *)&allNodeSocketAddrs[nextHop], sizeof(allNodeSocketAddrs[nextHop]));
        // saveLog: sent to ...
    }
    else
    {
        // saveLog: unreacheable
    }
}

void Node::setupNodeSockets()
{
    for (int i = 0; i < 256; i++)
    {
        // gettimeofday(&previousHeartbeat[i], 0);

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

    // char myAddr[100];
    // struct sockaddr_in bindAddr;
    // sprintf(myAddr, "10.1.1.%d", myNodeId);
    // memset(&bindAddr, 0, sizeof(bindAddr));
    // bindAddr.sin_family = AF_INET;
    // bindAddr.sin_port = htons(7777);
    // inet_pton(AF_INET, myAddr, &bindAddr.sin_addr);

    struct sockaddr_in bindAddr = allNodeSocketAddrs[myNodeId];
    if (bind(mySocketUDP, (struct sockaddr *)&bindAddr, sizeof(struct sockaddr_in)) < 0)
    {
        perror("bind error");
        close(mySocketUDP);
        exit(2);
    }
}
