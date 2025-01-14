#include "katana/GraphTopology.h"

#include <iostream>

#include "katana/Logging.h"
#include "katana/PropertyGraph.h"

void
katana::GraphTopology::Print() noexcept {
  auto print_array = [](const auto& arr, const auto& name) {
    std::cout << name << ": [ ";
    for (const auto& i : arr) {
      std::cout << i << ", ";
    }
    std::cout << "]" << std::endl;
  };

  print_array(adj_indices_, "adj_indices_");
  print_array(dests_, "dests_");
}

katana::GraphTopology::GraphTopology(
    const Edge* adj_indices, size_t num_nodes, const Node* dests,
    size_t num_edges) noexcept {
  adj_indices_.allocateInterleaved(num_nodes);
  dests_.allocateInterleaved(num_edges);

  katana::ParallelSTL::copy(
      &adj_indices[0], &adj_indices[num_nodes], adj_indices_.begin());
  katana::ParallelSTL::copy(&dests[0], &dests[num_edges], dests_.begin());
}

katana::GraphTopology
katana::GraphTopology::Copy(const GraphTopology& that) noexcept {
  return katana::GraphTopology(
      that.adj_indices_.data(), that.adj_indices_.size(), that.dests_.data(),
      that.dests_.size());
}

std::unique_ptr<katana::ShuffleTopology>
katana::ShuffleTopology::MakeFrom(
    const PropertyGraph*, const katana::EdgeShuffleTopology&) noexcept {
  KATANA_LOG_FATAL("Not implemented yet");
  std::unique_ptr<ShuffleTopology> ret;
  return ret;
}

std::unique_ptr<katana::EdgeShuffleTopology>
katana::EdgeShuffleTopology::MakeTransposeCopy(
    const katana::PropertyGraph* pg) {
  KATANA_LOG_DEBUG_ASSERT(pg);

  const auto& topology = pg->topology();
  if (topology.empty()) {
    EdgeShuffleTopology et;
    et.tpose_state_ = TransposeKind::kYes;
    return std::make_unique<EdgeShuffleTopology>(std::move(et));
  }

  GraphTopologyTypes::AdjIndexVec out_indices;
  GraphTopologyTypes::EdgeDestVec out_dests;
  GraphTopologyTypes::PropIndexVec edge_prop_indices;
  GraphTopologyTypes::AdjIndexVec out_dests_offset;

  out_indices.allocateInterleaved(topology.num_nodes());
  out_dests.allocateInterleaved(topology.num_edges());
  edge_prop_indices.allocateInterleaved(topology.num_edges());
  out_dests_offset.allocateInterleaved(topology.num_nodes());

  katana::ParallelSTL::fill(out_indices.begin(), out_indices.end(), Edge{0});

  // Keep a copy of old destinaton ids and compute number of
  // in-coming edges for the new prefix sum of out_indices.
  katana::do_all(
      katana::iterate(topology.all_edges()),
      [&](Edge e) {
        // Counting outgoing edges in the tranpose graph by
        // counting incoming edges in the original graph
        auto dest = topology.edge_dest(e);
        __sync_add_and_fetch(&(out_indices[dest]), 1);
      },
      katana::no_stats());

  // Prefix sum calculation of the edge index array
  katana::ParallelSTL::partial_sum(
      out_indices.begin(), out_indices.end(), out_indices.begin());

  // temporary buffer for storing the starting point of each node's transpose
  // adjacency
  out_dests_offset[0] = 0;
  katana::do_all(
      katana::iterate(Edge{1}, Edge{topology.num_nodes()}),
      [&](Edge n) { out_dests_offset[n] = out_indices[n - 1]; },
      katana::no_stats());

  // Update out_dests with the new destination ids
  // of the transposed graphs
  katana::do_all(
      katana::iterate(topology.all_nodes()),
      [&](auto src) {
        // get all outgoing edges of a particular
        // node and reverse the edges.
        for (GraphTopology::Edge e : topology.edges(src)) {
          // e = start index into edge array for a particular node
          // Destination node
          auto dest = topology.edge_dest(e);
          // Location to save edge
          auto e_new = __sync_fetch_and_add(&(out_dests_offset[dest]), 1);
          // Save src as destination
          out_dests[e_new] = src;
          // remember the original edge ID to look up properties
          edge_prop_indices[e_new] = e;
        }
      },
      katana::steal(), katana::no_stats());

  return std::make_unique<EdgeShuffleTopology>(EdgeShuffleTopology{
      TransposeKind::kYes, EdgeSortKind::kAny, std::move(out_indices),
      std::move(out_dests), std::move(edge_prop_indices)});
}

