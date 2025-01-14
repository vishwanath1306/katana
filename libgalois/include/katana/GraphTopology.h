#ifndef KATANA_LIBGALOIS_KATANA_GRAPHTOPOLOGY_H_
#define KATANA_LIBGALOIS_KATANA_GRAPHTOPOLOGY_H_

#include <utility>
#include <vector>

#include <boost/iterator/counting_iterator.hpp>

#include "katana/Iterators.h"
#include "katana/NUMAArray.h"
#include "katana/config.h"

namespace katana {

class KATANA_EXPORT PropertyGraph;

// TODO(amber): None of the topologies or views or PGViewCache can keep a member
// pointer to PropertyGraph because PropertyGraph can be moved. This issue plagues
// TypedPropertyGraph and TypedPropertyGraphView as well. If we really need to keep
// a pointer to parent PropertyGraph (which may be a good idea), we need to make
// PropertyGraph non-movable and non-copyable

/// Types used by all topologies
struct KATANA_EXPORT GraphTopologyTypes {
  using Node = uint32_t;
  using Edge = uint64_t;
  using PropertyIndex = uint64_t;
  using EntityType = uint8_t;
  using node_iterator = boost::counting_iterator<Node>;
  using edge_iterator = boost::counting_iterator<Edge>;
  using nodes_range = StandardRange<node_iterator>;
  using edges_range = StandardRange<edge_iterator>;
  using iterator = node_iterator;

  using AdjIndexVec = NUMAArray<Edge>;
  using EdgeDestVec = NUMAArray<Node>;
  using PropIndexVec = NUMAArray<PropertyIndex>;
  using EntityTypeVec = NUMAArray<EntityType>;
};

class KATANA_EXPORT EdgeShuffleTopology;
class KATANA_EXPORT EdgeTypeAwareTopology;

/// A graph topology represents the adjacency information for a graph in CSR
/// format.
class KATANA_EXPORT GraphTopology : public GraphTopologyTypes {
public:
  GraphTopology() = default;
  GraphTopology(GraphTopology&&) = default;
  GraphTopology& operator=(GraphTopology&&) = default;

  GraphTopology(const GraphTopology&) = delete;
  GraphTopology& operator=(const GraphTopology&) = delete;

  GraphTopology(
      const Edge* adj_indices, size_t num_nodes, const Node* dests,
      size_t num_edges) noexcept;

  GraphTopology(NUMAArray<Edge>&& adj_indices, NUMAArray<Node>&& dests) noexcept
      : adj_indices_(std::move(adj_indices)), dests_(std::move(dests)) {}

  static GraphTopology Copy(const GraphTopology& that) noexcept;

  uint64_t num_nodes() const noexcept { return adj_indices_.size(); }

  uint64_t num_edges() const noexcept { return dests_.size(); }

  const Edge* adj_data() const noexcept { return adj_indices_.data(); }

  const Node* dest_data() const noexcept { return dests_.data(); }

  /// Checks equality against another instance of GraphTopology.
  /// WARNING: Expensive operation due to element-wise checks on large arrays
  /// @param that: GraphTopology instance to compare against
  /// @returns true if topology arrays are equal
  bool Equals(const GraphTopology& that) const noexcept {
    if (this == &that) {
      return true;
    }
    if (num_nodes() != that.num_nodes()) {
      return false;
    }
    if (num_edges() != that.num_edges()) {
      return false;
    }

    return adj_indices_ == that.adj_indices_ && dests_ == that.dests_;
  }

  /// Gets the edge range of some node.
  ///
  /// \param node node to get the edge range of
  /// \returns iterable edge range for node.
  edges_range edges(Node node) const noexcept {
    KATANA_LOG_DEBUG_ASSERT(node <= adj_indices_.size());
    edge_iterator e_beg{node > 0 ? adj_indices_[node - 1] : 0};
    edge_iterator e_end{adj_indices_[node]};

    return MakeStandardRange(e_beg, e_end);
  }

  Node edge_dest(Edge edge_id) const noexcept {
    KATANA_LOG_DEBUG_ASSERT(edge_id < dests_.size());
    return dests_[edge_id];
  }

  nodes_range nodes(Node begin, Node end) const noexcept {
    return MakeStandardRange<node_iterator>(begin, end);
  }

  nodes_range all_nodes() const noexcept {
    return nodes(Node{0}, static_cast<Node>(num_nodes()));
  }

  edges_range all_edges() const noexcept {
    return MakeStandardRange<edge_iterator>(Edge{0}, Edge{num_edges()});
  }
  // Standard container concepts

  node_iterator begin() const noexcept { return node_iterator(0); }

  node_iterator end() const noexcept { return node_iterator(num_nodes()); }

  size_t size() const noexcept { return num_nodes(); }

  bool empty() const noexcept { return num_nodes() == 0; }

  ///@param node node to get degree for
  ///@returns Degree of node N
  size_t degree(Node node) const noexcept { return edges(node).size(); }

  PropertyIndex edge_property_index(const Edge& eid) const noexcept {
    return eid;
  }

  PropertyIndex node_property_index(const Node& nid) const noexcept {
    return nid;
  }

  void Print() noexcept;

private:
  // need these friend relationships to construct instances of friend classes below
  // by moving NUMAArrays in this class.
  friend class EdgeShuffleTopology;
  friend class EdgeTypeAwareTopology;

