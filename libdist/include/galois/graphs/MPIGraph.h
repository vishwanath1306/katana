/** MPI Graph -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2017, The University of Texas at Austin. All rights reserved.
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
 * @section Description
 *
 * Graph that uses MPI read to load relavent portions of a graph into memory
 * for later reading.
 *
 * @author Loc Hoang <l_hoang@utexas.edu>
 */

// TODO version 2 Galois binary graph support; currently only suppports
// version 1

#ifndef GALOIS_GRAPH_MPIGRAPH_H
#define GALOIS_GRAPH_MPIGRAPH_H

#include <limits>
#include <galois/Galois.h>
#include <galois/gIO.h>
#include <galois/gstl.h>
#include <galois/Threads.h>
#include <galois/Reduction.h>
#include <mpi.h>

#include <boost/iterator/counting_iterator.hpp>
namespace galois {
namespace graphs {

template <typename EdgeDataType>
class MPIGraph {
private:
  // buffers that you load data into
  uint64_t* outIndexBuffer;
  uint32_t* edgeDestBuffer;
  EdgeDataType* edgeDataBuffer;

  uint64_t numLocalNodes;
  uint64_t numLocalEdges;

  uint64_t nodeOffset;
  uint64_t edgeOffset;
  bool graphLoaded;


  // accumulators for tracking bytes read
  galois::GAccumulator<uint64_t> numBytesReadOutIndex;
  galois::GAccumulator<uint64_t> numBytesReadEdgeDest;
  galois::GAccumulator<uint64_t> numBytesReadEdgeData;

  /**
   * Load the out indices (i.e. where a particular node's edges begin in the
   * array of edges) from the file.
   *
   * @param graphFile loaded MPI file for the graph
   * @param nodeStart the first node to load
   * @param numNodesToLoad number of nodes to load
   */
  void loadOutIndex(MPI_File& graphFile, uint64_t nodeStart, 
                    uint64_t numNodesToLoad) {
    if (numNodesToLoad == 0) {
      return;
    }
    assert(outIndexBuffer == nullptr);
    outIndexBuffer = (uint64_t*)malloc(sizeof(uint64_t) * numNodesToLoad);

    if (outIndexBuffer == nullptr) {
      GALOIS_DIE("Failed to allocate memory for out index buffer.");
    }

    // each thread reads in disjunct portion
    galois::on_each(
      [&](unsigned tid, unsigned nthreads) {
        auto myWork = galois::block_range(nodeStart, nodeStart + numNodesToLoad,
                                          tid, nthreads);

        uint64_t threadNodeStart = myWork.first;
        uint64_t threadNumNodesToLoad = myWork.second - threadNodeStart;

        if (threadNumNodesToLoad == 0) {
          return;
        }

        // position to start of contiguous chunk of nodes to read
        uint64_t readPosition = (4 + threadNodeStart) * sizeof(uint64_t);
    
        uint64_t nodesLoaded = 0;
        MPI_Status mpiStatus;
    
        while (threadNumNodesToLoad > 0) {
          // File_read can only go up to the max int
          uint64_t toLoad = std::min(threadNumNodesToLoad, 
                                     (uint64_t)std::numeric_limits<int>::max());
    
          MPI_File_read_at(graphFile, 
                           readPosition + (nodesLoaded * sizeof(uint64_t)), 
                           ((char*)this->outIndexBuffer) + 
                            (threadNodeStart - nodeStart + nodesLoaded) * 
                            sizeof(uint64_t), 
                           toLoad, MPI_UINT64_T, &mpiStatus); 
    
          int itemsRead; 
          MPI_Get_count(&mpiStatus, MPI_UINT64_T, &itemsRead);
    
          threadNumNodesToLoad -= itemsRead;
          nodesLoaded += itemsRead;
        }

        assert(threadNumNodesToLoad == 0);
      },
      galois::no_stats()
    );

    nodeOffset = nodeStart;
  }