std::unique_ptr<katana::EdgeShuffleTopology>
katana::EdgeShuffleTopology::MakeOriginalCopy(const katana::PropertyGraph* pg) {
  GraphTopology copy_topo = GraphTopology::Copy(pg->topology());

  GraphTopologyTypes::PropIndexVec edge_prop_indices;
  edge_prop_indices.allocateInterleaved(copy_topo.num_edges());
  katana::ParallelSTL::iota(
      edge_prop_indices.begin(), edge_prop_indices.end(), Edge{0});

  return std::make_unique<EdgeShuffleTopology>(EdgeShuffleTopology{
      TransposeKind::kNo, EdgeSortKind::kAny,
      std::move(copy_topo.GetAdjIndices()), std::move(copy_topo.GetDests()),
      std::move(edge_prop_indices)});
}

katana::GraphTopologyTypes::edge_iterator
katana::EdgeShuffleTopology::find_edge(
    const katana::GraphTopologyTypes::Node& src,
    const katana::GraphTopologyTypes::Node& dst) const noexcept {
  auto e_range = edges(src);

  constexpr size_t kBinarySearchThreshold = 64;

  if (e_range.size() > kBinarySearchThreshold &&
      !has_edges_sorted_by(EdgeSortKind::kSortedByDestID)) {
    KATANA_WARN_ONCE(
        "find_edge(): expect poor performance. Edges not sorted by Dest ID");
  }

  if (e_range.size() <= kBinarySearchThreshold) {
    auto iter = std::find_if(
        e_range.begin(), e_range.end(),
        [&](const GraphTopology::Edge& e) { return edge_dest(e) == dst; });

    return iter;

  } else {
    auto iter = std::lower_bound(
        e_range.begin(), e_range.end(), dst,
        internal::EdgeDestComparator<EdgeShuffleTopology>{this});

    return edge_dest(*iter) == dst ? iter : e_range.end();
  }
}

katana::GraphTopologyTypes::edges_range
katana::EdgeShuffleTopology::find_edges(
    const katana::GraphTopologyTypes::Node& src,
    const katana::GraphTopologyTypes::Node& dst) const noexcept {
  auto e_range = edges(src);
  if (e_range.empty()) {
    return e_range;
  }

  KATANA_LOG_VASSERT(
      !has_edges_sorted_by(EdgeSortKind::kSortedByDestID),
      "Must have edges sorted by kSortedByDestID");

  internal::EdgeDestComparator<EdgeShuffleTopology> comp{this};
  auto [first_it, last_it] =
      std::equal_range(e_range.begin(), e_range.end(), dst, comp);

  if (first_it == e_range.end() || edge_dest(*first_it) != dst) {
    // return empty range
    return MakeStandardRange(e_range.end(), e_range.end());
  }

  auto ret_range = MakeStandardRange(first_it, last_it);
  for ([[maybe_unused]] auto e : ret_range) {
    KATANA_LOG_DEBUG_ASSERT(edge_dest(e) == dst);
  }
  return ret_range;
}

void
katana::EdgeShuffleTopology::SortEdgesByDestID() noexcept {
  katana::do_all(
      katana::iterate(Base::all_nodes()),
      [&](Node node) {
        // get this node's first and last edge
        auto e_beg = *Base::edges(node).begin();
        auto e_end = *Base::edges(node).end();

        // get iterators to locations to sort in the vector
        auto begin_sort_iter = katana::make_zip_iterator(
            edge_prop_indices_.begin() + e_beg,
            Base::GetDests().begin() + e_beg);

        auto end_sort_iter = katana::make_zip_iterator(
            edge_prop_indices_.begin() + e_end,
            Base::GetDests().begin() + e_end);

        // rearrange vector indices based on how the destinations of this
        // graph will eventually be sorted sort function not based on vector
        // being passed, but rather the type and destination of the graph
        std::sort(
            begin_sort_iter, end_sort_iter,
            [&](const auto& tup1, const auto& tup2) {
              auto dst1 = std::get<1>(tup1);
              auto dst2 = std::get<1>(tup2);
              static_assert(
                  std::is_same_v<decltype(dst1), GraphTopology::Node>);
              static_assert(
                  std::is_same_v<decltype(dst2), GraphTopology::Node>);
              return dst1 < dst2;
            });
      },
      katana::steal(), katana::no_stats());
  // remember to update sort state
  edge_sort_state_ = EdgeSortKind::kSortedByDestID;
}