  NUMAArray<Edge>& GetAdjIndices() noexcept { return adj_indices_; }
  NUMAArray<Node>& GetDests() noexcept { return dests_; }

private:
  NUMAArray<Edge> adj_indices_;
  NUMAArray<Node> dests_;
};

// TODO(amber): In the future, when we group properties e.g., by node or edge type,
// this class might get merged with ShuffleTopology. Not doing it at the moment to
// avoid having to keep unnecessary arrays like node_property_indices_
class KATANA_EXPORT EdgeShuffleTopology : public GraphTopology {
  using Base = GraphTopology;

public:
  EdgeShuffleTopology() = default;
  EdgeShuffleTopology(EdgeShuffleTopology&&) = default;
  EdgeShuffleTopology& operator=(EdgeShuffleTopology&&) = default;

  EdgeShuffleTopology(const EdgeShuffleTopology&) = delete;
  EdgeShuffleTopology& operator=(const EdgeShuffleTopology&) = delete;

  enum class TransposeKind : int { kNo = 0, kYes };

  enum class EdgeSortKind : int {
    kAny = 0,  // don't care. Sorted or Unsorted
    kSortedByDestID,
    kSortedByEdgeType,
    kSortedByNodeType
  };

  bool is_transposed() const noexcept {
    return has_transpose_state(TransposeKind::kYes);
  }

  bool has_transpose_state(const TransposeKind& expected) const noexcept {
    return tpose_state_ == expected;
  }

  TransposeKind transpose_state() const noexcept { return tpose_state_; }
  EdgeSortKind edge_sort_state() const noexcept { return edge_sort_state_; }

  bool is_valid() const noexcept { return is_valid_; }

  void invalidate() noexcept { is_valid_ = false; }

  bool has_edges_sorted_by(const EdgeSortKind& kind) const noexcept {
    if (kind == EdgeSortKind::kAny) {
      return true;
    }
    return edge_sort_state_ == kind;
  }

  PropertyIndex edge_property_index(const Edge& eid) const noexcept {
    KATANA_LOG_DEBUG_ASSERT(eid < num_edges());
    return edge_prop_indices_[eid];
  }

  static std::unique_ptr<EdgeShuffleTopology> MakeTransposeCopy(
      const PropertyGraph* pg);
  static std::unique_ptr<EdgeShuffleTopology> MakeOriginalCopy(
      const PropertyGraph* pg);

  static std::unique_ptr<EdgeShuffleTopology> Make(
      const PropertyGraph* pg, const TransposeKind& tpose_todo,
      const EdgeSortKind& edge_sort_todo) noexcept {
    std::unique_ptr<EdgeShuffleTopology> ret;

    if (tpose_todo == TransposeKind::kYes) {
      ret = MakeTransposeCopy(pg);
      KATANA_LOG_DEBUG_ASSERT(ret->has_transpose_state(TransposeKind::kYes));
    } else {
      ret = MakeOriginalCopy(pg);
      KATANA_LOG_DEBUG_ASSERT(ret->has_transpose_state(TransposeKind::kNo));
    }

    ret->sortEdges(pg, edge_sort_todo);
    return ret;
  }

  edge_iterator find_edge(const Node& src, const Node& dst) const noexcept;

  edges_range find_edges(const Node& src, const Node& dst) const noexcept;

  bool has_edge(const Node& src, const Node& dst) const noexcept {
    return find_edge(src, dst) != edges(src).end();
  }

protected:
  void SortEdgesByDestID() noexcept;

  void SortEdgesByTypeThenDest(const PropertyGraph* pg) noexcept;

  void SortEdgesByDestType(
      const PropertyGraph* pg, const PropIndexVec& node_prop_indices) noexcept;

  void sortEdges(
      const PropertyGraph* pg, const EdgeSortKind& edge_sort_todo) noexcept {
    switch (edge_sort_todo) {
    case EdgeSortKind::kAny:
      return;
    case EdgeSortKind::kSortedByDestID:
      SortEdgesByDestID();
      return;
    case EdgeSortKind::kSortedByEdgeType:
      SortEdgesByTypeThenDest(pg);
      return;
    case EdgeSortKind::kSortedByNodeType:
      KATANA_LOG_FATAL("Not implemented yet");
      return;
    default:
      KATANA_LOG_FATAL("switch-case fell through");
      return;
    }
  }

  EdgeShuffleTopology(
      const TransposeKind& tpose_todo, const EdgeSortKind& edge_sort_todo,
      AdjIndexVec&& adj_indices, EdgeDestVec&& dests,
      PropIndexVec&& edge_prop_indices) noexcept
      : Base(std::move(adj_indices), std::move(dests)),
        is_valid_(true),
        tpose_state_(tpose_todo),
        edge_sort_state_(edge_sort_todo),
        edge_prop_indices_(std::move(edge_prop_indices)) {
    KATANA_LOG_DEBUG_ASSERT(edge_prop_indices_.size() == num_edges());
  }

private:
  bool is_valid_ = true;
  TransposeKind tpose_state_ = TransposeKind::kNo;
  EdgeSortKind edge_sort_state_ = EdgeSortKind::kAny;

