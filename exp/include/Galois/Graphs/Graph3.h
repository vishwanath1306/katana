#include "Galois/Runtime/DistSupport.h"
#include "Galois/Runtime/Context.h"
#include "Galois/Runtime/MethodFlags.h"

#include <iterator>
#include <deque>

namespace Galois {
namespace Graph {

enum class EdgeDirection {Un, Out, InOut};

template<typename NodeTy, typename EdgeTy, EdgeDirection EDir>
class ThirdGraph;

template<typename NHTy>
class GraphNodeBase {
  NHTy nextNode;
  bool active;

protected:
  GraphNodeBase() :active(false) {}

  NHTy& getNextNode() { return nextNode; }

  void serialize(Galois::Runtime::Distributed::SerializeBuffer& s) const {
    gSerialize(s, nextNode, active);
  }
  void deserialize(Galois::Runtime::Distributed::DeSerializeBuffer& s) {
    gDeserialize(s,nextNode, active);
  }

  void dump(std::ostream& os) {
    os << "next: ";
    nextNode.dump();
    os << " active: ";
    os << active;
  }

public:
  void setActive(bool b) {
    active = b;
  }
};


template<typename NodeDataTy>
class GraphNodeData {
  NodeDataTy data;
  
protected:

  void serialize(Galois::Runtime::Distributed::SerializeBuffer& s) const {
    gSerialize(s,data);
  }
  void deserialize(Galois::Runtime::Distributed::DeSerializeBuffer& s) {
    gDeserialize(s,data);
  }

  void dump(std::ostream& os) {
    os << "data: " << data;
  }

public:
  template<typename... Args>
  GraphNodeData(Args&&... args) :data(std::forward<Args...>(args...)) {}
  GraphNodeData() :data() {}

  NodeDataTy& getData() {
    return data;
  }
};

template<>
class GraphNodeData<void> {};

template<typename NHTy, typename EdgeDataTy, EdgeDirection EDir>
class GraphNodeEdges;

template<typename NHTy, typename EdgeDataTy>
class Edge {
  NHTy dst;
  EdgeDataTy val;
public:
  template<typename... Args>
  Edge(const NHTy& d, Args&&... args) :dst(d), val(std::forward<Args...>(args...)) {}

  Edge() {}

  NHTy getDst() { return dst; }
  EdgeDataTy& getValue() { return val; }

  typedef int tt_has_serialize;
  void serialize(Galois::Runtime::Distributed::SerializeBuffer& s) const {
    gSerialize(s, dst, val);
  }
  void deserialize(Galois::Runtime::Distributed::DeSerializeBuffer& s) {
    gDeserialize(s,dst, val);
  }

  void dump(std::ostream& os) {
    os << "<{Edge: dst: ";
    dst.dump();
    os << " val: ";
    os << val;
    os << "}>";
  }
};

template<typename NHTy>
class Edge<NHTy, void> {
  NHTy dst;
public:
  Edge(const NHTy& d) :dst(d) {}
  Edge() {}

  NHTy getDst() { return dst; }

  typedef int tt_has_serialize;
  void serialize(Galois::Runtime::Distributed::SerializeBuffer& s) const {
    gSerialize(s,dst);
  }
  void deserialize(Galois::Runtime::Distributed::DeSerializeBuffer& s) {
    gDeserialize(s,dst);
  }

  void dump(std::ostream& os) {
    os << "<{Edge: dst: ";
    dst.dump();
    os << "}>";
  }
};

template<typename NHTy, typename EdgeDataTy>
class GraphNodeEdges<NHTy, EdgeDataTy, EdgeDirection::Out> {
  typedef Edge<NHTy, EdgeDataTy> EdgeTy;
  typedef std::deque<EdgeTy> EdgeListTy;

  EdgeListTy edges;

protected:
  void serialize(Galois::Runtime::Distributed::SerializeBuffer& s) const {
    gSerialize(s,edges);
  }
  void deserialize(Galois::Runtime::Distributed::DeSerializeBuffer& s) {
    gDeserialize(s,edges);
  }
  void dump(std::ostream& os) {
    os << "numedges: " << edges.size();
    for (decltype(edges.size()) x = 0; x < edges.size(); ++x) {
      os << " ";
      edges[x].dump(os);
    }
  }
 public:
  typedef typename EdgeListTy::iterator iterator;

  template<typename... Args>
  iterator createEdge(const NHTy& dst, Args&&... args) {
    return edges.emplace(edges.end(), dst, std::forward<Args...>(args...));
  }

