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

    Node node(inputId, costFile, logFile);

    // node.broadcastLSA();

    /*
    pqOfPaths pathVector[256];
    vector<int> path{1}; // for nodeID 1
    dp distancePathPair{0, path};
    pathVector[1].push(distancePathPair);

    pqOfPaths *currPaths = &pathVector[2];
    vector<int> fromToDestPath{2};
    vector<int> newPath(fromToDestPath);

    // bool isEmpty = currPaths.empty();
    // cout << "Is the currPaths empty: " << isEmpty << endl;

    currPaths->push(make_pair(0, newPath));
    // cout << "Adding new path." << endl;
    cout << "is the path empty? " << pathVector[2].empty() << endl;
    if (!pathVector[2].empty())
    {
        json j_vector(pathVector[2].top().second);
        string s1 = j_vector.dump();
        cout << "Updated path: " << s1 << endl;
    }
    */

    std::thread th1(&Node::sendHeartbeats, node);

    // good luck, have fun!
    // node.listenForNeighbors();

    std::thread th2(&Node::listenForNeighbors, node);

    th1.join();
    th2.join();
}