  // TODO(amber): In the future, we may need to keep a copy of edge_type_ids in
  // addition to edge_prop_indices_. Today, we assume that we can use
  // PropertyGraph.edge_type_set_id(edge_prop_indices_[edge_id]) to obtain
  // edge_type_id. This may not be true when we group properties
  PropIndexVec edge_prop_indices_;
};

/// This is a fully shuffled topology where both the nodes and edges can be sorted
class KATANA_EXPORT ShuffleTopology : public EdgeShuffleTopology {
  using Base = EdgeShuffleTopology;

public:
  enum class NodeSortKind : int {
    kAny = 0,
    kSortedByDegree,
    kSortedByNodeType,
  };

  PropertyIndex node_property_index(const Node& nid) const noexcept {
    KATANA_LOG_DEBUG_ASSERT(nid < num_nodes());
    return node_prop_indices_[nid];
  }

  bool has_nodes_sorted_by(const NodeSortKind& kind) const noexcept {
    if (kind == NodeSortKind::kAny) {
      return true;
    }
    return node_sort_state_ == kind;
  }

  static std::unique_ptr<ShuffleTopology> MakeFrom(
      const PropertyGraph* pg, const EdgeShuffleTopology& seed_topo) noexcept;

  static std::unique_ptr<ShuffleTopology> MakeSortedByDegree(
      const PropertyGraph* pg, const EdgeShuffleTopology& seed_topo) noexcept;

  static std::unique_ptr<ShuffleTopology> MakeSortedByNodeType(
      const PropertyGraph* pg, const EdgeShuffleTopology& seed_topo) noexcept;

  static std::unique_ptr<ShuffleTopology> MakeFromTopo(
      const PropertyGraph* pg, const EdgeShuffleTopology& seed_topo,
      const NodeSortKind& node_sort_todo,
      const EdgeSortKind& edge_sort_todo) noexcept {
    std::unique_ptr<ShuffleTopology> ret;

    switch (node_sort_todo) {
    case NodeSortKind::kAny:
      ret = MakeFrom(pg, seed_topo);
      break;
    case NodeSortKind::kSortedByDegree:
      ret = MakeSortedByDegree(pg, seed_topo);
      break;
    case NodeSortKind::kSortedByNodeType:
      ret = MakeSortedByNodeType(pg, seed_topo);
      break;
    default:
      KATANA_LOG_FATAL("switch case fell through");
    }

    ret->sortEdges(pg, edge_sort_todo);

    return ret;
  }

private:
  template <typename CmpFunc>
  static std::unique_ptr<ShuffleTopology> MakeNodeSortedTopo(
      const EdgeShuffleTopology& seed_topo, const CmpFunc& cmp,
      const NodeSortKind& node_sort_todo) {
    GraphTopology::PropIndexVec node_prop_indices;
    node_prop_indices.allocateInterleaved(seed_topo.num_nodes());

    katana::ParallelSTL::iota(
        node_prop_indices.begin(), node_prop_indices.end(),
        GraphTopologyTypes::PropertyIndex{0});

    katana::ParallelSTL::sort(
        node_prop_indices.begin(), node_prop_indices.end(),
        [&](const auto& i1, const auto& i2) { return cmp(i1, i2); });

    GraphTopology::AdjIndexVec degrees;
    degrees.allocateInterleaved(seed_topo.num_nodes());

    katana::NUMAArray<GraphTopologyTypes::Node> old_to_new_map;
    old_to_new_map.allocateInterleaved(seed_topo.num_nodes());
    // TODO(amber): given 32-bit node ids, put a check here that
    // node_prop_indices.size() < 2^32
    katana::do_all(
        katana::iterate(size_t{0}, node_prop_indices.size()),
        [&](auto i) {
          // node_prop_indices[i] gives old node id
          old_to_new_map[node_prop_indices[i]] = i;
          degrees[i] = seed_topo.degree(node_prop_indices[i]);
        },
        katana::no_stats());

    KATANA_LOG_DEBUG_ASSERT(std::is_sorted(degrees.begin(), degrees.end()));

    katana::ParallelSTL::partial_sum(
        degrees.begin(), degrees.end(), degrees.begin());

    GraphTopologyTypes::EdgeDestVec new_dest_vec;
    new_dest_vec.allocateInterleaved(seed_topo.num_edges());

    GraphTopologyTypes::PropIndexVec edge_prop_indices;
    edge_prop_indices.allocateInterleaved(seed_topo.num_edges());

    katana::do_all(
        katana::iterate(seed_topo.all_nodes()),
        [&](auto old_srd_id) {
          auto new_srd_id = old_to_new_map[old_srd_id];
          auto new_out_index = new_srd_id > 0 ? degrees[new_srd_id - 1] : 0;

          for (auto e : seed_topo.edges(old_srd_id)) {
            auto new_edge_dest = old_to_new_map[seed_topo.edge_dest(e)];

            auto new_edge_id = new_out_index;
            ++new_out_index;
            KATANA_LOG_DEBUG_ASSERT(new_out_index <= degrees[new_srd_id]);

            new_dest_vec[new_edge_id] = new_edge_dest;

            // copy over edge_property_index mapping from old edge to new edge
            edge_prop_indices[new_edge_id] = seed_topo.edge_property_index(e);
          }
        },
        katana::steal(), katana::no_stats());

    return std::make_unique<ShuffleTopology>(ShuffleTopology{
        seed_topo.transpose_state(), node_sort_todo,
        seed_topo.edge_sort_state(), std::move(degrees),
        std::move(node_prop_indices), std::move(new_dest_vec),
        std::move(edge_prop_indices)});
  }