  iterator createEdge(const NHTy& dst) {
    return edges.emplace(edges.end(), dst);
  }

  iterator begin() {
    return edges.begin();
  }

  iterator end() {
    return edges.end();
  }
};

template<typename NHTy, typename EdgeDataTy>
class GraphNodeEdges<NHTy, EdgeDataTy, EdgeDirection::InOut> {
  //FIXME
};

template<typename NodeDataTy, typename EdgeDataTy, EdgeDirection EDir>
class GraphNode;

template<typename NHTy>
class GraphNodeEdges<NHTy, void, EdgeDirection::Un> {
  typedef Edge<NHTy, void> EdgeTy;
  typedef std::deque<EdgeTy> EdgeListTy;

  EdgeListTy edges;

protected:
  void serialize(Galois::Runtime::Distributed::SerializeBuffer& s) const {
    gSerialize(s,edges);
  }
  void deserialize(Galois::Runtime::Distributed::DeSerializeBuffer& s) {
    gDeserialize(s,edges);
  }
  void dump(std::ostream& os) {
    os << "numedges: " << edges.size();
    for (decltype(edges.size()) x = 0; x < edges.size(); ++x) {
      os << " ";
      edges[x].dump(os);
    }
  }
 public:
  typedef typename EdgeListTy::iterator iterator;

 /*
  iterator createEdge(NHTy& node1, NHTy& node2) {
    GraphNodeEdges<NHTy, void, EdgeDirection::Un> *N1Edges, *N2Edges;
    N1Edges = static_cast<GraphNodeEdges<NHTy, void, EdgeDirection::Un>*>(&(*node1));
    N2Edges = static_cast<GraphNodeEdges<NHTy, void, EdgeDirection::Un>*>(&(*node2));
    N2Edges->edges.emplace(N2Edges->edges.end(), node1);
    return N1Edges->edges.emplace(N1Edges->edges.end(), node2);
  }
  */

  iterator createEdge(NHTy& node) {
    NHTy* NNode;
    GraphNodeEdges<NHTy, void, EdgeDirection::Un> *NEdges;
    NEdges = static_cast<GraphNodeEdges<NHTy, void, EdgeDirection::Un>*>(&(*node));
    NNode  = reinterpret_cast<NHTy*>(this);
    NEdges->edges.emplace(NEdges->edges.end(), *NNode);
    return edges.emplace(edges.end(), node);
  }

  iterator begin() {
    return edges.begin();
  }

  iterator end() {
    return edges.end();
  }
};

template<typename NHTy, typename EdgeDataTy>
class GraphNodeEdges<NHTy, EdgeDataTy, EdgeDirection::Un> {
  //FIXME
};


#define SHORTHAND Galois::Runtime::Distributed::gptr<GraphNode<NodeDataTy, EdgeDataTy, EDir> >

template<typename NodeDataTy, typename EdgeDataTy, EdgeDirection EDir>
class GraphNode
  : public Galois::Runtime::Lockable,
    public GraphNodeBase<SHORTHAND >,
    public GraphNodeData<NodeDataTy>,
    public GraphNodeEdges<SHORTHAND, EdgeDataTy, EDir>
{
  friend class ThirdGraph<NodeDataTy, EdgeDataTy, EDir>;

  using GraphNodeBase<SHORTHAND >::getNextNode;

public:
  typedef SHORTHAND Handle;

  template<typename... Args>
  GraphNode(Args&&... args) :GraphNodeData<NodeDataTy>(std::forward<Args...>(args...)) {}

  GraphNode() {}

  //serialize
  typedef int tt_has_serialize;
  void serialize(Galois::Runtime::Distributed::SerializeBuffer& s) const {
    GraphNodeBase<SHORTHAND >::serialize(s);
    GraphNodeData<NodeDataTy>::serialize(s);
    GraphNodeEdges<SHORTHAND, EdgeDataTy, EDir>::serialize(s);
  }
  void deserialize(Galois::Runtime::Distributed::DeSerializeBuffer& s) {
    GraphNodeBase<SHORTHAND >::deserialize(s);
    GraphNodeData<NodeDataTy>::deserialize(s);
    GraphNodeEdges<SHORTHAND, EdgeDataTy, EDir>::deserialize(s);
  }
  void dump(std::ostream& os) {
    os << this << " ";
    os << "<{GN: ";
    GraphNodeBase<SHORTHAND >::dump(os);
    os << " ";
    GraphNodeData<NodeDataTy>::dump(os);
    os << " ";
    GraphNodeEdges<SHORTHAND, EdgeDataTy, EDir>::dump(os);
    os << "}>";
  }
};

#undef SHORTHAND

/**
 * A Graph
 *
 * @param NodeTy type of node data (may be void)
 * @param EdgeTy type of edge data (may be void)
 * @param IsDir  bool indicated if graph is directed
 *
*/
template<typename NodeTy, typename EdgeTy, EdgeDirection EDir>
class ThirdGraph { //: public Galois::Runtime::Distributed::DistBase<ThirdGraph> {
  typedef GraphNode<NodeTy, EdgeTy, EDir> gNode;

