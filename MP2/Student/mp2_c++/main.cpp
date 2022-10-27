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

    std::thread th1(&Node::sendHeartbeats, &node);

    node.listenForNeighbors();
    // std::thread th2(&Node::listenForNeighbors, node);

    th1.join();
    //  th2.join();

    /*
    int destId = 1;
    set<int> nodesInPath = {1, 2, 3, 4};
    PATHT myPath = {1, 1, nodesInPath};

    json LSA = {
        {"fromID", inputId}, // 3
        {"destID", 0},
        {"dist", get<0>(myPath)}, // 1
        {"nodesInPath", get<2>(myPath)}};

    string strLSA = LSA.dump();

    cout << "LSA jason string: " << strLSA << endl;

    // when receving LSA from neighbor
    json rLSA = json::parse(strLSA);
    int rneighborId = rLSA["fromID"];
    int rdestId = rLSA["destID"];
    int fdistance = rLSA["dist"];
    set<int> s1 = rLSA["nodesInPath"];

    cout << "Parsed LSA: " << rneighborId << ", " << rdestId << ", " << fdistance << endl;

    set<int, greater<int>>::iterator itr;
    cout << "\nThe set s1 is : \n";
    for (itr = s1.begin(); itr != s1.end(); itr++)
    {
        cout << *itr << " ";
    }
    cout << endl;
    */
}