  ShuffleTopology(
      const TransposeKind& tpose_todo, const NodeSortKind& node_sort_todo,
      const EdgeSortKind& edge_sort_todo, AdjIndexVec&& adj_indices,
      PropIndexVec&& node_prop_indices, EdgeDestVec&& dests,
      PropIndexVec&& edge_prop_indices) noexcept
      :

        Base(
            tpose_todo, edge_sort_todo, std::move(adj_indices),
            std::move(dests), std::move(edge_prop_indices)),
        node_sort_state_(node_sort_todo),
        node_prop_indices_(std::move(node_prop_indices)) {
    KATANA_LOG_DEBUG_ASSERT(node_prop_indices_.size() == num_nodes());
  }

  NodeSortKind node_sort_state_ = NodeSortKind::kAny;

  // TODO(amber): In the future, we may need to keep a copy of node_type_ids in
  // addition to node_prop_indices_. Today, we assume that we can use
  // PropertyGraph.node_type_set_id(node_prop_indices_[node_id]) to obtain
  // node_type_id. This may not be true when we group properties
  PropIndexVec node_prop_indices_;
};

class KATANA_EXPORT CondensedTypeIDMap : public GraphTopologyTypes {
  /// map an integer id to each unique edge edge_type in the graph, such that, the
  /// integer ids assigned are contiguous, i.e., 0 .. num_unique_types-1
  using TypeIDToIndexMap = std::unordered_map<EntityType, uint32_t>;
  /// reverse map that allows looking up edge_type using its integer index
  using IndexToTypeIDMap = std::vector<EntityType>;

public:
  using EdgeTypeIDRange =
      katana::StandardRange<IndexToTypeIDMap::const_iterator>;

  CondensedTypeIDMap() = default;
  CondensedTypeIDMap(CondensedTypeIDMap&&) = default;
  CondensedTypeIDMap& operator=(CondensedTypeIDMap&&) = default;

  CondensedTypeIDMap(const CondensedTypeIDMap&) = delete;
  CondensedTypeIDMap& operator=(const CondensedTypeIDMap&) = delete;

  static std::unique_ptr<CondensedTypeIDMap> MakeFromEdgeTypes(
      const PropertyGraph* pg) noexcept;
  // TODO(amber): add MakeFromNodeTypes

  EntityType GetType(uint32_t index) const noexcept {
    KATANA_LOG_DEBUG_ASSERT(size_t(index) < index_to_type_map_.size());
    return index_to_type_map_[index];
  }

  uint32_t GetIndex(const EntityType& edge_type) const noexcept {
    KATANA_LOG_DEBUG_ASSERT(type_to_index_map_.count(edge_type) > 0);
    return type_to_index_map_.at(edge_type);
  }

  size_t num_unique_types() const noexcept { return index_to_type_map_.size(); }

  /// @param edge_type: edge_type to check
  /// @returns true iff there exists some edge in the graph with that edge_type
  bool has_edge_type_id(const EntityType& edge_type) const noexcept {
    return (type_to_index_map_.find(edge_type) != type_to_index_map_.cend());
  }

  /// Wrapper to get the distinct edge types in the graph.
  ///
  /// @returns Range of the distinct edge types
  EdgeTypeIDRange distinct_edge_type_ids() const noexcept {
    return EdgeTypeIDRange{
        index_to_type_map_.cbegin(), index_to_type_map_.cend()};
  }

  bool is_valid() const noexcept { return is_valid_; }
  void invalidate() noexcept { is_valid_ = false; };

private:
  CondensedTypeIDMap(
      TypeIDToIndexMap&& type_to_index,
      IndexToTypeIDMap&& index_to_type) noexcept
      : type_to_index_map_(std::move(type_to_index)),
        index_to_type_map_(std::move(index_to_type)),
        is_valid_(true) {
    KATANA_LOG_ASSERT(index_to_type_map_.size() == type_to_index_map_.size());
  }

  TypeIDToIndexMap type_to_index_map_;
  IndexToTypeIDMap index_to_type_map_;
  bool is_valid_ = true;
};

template <typename Topo>
class KATANA_EXPORT BasicTopologyWrapper : public GraphTopologyTypes {
public:
  explicit BasicTopologyWrapper(const Topo* t) noexcept : topo_ptr_(t) {
    KATANA_LOG_DEBUG_ASSERT(topo_ptr_);
  }

  auto num_nodes() const noexcept { return topo().num_nodes(); }

  auto num_edges() const noexcept { return topo().num_edges(); }

  /// Gets the edge range of some node.
  ///
  /// \param node node to get the edge range of
  /// \returns iterable edge range for node.
  auto edges(Node N) const noexcept { return topo().edges(N); }

  auto edge_dest(Edge eid) const noexcept { return topo().edge_dest(eid); }

  /// @param node node to get degree for
  /// @returns Degree of node N
  auto degree(Node node) const noexcept { return topo().degree(node); }

  auto nodes(Node begin, Node end) const noexcept {
    return topo().nodes(begin, end);
  }

  auto all_nodes() const noexcept { return topo().all_nodes(); }

  auto all_edges() const noexcept { return topo().all_edges(); }

  // Standard container concepts

  auto begin() const noexcept { return topo().begin(); }

  auto end() const noexcept { return topo().end(); }

  auto size() const noexcept { return topo().size(); }

  auto empty() const noexcept { return topo().empty(); }

  auto edge_property_index(const Edge& e) const noexcept {
    return topo().edge_property_index(e);
  }