  struct SubGraphState : public Galois::Runtime::Lockable {
    typename gNode::Handle head;
    Galois::Runtime::Distributed::gptr<SubGraphState> next;
    Galois::Runtime::Distributed::gptr<SubGraphState> master;
    typedef int tt_has_serialize;
    typedef int tt_dir_blocking;
    void serialize(Galois::Runtime::Distributed::SerializeBuffer& s) const {
      gSerialize(s, head, next, master);
    }
    void deserialize(Galois::Runtime::Distributed::DeSerializeBuffer& s) {
      gDeserialize(s,head, next, master);
    }
    SubGraphState() :head(), next(), master(this) {}
  };

  SubGraphState localState;

public:
  typedef typename gNode::Handle NodeHandle;

  template<typename... Args>
  NodeHandle createNode(Args&&... args) {
    NodeHandle N(new gNode(std::forward<Args...>(args...)));
    N->getNextNode() = localState.head;
    localState.head = N;
    return N;
  }

  NodeHandle createNode() {
    NodeHandle N(new gNode());
    N->getNextNode() = localState.head;
    localState.head = N;
    return N;
  }
  
  class iterator : public std::iterator<std::forward_iterator_tag, NodeHandle> {
    NodeHandle n;
    Galois::Runtime::Distributed::gptr<SubGraphState> s;
    void next() {
      n = n->getNextNode();
      while (!n && s->next) {
	s = s->next;
	n = s->head;
      }
      if (!n) s.initialize(nullptr);
    }
  public:
  iterator() :n(), s() {}
    explicit iterator(Galois::Runtime::Distributed::gptr<SubGraphState> ms) :n(ms->head), s(ms) {
      while (!n && s->next) {
	s = s->next;
	n = s->head;
      }
      if (!n) s.initialize(nullptr);
    }

    NodeHandle& operator*() { return n; }
    iterator& operator++() { next(); return *this; }
    iterator operator++(int) { iterator tmp(*this); next(); return tmp; }
    bool operator==(const iterator& rhs) { return n == rhs.n; }
    bool operator!=(const iterator& rhs) { return n != rhs.n; }

    void dump() {
      n.dump();
      s.dump();
    }
  };

  iterator begin() { return iterator(localState.master); }
  iterator end() { return iterator(); }

  class local_iterator : public std::iterator<std::forward_iterator_tag, NodeHandle> {
    NodeHandle n;
    void next() {
      n = n->getNextNode();
    }
  public:
    explicit local_iterator(NodeHandle N) :n(N) {}
    local_iterator() :n() {}
    local_iterator(const local_iterator& mit) : n(mit.n) {}

    NodeHandle& operator*() { return n; }
    local_iterator& operator++() { next(); return *this; }
    local_iterator operator++(int) { local_iterator tmp(*this); operator++(); return tmp; }
    bool operator==(const local_iterator& rhs) { return n == rhs.n; }
    bool operator!=(const local_iterator& rhs) { return n != rhs.n; }
  };

  local_iterator local_begin() { return local_iterator(localState.head); }
  local_iterator local_end() { return local_iterator(); }

  ThirdGraph() {}
  // mark the graph as persistent so that it is distributed
  typedef int tt_is_persistent;
  typedef int tt_has_serialize;
  void serialize(Galois::Runtime::Distributed::SerializeBuffer& s) const {
    //This is what is called on the source of a replicating source
    gSerialize(s,localState.master);
  }
  void deserialize(Galois::Runtime::Distributed::DeSerializeBuffer& s) {
    //This constructs the local node of the distributed graph
    gDeserialize(s,localState.master);
    localState.next = localState.master->next;
    localState.master->next.initialize(&localState);
  }
  
};


} //namespace Graph
} //namespace Galois