void
katana::EdgeShuffleTopology::SortEdgesByTypeThenDest(
    const PropertyGraph* pg) noexcept {
  katana::do_all(
      katana::iterate(Base::all_nodes()),
      [&](Node node) {
        // get this node's first and last edge
        auto e_beg = *Base::edges(node).begin();
        auto e_end = *Base::edges(node).end();

        // get iterators to locations to sort in the vector
        auto begin_sort_iter = katana::make_zip_iterator(
            edge_prop_indices_.begin() + e_beg,
            Base::GetDests().begin() + e_beg);

        auto end_sort_iter = katana::make_zip_iterator(
            edge_prop_indices_.begin() + e_end,
            Base::GetDests().begin() + e_end);

        // rearrange vector indices based on how the destinations of this
        // graph will eventually be sorted sort function not based on vector
        // being passed, but rather the type and destination of the graph
        std::sort(
            begin_sort_iter, end_sort_iter,
            [&](const auto& tup1, const auto& tup2) {
              // get edge type and destinations
              auto e1 = std::get<0>(tup1);
              auto e2 = std::get<0>(tup2);
              static_assert(
                  std::is_same_v<decltype(e1), GraphTopology::PropertyIndex>);
              static_assert(
                  std::is_same_v<decltype(e2), GraphTopology::PropertyIndex>);

              EntityType data1 = pg->GetTypeOfEdge(e1);
              EntityType data2 = pg->GetTypeOfEdge(e2);
              if (data1 != data2) {
                return data1 < data2;
              }

              auto dst1 = std::get<1>(tup1);
              auto dst2 = std::get<1>(tup2);
              static_assert(
                  std::is_same_v<decltype(dst1), GraphTopology::Node>);
              static_assert(
                  std::is_same_v<decltype(dst2), GraphTopology::Node>);
              return dst1 < dst2;
            });
      },
      katana::steal(), katana::no_stats());

  // remember to update sort state
  edge_sort_state_ = EdgeSortKind::kSortedByEdgeType;
}

void
katana::EdgeShuffleTopology::SortEdgesByDestType(
    const PropertyGraph*,
    const katana::GraphTopologyTypes::PropIndexVec&) noexcept {
  KATANA_LOG_FATAL("Not implemented yet");
}

std::unique_ptr<katana::ShuffleTopology>
katana::ShuffleTopology::MakeSortedByDegree(
    const PropertyGraph*,
    const katana::EdgeShuffleTopology& seed_topo) noexcept {
  auto cmp = [&](const auto& i1, const auto& i2) {
    auto d1 = seed_topo.degree(i1);
    auto d2 = seed_topo.degree(i2);
    if (d1 == d2) {
      return d1 < d2;
    }
    return d1 < d2;
  };

  return MakeNodeSortedTopo(seed_topo, cmp, NodeSortKind::kSortedByDegree);
}

std::unique_ptr<katana::ShuffleTopology>
katana::ShuffleTopology::MakeSortedByNodeType(
    const PropertyGraph* pg,
    const katana::EdgeShuffleTopology& seed_topo) noexcept {
  auto cmp = [&](const auto& i1, const auto& i2) {
    auto k1 = pg->GetTypeOfNode(i1);
    auto k2 = pg->GetTypeOfNode(i2);
    if (k1 == k2) {
      return i1 < i2;
    }
    return k1 < k2;
  };

  return MakeNodeSortedTopo(seed_topo, cmp, NodeSortKind::kSortedByNodeType);
}