  auto node_property_index(const Node& nid) const noexcept {
    return topo().node_property_index(nid);
  }

protected:
  const Topo& topo() const noexcept { return *topo_ptr_; }

private:
  const Topo* topo_ptr_;
};

namespace internal {
// TODO(amber): make private
template <typename Topo>
struct EdgeDestComparator {
  const Topo* topo_;

  bool operator()(const typename Topo::Edge& e, const typename Topo::Node& n)
      const noexcept {
    return topo_->edge_dest(e) < n;
  }

  bool operator()(const typename Topo::Node& n, const typename Topo::Edge& e)
      const noexcept {
    return n < topo_->edge_dest(e);
  }
};
}  // end namespace internal

/// store adjacency indices per each node such that they are divided by edge edge_type type.
/// Requires sorting the graph by edge edge_type type
class KATANA_EXPORT EdgeTypeAwareTopology
    : public BasicTopologyWrapper<EdgeShuffleTopology> {
  using Base = BasicTopologyWrapper<EdgeShuffleTopology>;

public:
  EdgeTypeAwareTopology(EdgeTypeAwareTopology&&) = default;
  EdgeTypeAwareTopology& operator=(EdgeTypeAwareTopology&&) = default;

  EdgeTypeAwareTopology(const EdgeTypeAwareTopology&) = delete;
  EdgeTypeAwareTopology& operator=(const EdgeTypeAwareTopology&) = delete;

  static std::unique_ptr<EdgeTypeAwareTopology> MakeFrom(
      const PropertyGraph* pg, const CondensedTypeIDMap* edge_type_index,
      const EdgeShuffleTopology* e_topo) noexcept;

  /// @param N node to get edges for
  /// @param edge_type edge_type to get edges of
  /// @returns Range to edges of node N that have edge type == edge_type
  edges_range edges(Node N, const EntityType& edge_type) const noexcept {
    // per_type_adj_indices_ is expanded so that it stores P prefix sums per node, where
    // P == edge_type_index_->num_unique_types()
    // We pick the prefix sum based on the index of the edge_type provided
    KATANA_LOG_DEBUG_ASSERT(edge_type_index_->num_unique_types() > 0);
    auto beg_idx = (N * edge_type_index_->num_unique_types()) +
                   edge_type_index_->GetIndex(edge_type);
    edge_iterator e_beg{
        (beg_idx == 0) ? 0 : per_type_adj_indices_[beg_idx - 1]};

    auto end_idx = (N * edge_type_index_->num_unique_types()) +
                   edge_type_index_->GetIndex(edge_type);
    KATANA_LOG_DEBUG_ASSERT(end_idx < per_type_adj_indices_.size());
    edge_iterator e_end{per_type_adj_indices_[end_idx]};

    return katana::MakeStandardRange(e_beg, e_end);
  }

  // C++ Derived classes hides Base class methods with the same name
  auto edges(const Node& N) const noexcept { return Base::edges(N); };

  /// @param N node to get degree for
  /// @param edge_type edge_type to get degree of
  /// @returns Degree of node N
  size_t degree(Node N, const EntityType& edge_type) const noexcept {
    return edges(N, edge_type).size();
  }

  // C++ Derived classes hides Base class methods with the same name
  auto degree(const Node& N) const noexcept { return Base::degree(N); }

  auto GetDistinctEdgeTypes() const noexcept {
    return edge_type_index_->distinct_edge_type_ids();
  }

  bool DoesEdgeTypeExist(const EntityType& edge_type) const noexcept {
    return edge_type_index_->has_edge_type_id(edge_type);
  }

  /// Returns all edges from src to dst with some edge_type.  If not found, returns
  /// empty range.
  edges_range FindAllEdgesWithType(
      Node node, Node key, const EntityType& edge_type) const noexcept {
    auto e_range = edges(node, edge_type);
    if (e_range.empty()) {
      return e_range;
    }

    internal::EdgeDestComparator<EdgeTypeAwareTopology> comp{this};
    auto [first_it, last_it] =
        std::equal_range(e_range.begin(), e_range.end(), key, comp);

    if (first_it == e_range.end() || edge_dest(*first_it) != key) {
      // return empty range
      return MakeStandardRange(e_range.end(), e_range.end());
    }

    auto ret_range = MakeStandardRange(first_it, last_it);
    for ([[maybe_unused]] auto e : ret_range) {
      KATANA_LOG_DEBUG_ASSERT(edge_dest(e) == key);
    }
    return ret_range;
  }

  /// Returns an edge iterator to an edge with some node and key by
  /// searching for the key via the node's outgoing or incoming edges.
  /// If not found, returns nothing.
  // TODO(amber): Assess the usefulness of this method. This method cannot return
  // edges of all types. Only the first found type. We should however support
  // find_edges(src, dst) or find_edge(src, dst) that doesn't care about edge type
  edges_range FindAllEdgesSingleType(Node src, Node dst) const {
    // trivial check; can't be connected if degree is 0

    auto empty_range = MakeStandardRange<edge_iterator>(Edge{0}, Edge{0});
    if (degree(src) == 0) {
      return empty_range;
    }

    // loop through all type_ids
    for (const EntityType& edge_type : GetDistinctEdgeTypes()) {
      // always use out edges (we want an id to the out edge returned)
      edges_range r = FindAllEdgesWithType(src, dst, edge_type);

      // return if something was found
      if (r) {
        return r;
      }
    }

    // not found, return empty optional
    return empty_range;
  }

  /// Check if vertex src is connected to vertex dst with the given edge edge_type
  ///
  /// @param src source node of the edge
  /// @param dst destination node of the edge
  /// @param edge_type edge_type of the edge
  /// @returns true iff the edge exists
  bool IsConnectedWithEdgeType(
      Node src, Node dst, const EntityType& edge_type) const {
    auto e_range = edges(src, edge_type);
    if (e_range.empty()) {
      return false;
    }

    internal::EdgeDestComparator<EdgeTypeAwareTopology> comp{this};
    return std::binary_search(e_range.begin(), e_range.end(), dst, comp);
  }

  /// Check if vertex src is connected to vertex dst with any edge edge_type
  ///
  /// @param src source node of the edge
  /// @param dst destination node of the edge
  /// @returns true iff the edge exists
  bool IsConnected(Node src, Node dst) const {
    // trivial check; can't be connected if degree is 0

    if (degree(src) == 0ul) {
      return false;
    }

    for (const auto& edge_type : GetDistinctEdgeTypes()) {
      if (IsConnectedWithEdgeType(src, dst, edge_type)) {
        return true;
      }
    }
    return false;
  }

  bool is_transposed() const noexcept {
    return edge_shuff_topo_->is_transposed();
  }

  bool has_transpose_state(
      const EdgeShuffleTopology::TransposeKind& k) const noexcept {
    return edge_shuff_topo_->has_transpose_state(k);
  }

  bool is_valid() const noexcept { return edge_shuff_topo_->is_valid(); }

  void invalidate() noexcept {
    const_cast<EdgeShuffleTopology*>(edge_shuff_topo_)->invalidate();
  }

private:
  // Must invoke SortAllEdgesByDataThenDst() before
  // calling this function
  static AdjIndexVec CreatePerEdgeTypeAdjacencyIndex(
      const PropertyGraph* pg, const CondensedTypeIDMap* edge_type_index,
      const EdgeShuffleTopology* topo) noexcept;

  EdgeTypeAwareTopology(
      const PropertyGraph* pg, const CondensedTypeIDMap* edge_type_index,
      const EdgeShuffleTopology* e_topo,
      AdjIndexVec&& per_type_adj_indices) noexcept
      :

        Base(e_topo),
        edge_type_index_(edge_type_index),
        edge_shuff_topo_(e_topo),
        per_type_adj_indices_(std::move(per_type_adj_indices)) {
    KATANA_LOG_ASSERT(pg);
    KATANA_LOG_DEBUG_ASSERT(edge_type_index);

    KATANA_LOG_DEBUG_ASSERT(
        per_type_adj_indices_.size() ==
        edge_shuff_topo_->num_nodes() * edge_type_index_->num_unique_types());
  }

  const CondensedTypeIDMap* edge_type_index_;
  const EdgeShuffleTopology* edge_shuff_topo_;
  AdjIndexVec per_type_adj_indices_;
};

template <typename OutTopo, typename InTopo>
class KATANA_EXPORT BasicBiDirTopoWrapper
    : public BasicTopologyWrapper<OutTopo> {
  using Base = BasicTopologyWrapper<OutTopo>;

public:
  BasicBiDirTopoWrapper(const OutTopo* out_topo, const InTopo* in_topo) noexcept
      : Base(out_topo), in_topo_(in_topo) {
    KATANA_LOG_DEBUG_ASSERT(out_topo);
    KATANA_LOG_DEBUG_ASSERT(in_topo_);

    KATANA_LOG_DEBUG_ASSERT(in_topo_->is_transposed());

    KATANA_LOG_DEBUG_ASSERT(out_topo->num_nodes() == in_topo_->num_nodes());
    KATANA_LOG_DEBUG_ASSERT(out_topo->num_edges() == in_topo_->num_edges());
  }

  auto in_edges(const GraphTopologyTypes::Node& node) const noexcept {
    return in().edges(node);
  }

  auto in_degree(const GraphTopologyTypes::Node& node) const noexcept {
    return in().degree(node);
  }

  auto in_edge_dest(const GraphTopologyTypes::Edge& edge_id) const noexcept {
    return in().edge_dest(edge_id);
  }

  auto in_edge_property_index(
      const GraphTopologyTypes::Edge& eid) const noexcept {
    return in().edge_property_index(eid);
  }

protected:
  const OutTopo& out() const noexcept { return Base::topo(); }
  const InTopo& in() const noexcept { return *in_topo_; }

private:
  const InTopo* in_topo_;
};

using SimpleBiDirTopology =
    BasicBiDirTopoWrapper<GraphTopology, EdgeShuffleTopology>;

template <typename Topo>
class SortedTopologyWrapper : public BasicTopologyWrapper<Topo> {
  using Base = BasicTopologyWrapper<Topo>;

public:
  using typename Base::Node;

