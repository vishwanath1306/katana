/** GMetis -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2011, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 *
 * @author Xin Sui <xinsui@cs.utexas.edu>
 */

#include <vector>
#include <iostream>
#include <string.h>
#include <stdlib.h>
#include <algorithm>
#include <cmath>

#include "GMetisConfig.h"
#include "MetisGraph.h"
#include "PMetis.h"

#include "Galois/Graph/LCGraph.h"
#include "Galois/Statistic.h"

#include "Lonestar/BoilerPlate.h"

namespace cll = llvm::cl;

static const char* name = "GMetis";
static const char* desc = "Partitions a graph into K parts and minimizing the graph cut";
static const char* url = "gMetis";

static cll::opt<bool> mtxInput("mtxinput", cll::desc("Use text mtx files instead binary based ones"), cll::init(false));
static cll::opt<bool> weighted("weighted", cll::desc("weighted"), cll::init(false));
static cll::opt<std::string> filename(cll::Positional, cll::desc("<input file>"), cll::Required);
static cll::opt<int> numPartitions(cll::Positional, cll::desc("<Number of partitions>"), cll::Required);

bool verifyCoarsening(MetisGraph *metisGraph) {

	if(metisGraph == NULL)
		return true;
	cout<<endl<<"##### Verifying coarsening #####"<<endl;
	int matchedCount=0;
	int unmatchedCount=0;
	GGraph *graph = metisGraph->getGraph();

	for(GGraph::iterator ii=graph->begin(),ee=graph->end();ii!=ee;ii++) {
		GNode node = *ii;
		MetisNode &nodeData = graph->getData(node);
		GNode matchNode;
		if(variantMetis::localNodeData) {
			if(!nodeData.isMatched())
				return false;
			matchNode = static_cast<GNode>(nodeData.getMatchNode());
		} else {
			if(!metisGraph->isMatched(nodeData.getNodeId())) {
				return false;
			}
			matchNode = metisGraph->getMatch(nodeData.getNodeId());
		}

		if(matchNode == node) {
			unmatchedCount++;
		}
		else{
			matchedCount++;
			MetisNode &matchNodeData = graph->getData(matchNode);
			GNode mmatch;
			if(variantMetis::localNodeData) {
				if(!matchNodeData.isMatched())
					return false;
				mmatch = static_cast<GNode>(matchNodeData.getMatchNode());
			} else {
				if(!metisGraph->isMatched(matchNodeData.getNodeId()))
						return false;
				mmatch = metisGraph->getMatch(matchNodeData.getNodeId());
			}

			if(node!=mmatch){
				cout<<"Node's matched node is not matched to this node";
				return false;
			}
		}
		int edges=0;
		for(GGraph::edge_iterator ii=graph->edge_begin(node),ee=graph->edge_end(node);ii!=ee;ii++) {
			edges++;
		}
		if(edges!=nodeData.getNumEdges()) {
			cout<<"Number of edges dont match";
			return false;
		}
	}
	bool ret = verifyCoarsening(metisGraph->getFinerGraph());
	cout<<matchedCount<<" "<<unmatchedCount<<endl;
	if(matchedCount+unmatchedCount != metisGraph->getNumNodes())
		return false;
	if(ret == false)
		return false;
	return true;

}

bool verifyRecursiveBisection(MetisGraph* metisGraph,int nparts) {

	GGraph *graph = metisGraph->getGraph();
	int partNodes[nparts];
	memset(partNodes,0,sizeof(partNodes));
	for(GGraph::iterator ii = graph->begin(),ee=graph->end();ii!=ee;ii++) {
		GNode node = *ii;
		MetisNode &nodeData = graph->getData(node);
		if(!(nodeData.getPartition()<nparts))
			return false;
		partNodes[nodeData.getPartition()]++;
		int edges=0;
		for(GGraph::edge_iterator ii=graph->edge_begin(node),ee=graph->edge_end(node);ii!=ee;ii++) {
			edges++;
		}
		if(nodeData.getNumEdges()!=edges) {
			return false;
		}
	}
	int sum=0;
	for(int i=0;i<nparts;i++) {
		if(partNodes[i]<=0)
			return false;
		sum+=partNodes[i];
	}


	if(sum != metisGraph->getNumNodes())
		return false;
	return true;
}

/**
 * KMetis Algorithm
 */