std::unique_ptr<katana::CondensedTypeIDMap>
katana::CondensedTypeIDMap::MakeFromEdgeTypes(
    const katana::PropertyGraph* pg) noexcept {
  TypeIDToIndexMap edge_type_to_index;
  IndexToTypeIDMap edge_index_to_type;

  katana::PerThreadStorage<katana::gstl::Set<EntityType>> edgeTypes;

  const auto& topo = pg->topology();

  katana::do_all(
      katana::iterate(Edge{0}, topo.num_edges()),
      [&](const Edge& e) {
        EntityType type = pg->GetTypeOfEdge(e);
        edgeTypes.getLocal()->insert(type);
      },
      katana::no_stats());

  // ordered map
  std::set<EntityType> mergedSet;
  for (uint32_t i = 0; i < katana::activeThreads; ++i) {
    auto& edgeTypesSet = *edgeTypes.getRemote(i);
    for (auto edgeType : edgeTypesSet) {
      mergedSet.insert(edgeType);
    }
  }

  // unordered map
  uint32_t num_edge_types = 0u;
  for (const auto& edgeType : mergedSet) {
    edge_type_to_index[edgeType] = num_edge_types++;
    edge_index_to_type.emplace_back(edgeType);
  }

  // TODO(amber): introduce a per-thread-container type that frees memory
  // correctly
  katana::on_each([&](unsigned, unsigned) {
    // free up memory by resetting
    *edgeTypes.getLocal() = gstl::Set<EntityType>();
  });

  return std::make_unique<CondensedTypeIDMap>(CondensedTypeIDMap{
      std::move(edge_type_to_index), std::move(edge_index_to_type)});
}

katana::EdgeTypeAwareTopology::AdjIndexVec
katana::EdgeTypeAwareTopology::CreatePerEdgeTypeAdjacencyIndex(
    const PropertyGraph* pg, const CondensedTypeIDMap* edge_type_index,
    const EdgeShuffleTopology* e_topo) noexcept {
  if (e_topo->num_nodes() == 0) {
    KATANA_LOG_VASSERT(
        e_topo->num_edges() == 0, "Found graph with edges but no nodes");
    return AdjIndexVec{};
  }

  if (edge_type_index->num_unique_types() == 0) {
    KATANA_LOG_VASSERT(
        e_topo->num_edges() == 0, "Found graph with edges but no edge types");
    // Graph has some nodes but no edges.
    return AdjIndexVec{};
  }

  const size_t sz = e_topo->num_nodes() * edge_type_index->num_unique_types();
  AdjIndexVec adj_indices;
  adj_indices.allocateInterleaved(sz);

  katana::do_all(
      katana::iterate(e_topo->all_nodes()),
      [&](Node N) {
        auto offset = N * edge_type_index->num_unique_types();
        uint32_t index = 0;
        for (auto e : e_topo->edges(N)) {
          // Since we sort the edges, we must use the
          // edge_property_index because EdgeShuffleTopology rearranges the edges
          const auto type = pg->GetTypeOfEdge(e_topo->edge_property_index(e));
          while (type != edge_type_index->GetType(index)) {
            adj_indices[offset + index] = e;
            index++;
            KATANA_LOG_DEBUG_ASSERT(
                index < edge_type_index->num_unique_types());
          }
        }
        auto e = *e_topo->edges(N).end();
        while (index < edge_type_index->num_unique_types()) {
          adj_indices[offset + index] = e;
          index++;
        }
      },
      katana::no_stats(), katana::steal());

  return adj_indices;
}

std::unique_ptr<katana::EdgeTypeAwareTopology>
katana::EdgeTypeAwareTopology::MakeFrom(
    const katana::PropertyGraph* pg,
    const katana::CondensedTypeIDMap* edge_type_index,
    const katana::EdgeShuffleTopology* e_topo) noexcept {
  KATANA_LOG_DEBUG_ASSERT(e_topo->has_edges_sorted_by(
      EdgeShuffleTopology::EdgeSortKind::kSortedByEdgeType));

  KATANA_LOG_DEBUG_ASSERT(e_topo->num_edges() == pg->topology().num_edges());

  AdjIndexVec per_type_adj_indices =
      CreatePerEdgeTypeAdjacencyIndex(pg, edge_type_index, e_topo);

  return std::make_unique<EdgeTypeAwareTopology>(EdgeTypeAwareTopology{
      pg, edge_type_index, e_topo, std::move(per_type_adj_indices)});
}

const katana::GraphTopology*
katana::PGViewCache::GetOriginalTopology(
    const PropertyGraph* pg) const noexcept {
  return &pg->topology();
}

katana::CondensedTypeIDMap*
katana::PGViewCache::BuildOrGetEdgeTypeIndex(
    const katana::PropertyGraph* pg) noexcept {
  if (edge_type_id_map_ && edge_type_id_map_->is_valid()) {
    return edge_type_id_map_.get();
  }

  edge_type_id_map_ = CondensedTypeIDMap::MakeFromEdgeTypes(pg);
  KATANA_LOG_DEBUG_ASSERT(edge_type_id_map_);
  return edge_type_id_map_.get();
};