  explicit SortedTopologyWrapper(const Topo* t) noexcept : Base(t) {
    KATANA_LOG_DEBUG_ASSERT(Base::topo().has_edges_sorted_by(
        EdgeShuffleTopology::EdgeSortKind::kSortedByDestID));
  }

  auto find_edge(const Node& src, const Node& dst) const noexcept {
    return Base::topo().find_edge(src, dst);
  }

  auto has_edge(const Node& src, const Node& dst) const noexcept {
    return Base::topo().has_edge(src, dst);
  }

  auto find_edges(const Node& src, const Node& dst) const noexcept {
    return Base::topo().find_edges(src, dst);
  }
};

using EdgesSortedByDestTopology = SortedTopologyWrapper<EdgeShuffleTopology>;

using NodesSortedByDegreeEdgesSortedByDestIDTopology =
    SortedTopologyWrapper<ShuffleTopology>;

class KATANA_EXPORT EdgeTypeAwareBiDirTopology
    : public BasicBiDirTopoWrapper<
          EdgeTypeAwareTopology, EdgeTypeAwareTopology> {
  using Base =
      BasicBiDirTopoWrapper<EdgeTypeAwareTopology, EdgeTypeAwareTopology>;

public:
  explicit EdgeTypeAwareBiDirTopology(
      const EdgeTypeAwareTopology* out_topo,
      const EdgeTypeAwareTopology* in_topo) noexcept
      : Base(out_topo, in_topo) {}

  auto GetDistinctEdgeTypes() const noexcept {
    return Base::out().GetDistinctEdgeTypes();
  }

  bool DoesEdgeTypeExist(const EntityType& edge_type) const noexcept {
    return Base::out().DoesEdgeTypeExist(edge_type);
  }

  auto edges(Node N, const EntityType& edge_type) const noexcept {
    return Base::out().edges(N, edge_type);
  }

  auto in_edges(Node N, const EntityType& edge_type) const noexcept {
    return Base::in().edges(N, edge_type);
  }

  auto degree(Node N, const EntityType& edge_type) const noexcept {
    return Base::out().degree(N, edge_type);
  }

  auto degree(Node N) const noexcept { return Base::out().degree(N); }

  auto in_degree(Node N, const EntityType& edge_type) const noexcept {
    return Base::in().degree(N, edge_type);
  }

  auto in_degree(Node N) const noexcept { return Base::in().degree(N); }

  auto FindAllEdgesWithType(
      const Node& src, const Node& dst,
      const EntityType& edge_type) const noexcept {
    return Base::out().FindAllEdgesWithType(src, dst, edge_type);
  }

  auto FindAllInEdgesWithType(
      const Node& src, const Node& dst,
      const EntityType& edge_type) const noexcept {
    return Base::in().FindAllEdgesWithType(src, dst, edge_type);
  }

  /// Returns an edge iterator to an edge with some node and key by
  /// searching for the key via the node's outgoing or incoming edges.
  /// If not found, returns nothing.
  edges_range FindAllEdgesSingleType(Node src, Node dst) const {
    // TODO: Similar to IsConnectedWithEdgeType, we should be able to switch
    // between searching out going topology or incoming topology. However, incoming
    // topology will return a different range of incoming edges instead of outgoing
    // edges. Can we convert easily between outing and incoming edge range
    if (Base::out().degree(src) == 0 || Base::in().degree(dst) == 0) {
      return MakeStandardRange<edge_iterator>(Edge{0}, Edge{0});
    }

    return Base::out().FindAllEdgesSingleType(src, dst);
  }

  /// Check if vertex src is connected to vertex dst with the given edge edge_type
  ///
  /// @param src source node of the edge
  /// @param dst destination node of the edge
  /// @param edge_type edge_type of the edge
  /// @returns true iff the edge exists
  bool IsConnectedWithEdgeType(
      Node src, Node dst, const EntityType& edge_type) const {
    const auto d_out = Base::out().degree(src, edge_type);
    const auto d_in = Base::in().degree(dst, edge_type);
    if (d_out == 0 || d_in == 0) {
      return false;
    }

    if (d_out < d_in) {
      return Base::out().IsConnectedWithEdgeType(src, dst, edge_type);
    } else {
      return Base::in().IsConnectedWithEdgeType(dst, src, edge_type);
    }
  }

  /// Check if vertex src is connected to vertex dst with any edge edge_type
  ///
  /// @param src source node of the edge
  /// @param dst destination node of the edge
  /// @returns true iff the edge exists
  bool IsConnected(Node src, Node dst) const {
    const auto d_out = Base::out().degree(src);
    const auto d_in = Base::in().degree(dst);
    if (d_out == 0 || d_in == 0) {
      return false;
    }

    if (d_out < d_in) {
      return Base::out().IsConnected(src, dst);
    } else {
      return Base::in().IsConnected(dst, src);
    }
  }
};

template <typename Topo>
class BasicPropGraphViewWrapper : public Topo {
  using Base = Topo;

public:
  explicit BasicPropGraphViewWrapper(
      const PropertyGraph* pg, const Topo& topo) noexcept
      : Base(topo), prop_graph_(pg) {}