  /**
   * Load the edge destination information from the file.
   *
   * @param graphFile loaded MPI file for the graph
   * @param edgeStart the first edge to load
   * @param numEdgesToLoad number of edges to load
   * @param numGlobalNodes total number of nodes in the graph file; needed
   * to determine offset into the file
   */
  void loadEdgeDest(MPI_File& graphFile, uint64_t edgeStart, 
                    uint64_t numEdgesToLoad, uint64_t numGlobalNodes) {
    if (numEdgesToLoad == 0) {
      return;
    }

    assert(edgeDestBuffer == nullptr);
    edgeDestBuffer = (uint32_t*)malloc(sizeof(uint32_t) * numEdgesToLoad);

    if (edgeDestBuffer == nullptr) {
      GALOIS_DIE("Failed to allocate memory for edge dest buffer.");
    }

    // each thread reads in disjunct portion
    galois::on_each(
      [&](unsigned tid, unsigned nthreads) {
        auto myWork = galois::block_range(edgeStart, edgeStart + numEdgesToLoad,
                                          tid, nthreads);

        uint64_t threadEdgeStart = myWork.first;
        uint64_t threadNumEdgesToLoad = myWork.second - threadEdgeStart;

        if (threadNumEdgesToLoad == 0) {
          return;
        }

        // position to start of contiguous chunk of edges to read
        uint64_t readPosition = (4 + numGlobalNodes) * sizeof(uint64_t) +
                                (sizeof(uint32_t) * threadEdgeStart);

        uint64_t edgesLoaded = 0;
        MPI_Status mpiStatus;
    
        while (threadNumEdgesToLoad > 0) {
          // File_read can only go up to the max int
          uint64_t toLoad = std::min(threadNumEdgesToLoad, 
                                     (uint64_t)std::numeric_limits<int>::max());
    
          MPI_File_read_at(graphFile, readPosition + (edgesLoaded * sizeof(uint32_t)), 
                           ((char*)this->edgeDestBuffer) + 
                            (threadEdgeStart - edgeStart + edgesLoaded) * 
                            sizeof(uint32_t), 
                           toLoad, MPI_UINT32_T, &mpiStatus); 
          int itemsRead; 
          MPI_Get_count(&mpiStatus, MPI_UINT32_T, &itemsRead);
    
          threadNumEdgesToLoad -= itemsRead;
          edgesLoaded += itemsRead;
        }

        assert(threadNumEdgesToLoad == 0);
      },
      galois::no_stats()
    );

    edgeOffset = edgeStart;
  }

  /**
   * Load the edge data information from the file.
   *
   * @tparam EdgeType must be non-void in order to call this function
   *
   * @param edgeStart the first edge to load
   * @param numEdgesToLoad number of edges to load
   * @param numGlobalNodes total number of nodes in the graph file; needed
   * to determine offset into the file
   * @param numGlobalEdges total number of edges in the graph file; needed
   * to determine offset into the file
   */
  template<typename EdgeType, 
           typename std::enable_if<!std::is_void<EdgeType>::value>::type* = 
                                   nullptr>
  void loadEdgeData(MPI_File& graphFile, uint64_t edgeStart, 
                    uint64_t numEdgesToLoad, uint64_t numGlobalNodes,
                    uint64_t numGlobalEdges) {
    galois::gDebug("Loading edge data with MPI read");

    if (numEdgesToLoad == 0) {
      return;
    }

    assert(edgeDataBuffer == nullptr);
    edgeDataBuffer = (EdgeDataType*)malloc(sizeof(EdgeDataType) * numEdgesToLoad);

    if (edgeDataBuffer == nullptr) {
      GALOIS_DIE("Failed to allocate memory for edge data buffer.");
    }

    // position after nodes + edges
    uint64_t baseReadPosition = (4 + numGlobalNodes) * sizeof(uint64_t) +
                                (sizeof(uint32_t) * numGlobalEdges);
    
    // version 1 padding TODO make version agnostic
    if (baseReadPosition % 2) {
      baseReadPosition += sizeof(uint32_t);
    }

    galois::on_each(
      [&](unsigned tid, unsigned nthreads) {
        auto myWork = galois::block_range(edgeStart, edgeStart + numEdgesToLoad,
                                          tid, nthreads);

        uint64_t threadEdgeStart = myWork.first;
        uint64_t threadNumEdgesToLoad = myWork.second - threadEdgeStart;

        if (threadNumEdgesToLoad == 0) {
          return;
        }

        uint64_t numBytesToLoad = threadNumEdgesToLoad * sizeof(EdgeDataType);
        uint64_t bytesLoaded = 0;
        MPI_Status mpiStatus;

        // jump to first byte of edge data
        uint64_t readPosition = baseReadPosition + 
                                (sizeof(EdgeDataType) * threadEdgeStart);
   
        while (numBytesToLoad > 0) {
          // File_read can only go up to the max int
          uint64_t toLoad = std::min(numBytesToLoad, 
                                     (uint64_t)std::numeric_limits<int>::max());

          MPI_File_read_at(graphFile, readPosition + bytesLoaded, 
                           ((char*)this->edgeDataBuffer) + 
                            bytesLoaded +
                            (threadEdgeStart - edgeStart) * sizeof(EdgeDataType),
                           toLoad, MPI_BYTE, &mpiStatus); 

          int bytesRead; 
          MPI_Get_count(&mpiStatus, MPI_BYTE, &bytesRead);

          numBytesToLoad -= bytesRead;
          bytesLoaded += bytesRead;
        }

        assert(numBytesToLoad == 0);
      },
      galois::no_stats()
    );
  }

