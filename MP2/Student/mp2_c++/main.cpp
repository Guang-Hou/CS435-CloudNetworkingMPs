#include "node.cpp"
#include <iostream>
#include <thread>

int main(int argc, char **argv)
{
    if (argc != 4)
    {
        fprintf(stderr, "Usage: %s mynodeid initialcostsfile logfile\n\n", argv[0]);
        exit(1);
    }

    // initialization: get this process's node ID, record what time it is,
    // and set up our sockaddr_in's for sending to the other nodes.
    int inputId = atoi(argv[1]);
    string costFile = argv[2];
    string logFile = argv[3];

    Node node(inputId);

    // Read and parse initial costs file. default to cost 1 if no entry for a node. file may be empty.

    node.readCostFile(costFile);

    node.broadcastLSA();
    
    std::thread th1(&Node::sendHeartbeats, node);

    // good luck, have fun!
    node.listenForNeighbors();

    th1.join();
}