  const PropertyGraph& property_graph() const noexcept { return *prop_graph_; }

private:
  const PropertyGraph* prop_graph_;
};

namespace internal {
using PGViewEdgesSortedByDestID =
    BasicPropGraphViewWrapper<EdgesSortedByDestTopology>;
using PGViewNodesSortedByDegreeEdgesSortedByDestID =
    BasicPropGraphViewWrapper<NodesSortedByDegreeEdgesSortedByDestIDTopology>;
using PGViewBiDirectional = BasicPropGraphViewWrapper<SimpleBiDirTopology>;
using PGViewEdgeTypeAwareBiDir =
    BasicPropGraphViewWrapper<EdgeTypeAwareBiDirTopology>;

template <typename PGView>
struct PGViewBuilder {};

template <>
struct PGViewBuilder<PGViewBiDirectional> {
  template <typename ViewCache>
  static internal::PGViewBiDirectional BuildView(
      const PropertyGraph* pg, ViewCache& viewCache) noexcept {
    auto tpose_topo = viewCache.BuildOrGetEdgeShuffTopo(
        pg, EdgeShuffleTopology::TransposeKind::kYes,
        EdgeShuffleTopology::EdgeSortKind::kAny);
    auto bidir_topo =
        SimpleBiDirTopology{viewCache.GetOriginalTopology(pg), tpose_topo};

    return PGViewBiDirectional{pg, bidir_topo};
  }
};

template <>
struct PGViewBuilder<PGViewEdgesSortedByDestID> {
  template <typename ViewCache>
  static PGViewEdgesSortedByDestID BuildView(
      const PropertyGraph* pg, ViewCache& viewCache) noexcept {
    auto sorted_topo = viewCache.BuildOrGetEdgeShuffTopo(
        pg, EdgeShuffleTopology::TransposeKind::kNo,
        EdgeShuffleTopology::EdgeSortKind::kSortedByDestID);

    return PGViewEdgesSortedByDestID{
        pg, EdgesSortedByDestTopology{sorted_topo}};
  }
};

template <>
struct PGViewBuilder<PGViewNodesSortedByDegreeEdgesSortedByDestID> {
  template <typename ViewCache>
  static PGViewNodesSortedByDegreeEdgesSortedByDestID BuildView(
      const PropertyGraph* pg, ViewCache& viewCache) noexcept {
    auto sorted_topo = viewCache.BuildOrGetShuffTopo(
        pg, EdgeShuffleTopology::TransposeKind::kYes,
        ShuffleTopology::NodeSortKind::kSortedByDegree,
        EdgeShuffleTopology::EdgeSortKind::kSortedByDestID);

    return PGViewNodesSortedByDegreeEdgesSortedByDestID{
        pg, NodesSortedByDegreeEdgesSortedByDestIDTopology{sorted_topo}};
  }
};

template <>
struct PGViewBuilder<PGViewEdgeTypeAwareBiDir> {
  template <typename ViewCache>
  static PGViewEdgeTypeAwareBiDir BuildView(
      const PropertyGraph* pg, ViewCache& viewCache) noexcept {
    auto out_topo = viewCache.BuildOrGetEdgeTypeAwareTopo(
        pg, EdgeShuffleTopology::TransposeKind::kNo);
    auto in_topo = viewCache.BuildOrGetEdgeTypeAwareTopo(
        pg, EdgeShuffleTopology::TransposeKind::kYes);

    return PGViewEdgeTypeAwareBiDir{
        pg, EdgeTypeAwareBiDirTopology{out_topo, in_topo}};
  }
};

}  // end namespace internal

struct PropertyGraphViews {
  using BiDirectional = internal::PGViewBiDirectional;
  using EdgesSortedByDestID = internal::PGViewEdgesSortedByDestID;
  using EdgeTypeAwareBiDir = internal::PGViewEdgeTypeAwareBiDir;
  using NodesSortedByDegreeEdgesSortedByDestID =
      internal::PGViewNodesSortedByDegreeEdgesSortedByDestID;
};

class KATANA_EXPORT PGViewCache {
  std::vector<std::unique_ptr<EdgeShuffleTopology>> edge_shuff_topos_;
  std::vector<std::unique_ptr<ShuffleTopology>> fully_shuff_topos_;
  std::vector<std::unique_ptr<EdgeTypeAwareTopology>> edge_type_aware_topos_;
  std::unique_ptr<CondensedTypeIDMap> edge_type_id_map_;
  // TODO(amber): define a node_type_id_map_;