  /**
   * Load edge data function for when the edge data type is void, i.e.
   * no edge data to load.
   *
   * Does nothing of importance.
   *
   * @tparam EdgeType if EdgeType is void, this function will be used
   */
  template<typename EdgeType, 
           typename std::enable_if<std::is_void<EdgeType>::value>::type* = 
                                   nullptr>
  void loadEdgeData(MPI_File& graphFile, uint64_t edgeStart, 
                    uint64_t numEdgesToLoad, uint64_t numGlobalNodes,
                    uint64_t numGlobalEdges) {
    galois::gDebug("Not loading edge data with MPI read");
    // do nothing (edge data is void, i.e. no edge data)
  }

  /**
   * Resets graph metadata to default values. Does NOT touch the buffers.
   */
  void resetGraphStatus() {
    graphLoaded = false;
    nodeOffset = 0;
    edgeOffset = 0;
    numLocalNodes = 0;
    numLocalEdges = 0;
    resetReadCounters();
  }

  /**
   * Free all of the buffers in memory.
   */
  void freeMemory() {
    if (outIndexBuffer != nullptr) {
      free(outIndexBuffer);
      outIndexBuffer = nullptr;
    }
    if (edgeDestBuffer != nullptr) {
      free(edgeDestBuffer);
      edgeDestBuffer = nullptr;
    }
    if (edgeDataBuffer != nullptr) {
      free(edgeDataBuffer);
      edgeDataBuffer = nullptr;
    }
  }


public:
  /**
   * Initialize class variables. 
   *
   * NOTE THAT IT IS ASSUMED THAT MPI HAS BEEN INITIALIZED ALREADY
   * WHEN THIS CLASS IS CREATED.
   */
  MPIGraph() {
    outIndexBuffer = nullptr;
    edgeDestBuffer = nullptr;
    edgeDataBuffer = nullptr;
    resetGraphStatus();
  }

  /**
   * On destruction, free allocated buffers (if necessary).
   */
  ~MPIGraph() noexcept {
    freeMemory();
  }

  // copy not allowed
  MPIGraph(const MPIGraph&) = delete;
  MPIGraph& operator=(const MPIGraph&) = delete;
  // move not allowed
  MPIGraph(MPIGraph&&) = delete;
  MPIGraph& operator=(MPIGraph&&) = delete;

  /**
   * Given a node/edge range to load, loads the specified portion of the graph 
   * into memory buffers using MPI read.
   *
   * @param filename name of graph to load; should be in Galois binary graph
   * format
   * @param nodeStart First node to load
   * @param nodeEnd Last node to load, non-inclusive
   * @param edgeStart First edge to load; should correspond to first edge of
   * first node
   * @param edgeEnd Last edge to load, non-inclusive
   * @param numGlobalNodes Total number of nodes in the graph
   * @param numGlobalEdges Total number of edges in the graph
   */
  void loadPartialGraph(const std::string& filename, uint64_t nodeStart,
                        uint64_t nodeEnd, uint64_t edgeStart, 
                        uint64_t edgeEnd, uint64_t numGlobalNodes,
                        uint64_t numGlobalEdges) {
    if (graphLoaded) {
      GALOIS_DIE("Cannot load an MPI graph more than once.");
    }

    MPI_File graphFile;

    int fileSuccess = MPI_File_open(MPI_COMM_SELF, filename.c_str(), 
                                    MPI_MODE_RDONLY, MPI_INFO_NULL, &graphFile);
    
    if (fileSuccess != MPI_SUCCESS) {
      MPI_Abort(MPI_COMM_WORLD, fileSuccess);
    }

    unsigned int numActiveThreads = galois::getActiveThreads();

    if (numActiveThreads > 4) {
      galois::setActiveThreads(4);
    }

    assert(nodeEnd >= nodeStart);
    numLocalNodes = nodeEnd - nodeStart;
    loadOutIndex(graphFile, nodeStart, numLocalNodes);

    assert(edgeEnd >= edgeStart);
    numLocalEdges = edgeEnd - edgeStart;
    loadEdgeDest(graphFile, edgeStart, numLocalEdges, numGlobalNodes);

    // may or may not do something depending on EdgeDataType
    loadEdgeData<EdgeDataType>(graphFile, edgeStart, numLocalEdges, 
                               numGlobalNodes, numGlobalEdges);

    galois::setActiveThreads(numActiveThreads);

    graphLoaded = true;

    int closeSuccess = MPI_File_close(&graphFile);

    if (closeSuccess != MPI_SUCCESS) {
      MPI_Abort(MPI_COMM_WORLD, closeSuccess);
    }
  }

