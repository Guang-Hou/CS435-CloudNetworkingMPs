from fcntl import F_WRLCK
import networkx as nx
import sys
from pyvis.network import Network
import glob
import os
import re

def get_graph(test_directory):
    g = nx.Graph()
    
    # gather a graph
    edges = open(os.path.join(test_directory,'topology.txt'), 'r').read().split("\n")
    for edge in edges:
        node_pair = edge.split()
        if len(node_pair) >= 2:
            g.add_edge(node_pair[0], node_pair[1], weight=1)
    for cost_file in glob.glob(test_directory+'/costs*'):
        node_no = re.findall(r'\d+', cost_file)[-1]
        with open(cost_file, 'r') as f:
            edges = f.read().split('\n')
            for edge in edges:
                node_pair = edge.split()
                if len(node_pair) >= 2:
                    g.add_edge(node_no, node_pair[0], weight=node_pair[1])
    with open(os.path.join(test_directory,'topology.txt'), 'r') as f:
        nodes = f.read().split()
        for node in nodes:
            g.add_node(node)
    return g

# given a graph, output nodes.txt, topology.txt, costs.txt
def output_files(g, test_directory):
    tmp = sorted(list(g.nodes), key=lambda x: int(x))
    with open(os.path.join(test_directory,'nodes.txt'), 'w') as f:
        f.write(' '.join(tmp))
    with open(os.path.join(test_directory,'topology.txt'), 'w') as f:
        for each_edge in list(g.edges):
            f.write(' '.join(each_edge) + '\n')
    for node in g.nodes:
        filename = os.path.join(test_directory,'costs'+node)
        with open(filename, 'w') as f:
            for neighbor in g[node]:
                f.write(neighbor+' '+g[node][neighbor]['weight']+'\n')

def main():
    if (len(sys.argv) < 2):
        print('usage: plot_topology.py test_directory')
        return
    test_directory = sys.argv[1]
    g = get_graph(test_directory)
    # output_files(g,test_directory)
    nt = Network('800px', '1500px')
    nt.from_nx(g)
    nt.show('nx.html')

if __name__ == '__main__':
    main()