void partition(MetisGraph* metisGraph, int nparts) {
	int coarsenTo = (int) max(metisGraph->getNumNodes() / (40 * intlog2(nparts)), 20 * (nparts));
	int maxVertexWeight = (int) (1.5 * ((metisGraph->getNumNodes()) / (double) coarsenTo));
	Coarsener coarsener(false, coarsenTo, maxVertexWeight);
	Galois::StatTimer T;
	T.start();
	Galois::Timer t;
	t.start();
	MetisGraph* mcg = coarsener.coarsen(metisGraph);
	t.stop();
	cout<<"coarsening time: " << t.get() << " ms"<<endl;
	T.stop();
	//return;
	if(testMetis::testCoarsening) {
		if(verifyCoarsening(mcg->getFinerGraph())) {
			cout<<"#### Coarsening is correct ####"<<endl;
		} else {
			cout<<"!!!! Coarsening is wrong !!!!"<<endl;
		}
	}

	float* totalPartitionWeights = new float[nparts];
	std::fill_n(totalPartitionWeights, nparts, 1 / (float) nparts);
	maxVertexWeight = (int) (1.5 * ((mcg->getNumNodes()) / COARSEN_FRACTION));
	PMetis pmetis(20, maxVertexWeight);
	Galois::Timer init_part_t;
	init_part_t.start();
	pmetis.mlevelRecursiveBisection(mcg, nparts, totalPartitionWeights, 0, 0);
	init_part_t.stop();
	cout << "initial partition time: "<< init_part_t.get()  << " ms"<<endl;

	//return;

	if(testMetis::testInitialPartition) {
		cout<<endl<<"#### Verifying initial partition ####"<<endl;
		if(!verifyRecursiveBisection(mcg,nparts)) {
			cout<<endl<<"!!!! Initial partition is wrong !!!!"<<endl;
		}else {
			cout<<endl<<"#### Initial partition is right ####"<<endl;
		}
	}

	//return;
	Galois::Timer refine_t;
	std::fill_n(totalPartitionWeights, nparts, 1 / (float) nparts);
	refine_t.start();
	refineKWay(mcg, metisGraph, totalPartitionWeights, (float) 1.03, nparts);
	refine_t.stop();
	cout << "refine time: " << refine_t.get() << " ms"<<endl;
	delete[] totalPartitionWeights;
	T.stop();
}


void verify(MetisGraph* metisGraph) {
	if (!metisGraph->verify()) {
		cout<<"KMetis failed."<<endl;
	}else{
		cout<<"KMetis okay"<<endl;
	}
}

typedef Galois::Graph::LC_CSR_Graph<int, unsigned int> InputGraph;
typedef Galois::Graph::LC_CSR_Graph<int, unsigned int>::GraphNode InputGNode;

void readMetisGraph(MetisGraph* metisGraph, const char* filename){
	std::ifstream file(filename);
	string line;
	std::getline(file, line);
	while(line.find('%')!=string::npos){
		std::getline(file, line);
	}

	int numNodes, numEdges;
	sscanf(line.c_str(), "%d %d", &numNodes, &numEdges);
	cout<<numNodes<<" "<<numEdges<<endl;
	GGraph* graph = metisGraph->getGraph();
	vector<GNode> nodes(numNodes);
	for (int i = 0; i < numNodes; i++) {
		GNode n = graph->createNode(MetisNode(i, 1));
		nodes[i] = n;
		graph->addNode(n);
	}
	int countEdges = 0;
	for (int i = 0; i < numNodes; i++) {
		std::getline(file, line);
		char const * items = line.c_str();
		char* remaining;
		GNode n1 = nodes[i];

		while (true) {
			int index = strtol(items, &remaining,10) - 1;
			if(index < 0) break;
			items = remaining;
			GNode n2 = nodes[index];
			if(n1==n2){
				continue;
			}
			graph->getEdgeData(graph->addEdge(n1, n2)) = 1;
			graph->getData(n1).addEdgeWeight(1);
			graph->getData(n1).incNumEdges();
			countEdges++;
		}
	}

	assert(countEdges == numEdges*2);
	metisGraph->setNumEdges(numEdges);
	metisGraph->setNumNodes(numNodes);
	cout<<"finshied reading graph " << metisGraph->getNumNodes() << " " << metisGraph->getNumEdges()<<endl;
}