  using EdgeIterator = boost::counting_iterator<uint64_t>; 
  /**
   * Get the index to the first edge of the provided node.
   *
   * @param globalNodeID the global node id of the node to get the edge
   * for
   * @returns a GLOBAL edge id iterator
   */
  EdgeIterator edgeBegin(uint64_t globalNodeID) {
    assert(graphLoaded);
    if (numLocalNodes == 0) {
      return EdgeIterator(0);
    }
    assert(nodeOffset <= globalNodeID);
    assert(globalNodeID < (nodeOffset + numLocalNodes));

    uint64_t localNodeID = globalNodeID - nodeOffset;

    if (localNodeID != 0) {
      numBytesReadOutIndex += sizeof(uint64_t);
      return EdgeIterator(outIndexBuffer[localNodeID - 1]);
    } else {
      return EdgeIterator(edgeOffset);
    }
  }

  /**
   * Get the index to the first edge of the node after the provided node.
   *
   * @param globalNodeID the global node id of the node to get the edge
   * for
   * @returns a GLOBAL edge id iterator
   */
  EdgeIterator edgeEnd(uint64_t globalNodeID) {
    assert(graphLoaded);
    if (numLocalNodes == 0) {
      return EdgeIterator(0);
    }
    assert(nodeOffset <= globalNodeID);
    assert(globalNodeID < (nodeOffset + numLocalNodes));

    numBytesReadOutIndex += sizeof(uint64_t);

    uint64_t localNodeID = globalNodeID - nodeOffset;
    return EdgeIterator(outIndexBuffer[localNodeID]);
  }

  /**
   * Get the global node id of the destination of the provided edge.
   *
   * @param globalEdgeID the global edge id of the edge to get the destination
   * for (should obtain from edgeBegin/End)
   */
  uint64_t edgeDestination(uint64_t globalEdgeID) {
    assert(graphLoaded);
    if (numLocalEdges == 0) {
      return 0;
    }
    assert(edgeOffset <= globalEdgeID); 
    assert(globalEdgeID < (edgeOffset + numLocalEdges));

    numBytesReadEdgeDest += sizeof(uint32_t);

    uint64_t localEdgeID = globalEdgeID - edgeOffset;
    return edgeDestBuffer[localEdgeID];
  }

  /**
   * Get the edge data of some edge.
   *
   * @param globalEdgeID the global edge id of the edge to get the data of
   * @returns the edge data of the requested edge id
   */
  EdgeDataType edgeData(uint64_t globalEdgeID) {
    assert(graphLoaded);
    // shouldn't call this unless edge data was instantiated
    assert(edgeDataBuffer != nullptr);

    if (numLocalEdges == 0) {
      return 0;
    }

    assert(edgeOffset <= globalEdgeID); 
    assert(globalEdgeID < (edgeOffset + numLocalEdges));

    numBytesReadEdgeData += sizeof(EdgeDataType);

    uint64_t localEdgeID = globalEdgeID - edgeOffset;
    return edgeDataBuffer[localEdgeID];
  }

  /**
   * Reset reading counters.
   */
  void resetReadCounters() {
    numBytesReadOutIndex.reset();
    numBytesReadEdgeDest.reset();
    numBytesReadEdgeData.reset();
  }

  /**
   * Returns the total number of bytes read from this graph so far.
   *
   * @returns Total number of bytes read using the "get" functions on
   * out indices, edge destinations, and edge data.
   */
  uint64_t getBytesRead() {
    return numBytesReadOutIndex.reduce() +
           numBytesReadEdgeDest.reduce() +
           numBytesReadEdgeData.reduce();
  }

  /**
   * Free all of the in memory buffers in this object and reset graph status.
   */
  void resetAndFree() {
    freeMemory();
    resetGraphStatus();
  }
};
} // end graph namespace
} // end galois namespace
#endif