template <typename Topo>
[[maybe_unused]] bool
CheckTopology(const katana::PropertyGraph* pg, const Topo* t) noexcept {
  return (pg->num_nodes() == t->num_nodes()) &&
         (pg->num_edges() == t->num_edges());
}

katana::EdgeShuffleTopology*
katana::PGViewCache::BuildOrGetEdgeShuffTopo(
    const katana::PropertyGraph* pg,
    const katana::EdgeShuffleTopology::TransposeKind& tpose_kind,
    const katana::EdgeShuffleTopology::EdgeSortKind& sort_kind) noexcept {
  auto pred = [&](const auto& topo_ptr) {
    return topo_ptr->is_valid() && topo_ptr->has_transpose_state(tpose_kind) &&
           topo_ptr->has_edges_sorted_by(sort_kind);
  };
  auto it =
      std::find_if(edge_shuff_topos_.begin(), edge_shuff_topos_.end(), pred);

  if (it != edge_shuff_topos_.end()) {
    KATANA_LOG_DEBUG_ASSERT(CheckTopology(pg, it->get()));
    return it->get();
  } else {
    edge_shuff_topos_.emplace_back(
        EdgeShuffleTopology::Make(pg, tpose_kind, sort_kind));
    KATANA_LOG_DEBUG_ASSERT(CheckTopology(pg, edge_shuff_topos_.back().get()));
    return edge_shuff_topos_.back().get();
  }
}

katana::ShuffleTopology*
katana::PGViewCache::BuildOrGetShuffTopo(
    const katana::PropertyGraph* pg,
    const katana::EdgeShuffleTopology::TransposeKind& tpose_kind,
    const katana::ShuffleTopology::NodeSortKind& node_sort_todo,
    const katana::EdgeShuffleTopology::EdgeSortKind& edge_sort_todo) noexcept {
  auto pred = [&](const auto& topo_ptr) {
    return topo_ptr->is_valid() && topo_ptr->has_transpose_state(tpose_kind) &&
           topo_ptr->has_edges_sorted_by(edge_sort_todo) &&
           topo_ptr->has_nodes_sorted_by(node_sort_todo);
  };

  auto it =
      std::find_if(fully_shuff_topos_.begin(), fully_shuff_topos_.end(), pred);

  if (it != fully_shuff_topos_.end()) {
    KATANA_LOG_DEBUG_ASSERT(CheckTopology(pg, it->get()));
    return it->get();
  } else {
    auto e_topo = BuildOrGetEdgeShuffTopo(pg, tpose_kind, edge_sort_todo);
    KATANA_LOG_DEBUG_ASSERT(e_topo->has_transpose_state(tpose_kind));

    fully_shuff_topos_.emplace_back(ShuffleTopology::MakeFromTopo(
        pg, *e_topo, node_sort_todo, edge_sort_todo));

    KATANA_LOG_DEBUG_ASSERT(CheckTopology(pg, fully_shuff_topos_.back().get()));
    return fully_shuff_topos_.back().get();
  }
}

katana::EdgeTypeAwareTopology*
katana::PGViewCache::BuildOrGetEdgeTypeAwareTopo(
    const katana::PropertyGraph* pg,
    const katana::EdgeShuffleTopology::TransposeKind& tpose_kind) noexcept {
  auto pred = [&](const auto& topo_ptr) {
    return topo_ptr->is_valid() && topo_ptr->has_transpose_state(tpose_kind);
  };
  auto it = std::find_if(
      edge_type_aware_topos_.begin(), edge_type_aware_topos_.end(), pred);

  if (it != edge_type_aware_topos_.end()) {
    KATANA_LOG_DEBUG_ASSERT(CheckTopology(pg, it->get()));
    return it->get();
  } else {
    auto sorted_topo = BuildOrGetEdgeShuffTopo(
        pg, tpose_kind, EdgeShuffleTopology::EdgeSortKind::kSortedByEdgeType);
    auto edge_type_index = BuildOrGetEdgeTypeIndex(pg);
    edge_type_aware_topos_.emplace_back(
        EdgeTypeAwareTopology::MakeFrom(pg, edge_type_index, sorted_topo));

    KATANA_LOG_DEBUG_ASSERT(
        CheckTopology(pg, edge_type_aware_topos_.back().get()));
    return edge_type_aware_topos_.back().get();
  }
}