void readGraph(MetisGraph* metisGraph, const char* filename, bool weighted = false, bool directed = true){
	InputGraph inputGraph;
	inputGraph.structureFromFile(filename);
	cout<<"start to transfer data to GGraph"<<endl;
	int id = 0;
	for (InputGraph::iterator ii = inputGraph.begin(), ee = inputGraph.end(); ii != ee; ++ii) {
		InputGNode node = *ii;
		inputGraph.getData(node)=id++;
	}

	GGraph* graph = metisGraph->getGraph();
	vector<GNode> gnodes(inputGraph.size());
	id = 0;
	for(uint64_t i=0;i<inputGraph.size();i++){
		GNode node = graph->createNode(MetisNode(id, 1));
		graph->addNode(node);
		gnodes[id++] = node;
	}

	int numEdges = 0;
	for (InputGraph::iterator ii = inputGraph.begin(), ee = inputGraph.end(); ii != ee; ++ii) {
		InputGNode inNode = *ii;

		int nodeId = inputGraph.getData(inNode);
		GNode node = gnodes[nodeId];

		MetisNode& nodeData = graph->getData(node);

		for (InputGraph::edge_iterator jj = inputGraph.edge_begin(inNode), eejj = inputGraph.edge_end(inNode); jj != eejj; ++jj) {
		  InputGNode inNeighbor = inputGraph.getEdgeDst(jj);
			if(inNode == inNeighbor) continue;
			int neighId = inputGraph.getData(inNeighbor);
			int weight = 1;
			if(weighted){
			  weight = inputGraph.getEdgeData(jj);
			}
			if(!directed){
			  graph->getEdgeData(graph->addEdge(node, gnodes[neighId])) = weight;//
				nodeData.incNumEdges();
				nodeData.addEdgeWeight(weight);//inputGraph.getEdgeData(inNode, inNeighbor));
				numEdges++;
			}else{
			  graph->getEdgeData(graph->addEdge(node, gnodes[neighId])) = weight;//
			  graph->getEdgeData(graph->addEdge(gnodes[neighId], node)) = weight;//
			}
		}

	}

	if(directed){
		for (GGraph::iterator ii = graph->begin(), ee = graph->end(); ii != ee; ++ii) {
			GNode node = *ii;
			MetisNode& nodeData = graph->getData(node);
			for (GGraph::edge_iterator jj = graph->edge_begin(node), eejj = graph->edge_end(node); jj != eejj; ++jj) {
			  GNode neighbor = graph->getEdgeDst(jj);
				nodeData.incNumEdges();
				nodeData.addEdgeWeight(graph->getEdgeData(jj));
				assert(graph->getEdgeData(jj) == graph->getEdgeData(graph->findEdge(neighbor, node)));
				numEdges++;
			}
		}
	}
	cout<<"numNodes: "<<inputGraph.size()<<"|numEdges: "<<numEdges/2<<endl;
	metisGraph->setNumEdges(numEdges/2);
	metisGraph->setNumNodes(gnodes.size());
	cout<<"end of transfer data to GGraph"<<endl;
}

namespace testMetis {
	bool testCoarsening = true;
	bool testInitialPartition = true;;
}
namespace variantMetis {
	bool mergeMatching = true;
	bool noPartInfo = false;
	bool localNodeData = true;
}


int main(int argc, char** argv) {
	Galois::StatManager statManager;
	LonestarStart(argc, argv, name, desc, url);

	srand(-1);
	MetisGraph metisGraph;
	GGraph graph;
	metisGraph.setGraph(&graph);
	bool directed = true;
	if(mtxInput){
	  readMetisGraph(&metisGraph, filename.c_str());
	}else{
	  readGraph(&metisGraph, filename.c_str(), weighted, false);
	}
  	Galois::Statistic("MeminfoPre1", Galois::Runtime::MM::pageAllocInfo());
  	Galois::preAlloc(9000);
  	Galois::Statistic("MeminfoPre2", Galois::Runtime::MM::pageAllocInfo());
	partition(&metisGraph, numPartitions);
  	Galois::Statistic("MeminfoPre3", Galois::Runtime::MM::pageAllocInfo());
	verify(&metisGraph);
}

int getRandom(int num){
//	int randNum = rand()%num;
//	return (rand()>>3)%(num);
	//	return randNum;
	return ((int)(drand48()*((double)(num))));
}

// int gNodeToInt(GNode node){
// 	return graph->getData(node).getNodeId();
// }

int intlog2(int a){
	int i;
	for (i=1; a > 1; i++, a = a>>1);
	return i-1;
}