  template <typename>
  friend struct internal::PGViewBuilder;

public:
  PGViewCache() = default;
  PGViewCache(PGViewCache&&) = default;
  PGViewCache& operator=(PGViewCache&&) = default;

  PGViewCache(const PGViewCache&) = delete;
  PGViewCache& operator=(const PGViewCache&) = delete;

  template <typename PGView>
  PGView BuildView(const PropertyGraph* pg) noexcept {
    return internal::PGViewBuilder<PGView>::BuildView(pg, *this);
  }

private:
  const GraphTopology* GetOriginalTopology(
      const PropertyGraph* pg) const noexcept;

  CondensedTypeIDMap* BuildOrGetEdgeTypeIndex(const PropertyGraph* pg) noexcept;

  EdgeShuffleTopology* BuildOrGetEdgeShuffTopo(
      const PropertyGraph* pg,
      const EdgeShuffleTopology::TransposeKind& tpose_kind,
      const EdgeShuffleTopology::EdgeSortKind& sort_kind) noexcept;

  ShuffleTopology* BuildOrGetShuffTopo(
      const PropertyGraph* pg,
      const EdgeShuffleTopology::TransposeKind& tpose_kind,
      const ShuffleTopology::NodeSortKind& node_sort_todo,
      const EdgeShuffleTopology::EdgeSortKind& edge_sort_todo) noexcept;

  EdgeTypeAwareTopology* BuildOrGetEdgeTypeAwareTopo(
      const PropertyGraph* pg,
      const EdgeShuffleTopology::TransposeKind& tpose_kind) noexcept;
};

}  // end namespace katana

#endif  // KATANA_LIBGALOIS_KATANA_GRAPHTOPOLOGY_H_
