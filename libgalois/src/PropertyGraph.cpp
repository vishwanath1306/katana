#include "katana/PropertyGraph.h"

#include <sys/mman.h>

#include <utility>

#include "katana/ArrowInterchange.h"
#include "katana/Iterators.h"
#include "katana/Logging.h"
#include "katana/Loops.h"
#include "katana/NUMAArray.h"
#include "katana/PerThreadStorage.h"
#include "katana/Platform.h"
#include "katana/Properties.h"
#include "katana/Result.h"
#include "tsuba/Errors.h"
#include "tsuba/FileFrame.h"
#include "tsuba/RDG.h"
#include "tsuba/tsuba.h"

namespace {

constexpr uint64_t
GetGraphSize(uint64_t num_nodes, uint64_t num_edges) {
  /// version, sizeof_edge_data, num_nodes, num_edges
  constexpr int mandatory_fields = 4;

  return (mandatory_fields + num_nodes) * sizeof(uint64_t) +
         (num_edges * sizeof(uint32_t));
}

[[maybe_unused]] bool
CheckTopology(
    const uint64_t* out_indices, const uint64_t num_nodes,
    const uint32_t* out_dests, const uint64_t num_edges) {
  bool has_bad_adj = false;

  katana::do_all(
      katana::iterate(uint64_t{0}, num_nodes),
      [&](auto n) {
        if (out_indices[n] > num_edges) {
          has_bad_adj = true;
        }
      },
      katana::no_stats());

  bool has_bad_dest = false;
  katana::do_all(
      katana::iterate(uint64_t{0}, num_edges),
      [&](auto e) {
        if (out_dests[e] >= num_nodes) {
          has_bad_dest = true;
        }
      },
      katana::no_stats());

  return !has_bad_adj && !has_bad_dest;
}

/// MapTopology takes a file buffer of a topology file and extracts the
/// topology files.
///
/// Format of a topology file (borrowed from the original FileGraph.cpp:
///
///   uint64_t version: 1
///   uint64_t sizeof_edge_data: size of edge data element
///   uint64_t num_nodes: number of nodes
///   uint64_t num_edges: number of edges
///   uint64_t[num_nodes] out_indices: start and end of the edges for a node
///   uint32_t[num_edges] out_dests: destinations (node indexes) of each edge
///   uint32_t padding if num_edges is odd
///   void*[num_edges] edge_data: edge data
///
/// Since property graphs store their edge data separately, we will
/// ignore the size_of_edge_data (data[1]).
katana::Result<katana::GraphTopology>
MapTopology(const tsuba::FileView& file_view) {
  const auto* data = file_view.ptr<uint64_t>();
  if (file_view.size() < 4) {
    return katana::ErrorCode::InvalidArgument;
  }

  if (data[0] != 1) {
    return katana::ErrorCode::InvalidArgument;
  }

  const uint64_t num_nodes = data[2];
  const uint64_t num_edges = data[3];

  uint64_t expected_size = GetGraphSize(num_nodes, num_edges);

  if (file_view.size() < expected_size) {
    return KATANA_ERROR(
        katana::ErrorCode::InvalidArgument, "file_view size: {} expected {}",
        file_view.size(), expected_size);
  }

  const uint64_t* out_indices = &data[4];
  const uint32_t* out_dests =
      reinterpret_cast<const uint32_t*>(out_indices + num_nodes);

  KATANA_LOG_DEBUG_ASSERT(
      CheckTopology(out_indices, num_nodes, out_dests, num_edges));
  return katana::GraphTopology(out_indices, num_nodes, out_dests, num_edges);
}

katana::Result<std::unique_ptr<tsuba::FileFrame>>
WriteTopology(const katana::GraphTopology& topology) {
  auto ff = std::make_unique<tsuba::FileFrame>();
  if (auto res = ff->Init(); !res) {
    return res.error();
  }
  const uint64_t num_nodes = topology.num_nodes();
  const uint64_t num_edges = topology.num_edges();

  uint64_t data[4] = {1, 0, num_nodes, num_edges};
  arrow::Status aro_sts = ff->Write(&data, 4 * sizeof(uint64_t));
  if (!aro_sts.ok()) {
    return tsuba::ArrowToTsuba(aro_sts.code());
  }

  if (num_nodes) {
    const auto* raw = topology.adj_data();
    static_assert(std::is_same_v<std::decay_t<decltype(*raw)>, uint64_t>);
    auto buf = arrow::Buffer::Wrap(raw, num_nodes);
    aro_sts = ff->Write(buf);
    if (!aro_sts.ok()) {
      return tsuba::ArrowToTsuba(aro_sts.code());
    }
  }

  if (num_edges) {
    const auto* raw = topology.dest_data();
    static_assert(std::is_same_v<std::decay_t<decltype(*raw)>, uint32_t>);
    auto buf = arrow::Buffer::Wrap(raw, num_edges);
    aro_sts = ff->Write(buf);
    if (!aro_sts.ok()) {
      return tsuba::ArrowToTsuba(aro_sts.code());
    }
  }
  return std::unique_ptr<tsuba::FileFrame>(std::move(ff));
}

template <typename ArrowType>
struct PropertyColumn {
  int field_index;
  std::shared_ptr<ArrowType> array;

  PropertyColumn(int i, std::shared_ptr<ArrowType>& a)
      : field_index(i), array(a) {}
};

/// Assumes all boolean or uint8 properties are types
katana::Result<katana::NUMAArray<katana::EntityTypeID>>
GetEntityTypeIDsFromProperties(
    const std::shared_ptr<arrow::Table>& properties,
    katana::EntityTypeManager* entity_type_manager) {
  // throw an error if each column/property has more than 1 chunk
  for (int i = 0, n = properties->num_columns(); i < n; i++) {
    std::shared_ptr<arrow::ChunkedArray> property = properties->column(i);
    if (property->num_chunks() != 1) {
      return KATANA_ERROR(
          katana::ErrorCode::NotImplemented,
          "property {} has {} chunks (1 chunk expected)",
          properties->schema()->field(i)->name(), property->num_chunks());
    }
  }

  // collect the list of types
  std::vector<int> type_field_indices;
  using BoolPropertyColumn = PropertyColumn<arrow::BooleanArray>;
  std::vector<BoolPropertyColumn> bool_properties;
  using UInt8PropertyColumn = PropertyColumn<arrow::UInt8Array>;
  std::vector<UInt8PropertyColumn> uint8_properties;
  const std::shared_ptr<arrow::Schema>& schema = properties->schema();
  KATANA_LOG_DEBUG_ASSERT(schema->num_fields() == properties->num_columns());
  for (int i = 0, n = schema->num_fields(); i < n; i++) {
    const std::shared_ptr<arrow::Field>& current_field = schema->field(i);

    // a bool or uint8 property is (always) considered a type
    // TODO(roshan) make this customizable by the user
    if (current_field->type()->Equals(arrow::boolean())) {
      type_field_indices.push_back(i);
      std::shared_ptr<arrow::Array> property = properties->column(i)->chunk(0);
      auto bool_property =
          std::static_pointer_cast<arrow::BooleanArray>(property);
      bool_properties.emplace_back(i, bool_property);
    } else if (current_field->type()->Equals(arrow::uint8())) {
      type_field_indices.push_back(i);
      std::shared_ptr<arrow::Array> property = properties->column(i)->chunk(0);
      auto uint8_property =
          std::static_pointer_cast<arrow::UInt8Array>(property);
      uint8_properties.emplace_back(i, uint8_property);
    }
  }

  // assign a new ID to each type
  // NB: cannot use unordered_map without defining a hash function for vectors;
  // performance is not affected here because the map is very small (<=256)
  std::map<katana::gstl::Vector<int>, katana::EntityTypeID>
      type_field_indices_to_id;
  for (int i : type_field_indices) {
    const std::shared_ptr<arrow::Field>& current_field = schema->field(i);
    const std::string& field_name = current_field->name();
    katana::EntityTypeID new_entity_type_id =
        entity_type_manager->AddAtomicEntityType(field_name);
    katana::gstl::Vector<int> field_indices = {i};
    type_field_indices_to_id.emplace(
        std::make_pair(field_indices, new_entity_type_id));
  }

  // collect the list of unique combination of types
  using FieldEntityType = katana::gstl::Vector<int>;
  // NB: cannot use unordered_set without defining a hash function for vectors;
  // performance is not affected here because the set is very small (<=256)
  using FieldEntityTypeSet = katana::gstl::Set<FieldEntityType>;
  FieldEntityTypeSet type_combinations;
  katana::PerThreadStorage<FieldEntityTypeSet> type_combinations_pts;
  katana::do_all(
      katana::iterate(int64_t{0}, properties->num_rows()), [&](int64_t row) {
        FieldEntityType field_indices;
        for (auto bool_property : bool_properties) {
          if (bool_property.array->IsValid(row) &&
              bool_property.array->Value(row)) {
            field_indices.emplace_back(bool_property.field_index);
          }
        }
        for (auto uint8_property : uint8_properties) {
          if (uint8_property.array->IsValid(row) &&
              uint8_property.array->Value(row)) {
            field_indices.emplace_back(uint8_property.field_index);
          }
        }
        if (field_indices.size() > 1) {
          FieldEntityTypeSet& local_type_combinations =
              *type_combinations_pts.getLocal();
          local_type_combinations.emplace(field_indices);
        }
      });
  for (unsigned t = 0, n = katana::activeThreads; t < n; t++) {
    FieldEntityTypeSet& remote_type_combinations =
        *type_combinations_pts.getRemote(t);
    for (auto& type_combination : remote_type_combinations) {
      type_combinations.emplace(type_combination);
    }
  }
  // deallocate PerThreadStorage in parallel
  katana::on_each([&](unsigned, unsigned) {
    FieldEntityTypeSet& local_type_combinations =
        *type_combinations_pts.getLocal();
    local_type_combinations = FieldEntityTypeSet();
  });

  // assign a new ID to each unique combination of types
  for (const FieldEntityType& field_indices : type_combinations) {
    std::vector<std::string> field_names;
    for (int i : field_indices) {
      const std::shared_ptr<arrow::Field>& current_field = schema->field(i);
      const std::string& field_name = current_field->name();
      field_names.emplace_back(field_name);
    }
    katana::EntityTypeID new_entity_type_id =
        entity_type_manager->AddNonAtomicEntityType(field_names);
    type_field_indices_to_id.emplace(
        std::make_pair(field_indices, new_entity_type_id));
  }

  // assert that all type IDs (including kUnknownEntityType) and
  // 1 special type ID (kInvalidEntityType)
  // can be stored in 8 bits
  if (entity_type_manager->GetNumEntityTypes() >
      (std::numeric_limits<katana::EntityTypeID>::max() - size_t{1})) {
    return KATANA_ERROR(
        katana::ErrorCode::NotImplemented,
        "number of unique combination of types is {} "
        "but only up to {} is supported currently",
        // exclude kUnknownEntityType
        entity_type_manager->GetNumEntityTypes() - 1,
        // exclude kUnknownEntityType and kInvalidEntityType
        std::numeric_limits<katana::EntityTypeID>::max() - 2);
  }

  // allocate type IDs array
  katana::NUMAArray<katana::EntityTypeID> entity_type_ids;
  int64_t num_rows = properties->num_rows();
  entity_type_ids.allocateInterleaved(num_rows);

  // assign the type ID for each row
  katana::do_all(katana::iterate(int64_t{0}, num_rows), [&](int64_t row) {
    FieldEntityType field_indices;
    for (auto bool_property : bool_properties) {
      if (bool_property.array->IsValid(row) &&
          bool_property.array->Value(row)) {
        field_indices.emplace_back(bool_property.field_index);
      }
    }
    for (auto uint8_property : uint8_properties) {
      if (uint8_property.array->IsValid(row) &&
          uint8_property.array->Value(row)) {
        field_indices.emplace_back(uint8_property.field_index);
      }
    }
    if (field_indices.empty()) {
      entity_type_ids[row] = katana::kUnknownEntityType;
    } else {
      katana::EntityTypeID entity_type_id =
          type_field_indices_to_id.at(field_indices);
      entity_type_ids[row] = entity_type_id;
    }
  });

  return katana::Result<katana::NUMAArray<katana::EntityTypeID>>(
      std::move(entity_type_ids));
}

katana::NUMAArray<katana::EntityTypeID>
GetUnknownEntityTypeIDs(uint64_t num_rows) {
  katana::NUMAArray<katana::EntityTypeID> entity_type_ids;
  entity_type_ids.allocateInterleaved(num_rows);
  katana::do_all(katana::iterate(uint64_t{0}, num_rows), [&](uint64_t row) {
    entity_type_ids[row] = katana::kUnknownEntityType;
  });
  return entity_type_ids;
}

}  // namespace

katana::Result<std::unique_ptr<katana::PropertyGraph>>
katana::PropertyGraph::Make(
    std::unique_ptr<tsuba::RDGFile> rdg_file, tsuba::RDG&& rdg) {
  auto topo_result = MapTopology(rdg.topology_file_storage());
  if (!topo_result) {
    return topo_result.error();
  }

  return std::make_unique<PropertyGraph>(
      std::move(rdg_file), std::move(rdg), std::move(topo_result.value()));
}

katana::Result<std::unique_ptr<katana::PropertyGraph>>
MakePropertyGraph(
    std::unique_ptr<tsuba::RDGFile> rdg_file,
    const tsuba::RDGLoadOptions& opts) {
  auto rdg_result = tsuba::RDG::Make(*rdg_file, opts);
  if (!rdg_result) {
    return rdg_result.error();
  }

  return katana::PropertyGraph::Make(
      std::move(rdg_file), std::move(rdg_result.value()));
}

katana::Result<std::unique_ptr<katana::PropertyGraph>>
katana::PropertyGraph::Make(
    const std::string& rdg_name, const tsuba::RDGLoadOptions& opts) {
  auto handle = tsuba::Open(rdg_name, tsuba::kReadWrite);
  if (!handle) {
    return handle.error();
  }

  return MakePropertyGraph(
      std::make_unique<tsuba::RDGFile>(handle.value()), opts);
}

katana::Result<std::unique_ptr<katana::PropertyGraph>>
katana::PropertyGraph::Make(katana::GraphTopology&& topo_to_assign) {
  return std::make_unique<katana::PropertyGraph>(std::move(topo_to_assign));
}

katana::Result<std::unique_ptr<katana::PropertyGraph>>
katana::PropertyGraph::Make(
    katana::GraphTopology&& topo_to_assign,
    NUMAArray<EntityTypeID>&& node_entity_type_id,
    NUMAArray<EntityTypeID>&& edge_entity_type_id,
    EntityTypeManager&& node_type_manager,
    EntityTypeManager&& edge_type_manager) {
  return std::make_unique<katana::PropertyGraph>(
      std::move(topo_to_assign), std::move(node_entity_type_id),
      std::move(edge_entity_type_id), std::move(node_type_manager),
      std::move(edge_type_manager));
}

katana::Result<std::unique_ptr<katana::PropertyGraph>>
katana::PropertyGraph::Copy() const {
  return Copy(
      loaded_node_schema()->field_names(), loaded_edge_schema()->field_names());
}

katana::Result<std::unique_ptr<katana::PropertyGraph>>
katana::PropertyGraph::Copy(
    const std::vector<std::string>& node_properties,
    const std::vector<std::string>& edge_properties) const {
  // TODO(gill): This should copy the RDG in memory without reloading from storage.
  tsuba::RDGLoadOptions opts;
  opts.partition_id_to_load = partition_id();
  opts.node_properties = node_properties;
  opts.edge_properties = edge_properties;

  return Make(rdg_dir(), opts);
}

katana::Result<void>
katana::PropertyGraph::Validate() {
  // TODO (thunt) check that arrow table sizes match topology
  // if (topology_.out_dests &&
  //    topology_.out_dests->length() != table->num_rows()) {
  //  return ErrorCode::InvalidArgument;
  //}
  // if (topology_.out_indices &&
  //    topology_.out_indices->length() != table->num_rows()) {
  //  return ErrorCode::InvalidArgument;
  //}

  uint64_t num_node_rows = static_cast<uint64_t>(node_properties()->num_rows());
  if (num_node_rows == 0) {
    if ((node_properties()->num_columns() != 0) && (num_nodes() != 0)) {
      return KATANA_ERROR(
          ErrorCode::AssertionFailed,
          "number of rows in node properties is 0 but "
          "the number of node properties is {} and the number of nodes is {}",
          node_properties()->num_columns(), num_nodes());
    }
  } else if (num_node_rows != num_nodes()) {
    return KATANA_ERROR(
        ErrorCode::AssertionFailed,
        "number of rows in node properties {} differs "
        "from the number of nodes {}",
        node_properties()->num_rows(), num_nodes());
  }

  uint64_t num_edge_rows = static_cast<uint64_t>(edge_properties()->num_rows());
  if (num_edge_rows == 0) {
    if ((edge_properties()->num_columns() != 0) && (num_edges() != 0)) {
      return KATANA_ERROR(
          ErrorCode::AssertionFailed,
          "number of rows in edge properties is 0 but "
          "the number of edge properties is {} and the number of edges is {}",
          edge_properties()->num_columns(), num_edges());
    }
  } else if (num_edge_rows != num_edges()) {
    return KATANA_ERROR(
        ErrorCode::AssertionFailed,
        "number of rows in edge properties {} differs "
        "from the number of edges {}",
        edge_properties()->num_rows(), num_edges());
  }

  return katana::ResultSuccess();
}

katana::Result<void>
katana::PropertyGraph::ConstructEntityTypeIDs() {
  node_entity_type_manager_.Reset();
  uint64_t num_node_rows = static_cast<uint64_t>(node_properties()->num_rows());
  if (num_node_rows == 0) {
    node_entity_type_id_ = GetUnknownEntityTypeIDs(num_nodes());
  } else {
    auto node_types_res = GetEntityTypeIDsFromProperties(
        node_properties(), &node_entity_type_manager_);
    if (!node_types_res) {
      return node_types_res.error().WithContext("node properties");
    }
    node_entity_type_id_ = std::move(node_types_res.value());
  }

  edge_entity_type_manager_.Reset();
  uint64_t num_edge_rows = static_cast<uint64_t>(edge_properties()->num_rows());
  if (num_edge_rows == 0) {
    edge_entity_type_id_ = GetUnknownEntityTypeIDs(num_edges());
  } else {
    auto edge_types_res = GetEntityTypeIDsFromProperties(
        edge_properties(), &edge_entity_type_manager_);
    if (!edge_types_res) {
      return edge_types_res.error().WithContext("edge properties");
    }
    edge_entity_type_id_ = std::move(edge_types_res.value());
  }

  return katana::ResultSuccess();
}

katana::Result<void>
katana::PropertyGraph::DoWrite(
    tsuba::RDGHandle handle, const std::string& command_line,
    tsuba::RDG::RDGVersioningPolicy versioning_action) {
  if (!rdg_.topology_file_storage().Valid()) {
    auto result = WriteTopology(topology());
    if (!result) {
      return result.error();
    }
    return rdg_.Store(
        handle, command_line, versioning_action, std::move(result.value()));
  }

  return rdg_.Store(handle, command_line, versioning_action);
}

katana::Result<void>
katana::PropertyGraph::ConductWriteOp(
    const std::string& uri, const std::string& command_line,
    tsuba::RDG::RDGVersioningPolicy versioning_action) {
  auto open_res = tsuba::Open(uri, tsuba::kReadWrite);
  if (!open_res) {
    return open_res.error();
  }
  auto new_file = std::make_unique<tsuba::RDGFile>(open_res.value());

  if (auto res = DoWrite(*new_file, command_line, versioning_action); !res) {
    return res.error();
  }

  file_ = std::move(new_file);

  return katana::ResultSuccess();
}

katana::Result<void>
katana::PropertyGraph::WriteView(
    const std::string& uri, const std::string& command_line) {
  return ConductWriteOp(
      uri, command_line, tsuba::RDG::RDGVersioningPolicy::RetainVersion);
}

katana::Result<void>
katana::PropertyGraph::WriteGraph(
    const std::string& uri, const std::string& command_line) {
  return ConductWriteOp(
      uri, command_line, tsuba::RDG::RDGVersioningPolicy::IncrementVersion);
}

katana::Result<void>
katana::PropertyGraph::Commit(const std::string& command_line) {
  if (file_ == nullptr) {
    if (rdg_.rdg_dir().empty()) {
      return KATANA_ERROR(
          ErrorCode::InvalidArgument, "RDG commit but rdg_dir_ is empty");
    }
    return WriteGraph(rdg_.rdg_dir().string(), command_line);
  }
  return DoWrite(
      *file_, command_line, tsuba::RDG::RDGVersioningPolicy::IncrementVersion);
}

katana::Result<void>
katana::PropertyGraph::WriteView(const std::string& command_line) {
  // WriteView occurs once, and only before any Commit/Write operation
  KATANA_LOG_DEBUG_ASSERT(file_ == nullptr);
  return WriteView(rdg_.rdg_dir().string(), command_line);
}

bool
katana::PropertyGraph::Equals(const PropertyGraph* other) const {
  if (!topology().Equals(other->topology())) {
    return false;
  }
  const auto& node_props = rdg_.node_properties();
  const auto& edge_props = rdg_.edge_properties();
  const auto& other_node_props = other->node_properties();
  const auto& other_edge_props = other->edge_properties();
  if (node_props->num_columns() != other_node_props->num_columns()) {
    return false;
  }
  if (edge_props->num_columns() != other_edge_props->num_columns()) {
    return false;
  }
  for (const auto& prop_name : node_props->ColumnNames()) {
    if (!node_props->GetColumnByName(prop_name)->Equals(
            other_node_props->GetColumnByName(prop_name))) {
      return false;
    }
  }
  for (const auto& prop_name : edge_props->ColumnNames()) {
    if (!edge_props->GetColumnByName(prop_name)->Equals(
            other_edge_props->GetColumnByName(prop_name))) {
      return false;
    }
  }
  return true;
}

std::string
katana::PropertyGraph::ReportDiff(const PropertyGraph* other) const {
  fmt::memory_buffer buf;
  if (!topology().Equals(other->topology())) {
    fmt::format_to(
        std::back_inserter(buf),
        "Topologies differ nodes/edges {}/{} vs. {}/{}\n",
        topology().num_nodes(), topology().num_edges(),
        other->topology().num_nodes(), other->topology().num_edges());
  } else {
    fmt::format_to(std::back_inserter(buf), "Topologies match!\n");
  }
  const auto& node_props = rdg_.node_properties();
  const auto& edge_props = rdg_.edge_properties();
  const auto& other_node_props = other->node_properties();
  const auto& other_edge_props = other->edge_properties();
  if (node_props->num_columns() != other_node_props->num_columns()) {
    fmt::format_to(
        std::back_inserter(buf), "Number of node properties differ {} vs. {}\n",
        node_props->num_columns(), other_node_props->num_columns());
  }
  if (edge_props->num_columns() != other_edge_props->num_columns()) {
    fmt::format_to(
        std::back_inserter(buf), "Number of edge properties differ {} vs. {}\n",
        edge_props->num_columns(), other_edge_props->num_columns());
  }
  for (const auto& prop_name : node_props->ColumnNames()) {
    auto other_col = other_node_props->GetColumnByName(prop_name);
    auto my_col = node_props->GetColumnByName(prop_name);
    if (other_col == nullptr) {
      fmt::format_to(
          std::back_inserter(buf), "Only first has node property {}\n",
          prop_name);
    } else if (!my_col->Equals(other_col)) {
      fmt::format_to(
          std::back_inserter(buf), "Node property {:15} {:12} differs\n",
          prop_name, fmt::format("({})", my_col->type()->name()));
      if (my_col->length() != other_col->length()) {
        fmt::format_to(
            std::back_inserter(buf), " size {}/{}\n", my_col->length(),
            other_col->length());
      } else {
        DiffFormatTo(buf, my_col, other_col);
      }
    } else {
      fmt::format_to(
          std::back_inserter(buf), "Node property {:15} {:12} matches!\n",
          prop_name, fmt::format("({})", my_col->type()->name()));
    }
  }
  for (const auto& prop_name : edge_props->ColumnNames()) {
    auto other_col = other_edge_props->GetColumnByName(prop_name);
    auto my_col = edge_props->GetColumnByName(prop_name);
    if (other_col == nullptr) {
      fmt::format_to(
          std::back_inserter(buf), "Only first has edge property {}\n",
          prop_name);
    } else if (!edge_props->GetColumnByName(prop_name)->Equals(
                   other_edge_props->GetColumnByName(prop_name))) {
      fmt::format_to(
          std::back_inserter(buf), "Edge property {:15} {:12} differs\n",
          prop_name, fmt::format("({})", my_col->type()->name()));
      if (my_col->length() != other_col->length()) {
        fmt::format_to(
            std::back_inserter(buf), " size {}/{}\n", my_col->length(),
            other_col->length());
      } else {
        DiffFormatTo(buf, my_col, other_col);
      }
    } else {
      fmt::format_to(
          std::back_inserter(buf), "Edge property {:15} {:12} matches!\n",
          prop_name, fmt::format("({})", my_col->type()->name()));
    }
  }
  return std::string(buf.begin(), buf.end());
}

katana::Result<void>
katana::PropertyGraph::Write(
    const std::string& rdg_name, const std::string& command_line) {
  if (auto res = tsuba::Create(rdg_name); !res) {
    return res.error();
  }
  return WriteGraph(rdg_name, command_line);
}

katana::Result<void>
katana::PropertyGraph::AddNodeProperties(
    const std::shared_ptr<arrow::Table>& props) {
  if (props->num_columns() == 0) {
    KATANA_LOG_DEBUG("adding empty node prop table");
    return ResultSuccess();
  }
  if (topology().num_nodes() != static_cast<uint64_t>(props->num_rows())) {
    return KATANA_ERROR(
        ErrorCode::InvalidArgument, "expected {} rows found {} instead",
        topology().num_nodes(), props->num_rows());
  }
  return rdg_.AddNodeProperties(props);
}

katana::Result<void>
katana::PropertyGraph::UpsertNodeProperties(
    const std::shared_ptr<arrow::Table>& props) {
  if (props->num_columns() == 0) {
    KATANA_LOG_DEBUG("upsert empty node prop table");
    return ResultSuccess();
  }
  if (topology().num_nodes() != static_cast<uint64_t>(props->num_rows())) {
    return KATANA_ERROR(
        ErrorCode::InvalidArgument, "expected {} rows found {} instead",
        topology().num_nodes(), props->num_rows());
  }
  return rdg_.UpsertNodeProperties(props);
}

katana::Result<void>
katana::PropertyGraph::RemoveNodeProperty(int i) {
  return rdg_.RemoveNodeProperty(i);
}

katana::Result<void>
katana::PropertyGraph::RemoveNodeProperty(const std::string& prop_name) {
  auto col_names = node_properties()->ColumnNames();
  auto pos = std::find(col_names.cbegin(), col_names.cend(), prop_name);
  if (pos != col_names.cend()) {
    return rdg_.RemoveNodeProperty(std::distance(col_names.cbegin(), pos));
  }
  return katana::ErrorCode::PropertyNotFound;
}

katana::Result<void>
katana::PropertyGraph::UnloadNodeProperty(int i) {
  return rdg_.UnloadNodeProperty(i);
}

katana::Result<void>
katana::PropertyGraph::LoadNodeProperty(const std::string& name, int i) {
  return rdg_.LoadNodeProperty(name, i);
}
/// Load a node property by name if it is absent and append its column to
/// the table do nothing otherwise
katana::Result<void>
katana::PropertyGraph::EnsureNodePropertyLoaded(const std::string& name) {
  if (HasNodeProperty(name)) {
    return katana::ResultSuccess();
  }
  return LoadNodeProperty(name);
}

std::vector<std::string>
katana::PropertyGraph::ListNodeProperties() const {
  return rdg_.ListNodeProperties();
}

std::vector<std::string>
katana::PropertyGraph::ListEdgeProperties() const {
  return rdg_.ListEdgeProperties();
}

katana::Result<void>
katana::PropertyGraph::UnloadNodeProperty(const std::string& prop_name) {
  auto col_names = node_properties()->ColumnNames();
  auto pos = std::find(col_names.cbegin(), col_names.cend(), prop_name);
  if (pos != col_names.cend()) {
    return rdg_.UnloadNodeProperty(std::distance(col_names.cbegin(), pos));
  }
  return katana::ErrorCode::PropertyNotFound;
}

katana::Result<void>
katana::PropertyGraph::AddEdgeProperties(
    const std::shared_ptr<arrow::Table>& props) {
  if (props->num_columns() == 0) {
    KATANA_LOG_DEBUG("adding empty edge prop table");
    return ResultSuccess();
  }
  if (topology().num_edges() != static_cast<uint64_t>(props->num_rows())) {
    return KATANA_ERROR(
        ErrorCode::InvalidArgument, "expected {} rows found {} instead",
        topology().num_edges(), props->num_rows());
  }
  return rdg_.AddEdgeProperties(props);
}

katana::Result<void>
katana::PropertyGraph::UpsertEdgeProperties(
    const std::shared_ptr<arrow::Table>& props) {
  if (props->num_columns() == 0) {
    KATANA_LOG_DEBUG("upsert empty edge prop table");
    return ResultSuccess();
  }
  if (topology().num_edges() != static_cast<uint64_t>(props->num_rows())) {
    return KATANA_ERROR(
        ErrorCode::InvalidArgument, "expected {} rows found {} instead",
        topology().num_edges(), props->num_rows());
  }
  return rdg_.UpsertEdgeProperties(props);
}

katana::Result<void>
katana::PropertyGraph::RemoveEdgeProperty(int i) {
  return rdg_.RemoveEdgeProperty(i);
}

katana::Result<void>
katana::PropertyGraph::RemoveEdgeProperty(const std::string& prop_name) {
  auto col_names = edge_properties()->ColumnNames();
  auto pos = std::find(col_names.cbegin(), col_names.cend(), prop_name);
  if (pos != col_names.cend()) {
    return rdg_.RemoveEdgeProperty(std::distance(col_names.cbegin(), pos));
  }
  return katana::ErrorCode::PropertyNotFound;
}

katana::Result<void>
katana::PropertyGraph::UnloadEdgeProperty(int i) {
  return rdg_.UnloadEdgeProperty(i);
}

katana::Result<void>
katana::PropertyGraph::LoadEdgeProperty(const std::string& name, int i) {
  return rdg_.LoadEdgeProperty(name, i);
}

/// Load an edge property by name if it is absent and append its column to
/// the table do nothing otherwise
katana::Result<void>
katana::PropertyGraph::EnsureEdgePropertyLoaded(const std::string& name) {
  if (HasEdgeProperty(name)) {
    return katana::ResultSuccess();
  }
  return LoadEdgeProperty(name);
}

katana::Result<void>
katana::PropertyGraph::UnloadEdgeProperty(const std::string& prop_name) {
  auto col_names = edge_properties()->ColumnNames();
  auto pos = std::find(col_names.cbegin(), col_names.cend(), prop_name);
  if (pos != col_names.cend()) {
    return rdg_.UnloadEdgeProperty(std::distance(col_names.cbegin(), pos));
  }
  return katana::ErrorCode::PropertyNotFound;
}

katana::Result<void>
katana::PropertyGraph::InformPath(const std::string& input_path) {
  if (!rdg_.rdg_dir().empty()) {
    KATANA_LOG_DEBUG("rdg dir from {} to {}", rdg_.rdg_dir(), input_path);
  }
  auto uri_res = katana::Uri::Make(input_path);
  if (!uri_res) {
    return uri_res.error();
  }

  rdg_.set_rdg_dir(uri_res.value());
  return ResultSuccess();
}

// Build an index over nodes.
katana::Result<void>
katana::PropertyGraph::MakeNodeIndex(const std::string& column_name) {
  for (const auto& existing_index : node_indexes_) {
    if (existing_index->column_name() == column_name) {
      return KATANA_ERROR(
          katana::ErrorCode::AlreadyExists,
          "Index already exists for column {}", column_name);
    }
  }

  // Get a view of the property.
  std::shared_ptr<arrow::ChunkedArray> chunked_property =
      GetNodeProperty(column_name);
  if (!chunked_property) {
    return KATANA_ERROR(
        katana::ErrorCode::NotFound, "No such property: {}", column_name);
  }
  KATANA_LOG_ASSERT(chunked_property->num_chunks() == 1);
  std::shared_ptr<arrow::Array> property = chunked_property->chunk(0);

  // Create an index based on the type of the field.
  std::unique_ptr<katana::PropertyIndex<GraphTopology::Node>> index =
      KATANA_CHECKED(katana::MakeTypedIndex<katana::GraphTopology::Node>(
          column_name, num_nodes(), property));

  KATANA_CHECKED(index->BuildFromProperty());

  node_indexes_.push_back(std::move(index));

  return katana::ResultSuccess();
}

// Build an index over edges.
katana::Result<void>
katana::PropertyGraph::MakeEdgeIndex(const std::string& column_name) {
  for (const auto& existing_index : edge_indexes_) {
    if (existing_index->column_name() == column_name) {
      return KATANA_ERROR(
          katana::ErrorCode::AlreadyExists,
          "Index already exists for column {}", column_name);
    }
  }

  // Get a view of the property.
  std::shared_ptr<arrow::ChunkedArray> chunked_property =
      GetEdgeProperty(column_name);
  if (!chunked_property) {
    return KATANA_ERROR(
        katana::ErrorCode::NotFound, "No such property: {}", column_name);
  }
  KATANA_LOG_ASSERT(chunked_property->num_chunks() == 1);
  std::shared_ptr<arrow::Array> property = chunked_property->chunk(0);

  // Create an index based on the type of the field.
  std::unique_ptr<katana::PropertyIndex<katana::GraphTopology::Edge>> index =
      KATANA_CHECKED(katana::MakeTypedIndex<katana::GraphTopology::Edge>(
          column_name, num_edges(), property));

  KATANA_CHECKED(index->BuildFromProperty());

  edge_indexes_.push_back(std::move(index));

  return katana::ResultSuccess();
}

katana::Result<std::unique_ptr<katana::NUMAArray<uint64_t>>>
katana::SortAllEdgesByDest(katana::PropertyGraph* pg) {
  // TODO(amber): This function will soon change so that it produces a new sorted
  // topology instead of modifying an existing one. The const_cast will go away
  const auto& topo = pg->topology();

  auto permutation_vec = std::make_unique<katana::NUMAArray<uint64_t>>();
  permutation_vec->allocateInterleaved(topo.num_edges());
  katana::ParallelSTL::iota(
      permutation_vec->begin(), permutation_vec->end(), uint64_t{0});

  auto* out_dests_data = const_cast<GraphTopology::Node*>(topo.dest_data());

  katana::do_all(
      katana::iterate(pg->topology().all_nodes()),
      [&](GraphTopology::Node n) {
        const auto e_beg = *pg->topology().edges(n).begin();
        const auto e_end = *pg->topology().edges(n).end();

        auto sort_iter_beg = katana::make_zip_iterator(
            out_dests_data + e_beg, permutation_vec->begin() + e_beg);
        auto sort_iter_end = katana::make_zip_iterator(
            out_dests_data + e_end, permutation_vec->begin() + e_end);

        std::sort(
            sort_iter_beg, sort_iter_end,
            [&](const auto& tup1, const auto& tup2) {
              auto d1 = std::get<0>(tup1);
              auto d2 = std::get<0>(tup2);
              static_assert(std::is_same_v<decltype(d1), GraphTopology::Node>);
              static_assert(std::is_same_v<decltype(d1), GraphTopology::Node>);
              return d1 < d2;
            });
      },
      katana::steal());

  return std::unique_ptr<katana::NUMAArray<uint64_t>>(
      std::move(permutation_vec));
}

// TODO(amber): make this a method of a sorted topology class in the near future
// TODO(amber): this method should return an edge_iterator
katana::GraphTopology::Edge
katana::FindEdgeSortedByDest(
    const PropertyGraph* graph, const GraphTopology::Node src,
    const GraphTopology::Node dst) {
  const auto& topo = graph->topology();
  auto e_range = topo.edges(src);

  constexpr size_t kBinarySearchThreshold = 64;

  if (e_range.size() <= kBinarySearchThreshold) {
    auto iter = std::find_if(
        e_range.begin(), e_range.end(),
        [&](const GraphTopology::Edge& e) { return topo.edge_dest(e) == dst; });

    return *iter;

  } else {
    auto iter = std::lower_bound(
        e_range.begin(), e_range.end(), dst,
        internal::EdgeDestComparator<GraphTopology>{&topo});

    return topo.edge_dest(*iter) == dst ? *iter : *e_range.end();
  }
}

// TODO(amber): this method should return a new sorted topology
katana::Result<void>
katana::SortNodesByDegree(katana::PropertyGraph* pg) {
  const auto& topo = pg->topology();

  uint64_t num_nodes = topo.num_nodes();
  uint64_t num_edges = topo.num_edges();

  using DegreeNodePair = std::pair<uint64_t, uint32_t>;
  katana::NUMAArray<DegreeNodePair> dn_pairs;
  dn_pairs.allocateInterleaved(num_nodes);

  katana::do_all(katana::iterate(topo.all_nodes()), [&](auto node) {
    size_t node_degree = pg->edges(node).size();
    dn_pairs[node] = DegreeNodePair(node_degree, node);
  });

  // sort by degree (first item)
  katana::ParallelSTL::sort(
      dn_pairs.begin(), dn_pairs.end(), std::greater<DegreeNodePair>());

  // create mapping, get degrees out to another vector to get prefix sum
  katana::NUMAArray<uint32_t> old_to_new_mapping;
  old_to_new_mapping.allocateInterleaved(num_nodes);

  katana::NUMAArray<uint64_t> new_prefix_sum;
  new_prefix_sum.allocateInterleaved(num_nodes);

  katana::do_all(katana::iterate(uint64_t{0}, num_nodes), [&](uint64_t index) {
    // save degree, which is pair.first
    new_prefix_sum[index] = dn_pairs[index].first;
    // save mapping; original index is in .second, map it to current index
    old_to_new_mapping[dn_pairs[index].second] = index;
  });

  katana::ParallelSTL::partial_sum(
      new_prefix_sum.begin(), new_prefix_sum.end(), new_prefix_sum.begin());

  katana::NUMAArray<uint32_t> new_out_dest;
  new_out_dest.allocateInterleaved(num_edges);

  auto* out_dests_data = const_cast<GraphTopology::Node*>(topo.dest_data());
  auto* out_indices_data = const_cast<GraphTopology::Edge*>(topo.adj_data());

  katana::do_all(
      katana::iterate(topo.all_nodes()),
      [&](auto old_node_id) {
        uint32_t new_node_id = old_to_new_mapping[old_node_id];

        // get the start location of this reindex'd nodes edges
        uint64_t new_out_index =
            (new_node_id == 0) ? 0 : new_prefix_sum[new_node_id - 1];

        // construct the graph, reindexing as it goes along
        for (auto e : topo.edges(old_node_id)) {
          // get destination, reindex
          uint32_t old_edge_dest = out_dests_data[e];
          uint32_t new_edge_dest = old_to_new_mapping[old_edge_dest];

          new_out_dest[new_out_index] = new_edge_dest;

          new_out_index++;
        }
        // this assert makes sure reindex was correct + makes sure all edges
        // are accounted for
        KATANA_LOG_DEBUG_ASSERT(new_out_index == new_prefix_sum[new_node_id]);
      },
      katana::steal());

  //Update the underlying PropertyGraph topology
  // TODO(amber): eliminate these copies since we will be returning a new topology
  katana::do_all(katana::iterate(uint64_t{0}, num_nodes), [&](auto node_id) {
    out_indices_data[node_id] = new_prefix_sum[node_id];
  });

  katana::do_all(katana::iterate(uint64_t{0}, num_edges), [&](auto edge_id) {
    out_dests_data[edge_id] = new_out_dest[edge_id];
  });

  return katana::ResultSuccess();
}

katana::Result<std::unique_ptr<katana::PropertyGraph>>
katana::CreateSymmetricGraph(katana::PropertyGraph* pg) {
  const GraphTopology& topology = pg->topology();
  if (topology.num_nodes() == 0) {
    return std::make_unique<PropertyGraph>();
  }

  // New symmetric graph topology
  katana::NUMAArray<uint64_t> out_indices;
  katana::NUMAArray<uint32_t> out_dests;

  out_indices.allocateInterleaved(topology.num_nodes());
  // Store the out-degree of nodes from original graph
  katana::do_all(katana::iterate(topology.all_nodes()), [&](auto n) {
    auto edges = topology.edges(n);
    out_indices[n] = (*edges.end() - *edges.begin());
  });

  katana::do_all(
      katana::iterate(topology.all_nodes()),
      [&](auto n) {
        // update the out_indices for the symmetric topology
        for (auto e : topology.edges(n)) {
          auto dest = topology.edge_dest(e);
          // Do not add reverse edge for self-loops
          if (n != dest) {
            __sync_fetch_and_add(&(out_indices[dest]), 1);
          }
        }
      },
      katana::steal());

  // Compute prefix sum
  katana::ParallelSTL::partial_sum(
      out_indices.begin(), out_indices.end(), out_indices.begin());

  uint64_t num_nodes_symmetric = topology.num_nodes();
  uint64_t num_edges_symmetric = out_indices[num_nodes_symmetric - 1];

  katana::NUMAArray<uint64_t> out_dests_offset;
  out_dests_offset.allocateInterleaved(topology.num_nodes());
  // Temp NUMAArray for computing new destination positions
  out_dests_offset[0] = 0;
  katana::do_all(
      katana::iterate(uint64_t{1}, topology.num_nodes()),
      [&](uint64_t n) { out_dests_offset[n] = out_indices[n - 1]; },
      katana::no_stats());

  out_dests.allocateInterleaved(num_edges_symmetric);
  // Update graph topology with the original edges + reverse edges
  katana::do_all(
      katana::iterate(topology.all_nodes()),
      [&](auto src) {
        // get all outgoing edges (excluding self edges) of a particular
        // node and add reverse edges.
        for (GraphTopology::Edge e : topology.edges(src)) {
          // e = start index into edge array for a particular node
          // destination node
          auto dest = topology.edge_dest(e);

          // Add original edge
          auto e_new_src = __sync_fetch_and_add(&(out_dests_offset[src]), 1);
          out_dests[e_new_src] = dest;

          // Do not add reverse edge for self-loops
          if (dest != src) {
            // Add reverse edge
            auto e_new_dst = __sync_fetch_and_add(&(out_dests_offset[dest]), 1);
            out_dests[e_new_dst] = src;
            // TODO(gill) copy edge data to "new" array
          }
        }
      },
      katana::no_stats());

  GraphTopology sym_topo(std::move(out_indices), std::move(out_dests));
  auto symmetric = std::make_unique<katana::PropertyGraph>(std::move(sym_topo));

  return std::unique_ptr<PropertyGraph>(std::move(symmetric));
}

katana::Result<std::unique_ptr<katana::PropertyGraph>>
katana::CreateTransposeGraphTopology(const GraphTopology& topology) {
  if (topology.num_nodes() == 0) {
    return std::make_unique<PropertyGraph>();
  }

  katana::NUMAArray<GraphTopology::Edge> out_indices;
  katana::NUMAArray<GraphTopology::Node> out_dests;

  out_indices.allocateInterleaved(topology.num_nodes());
  out_dests.allocateInterleaved(topology.num_edges());

  // Initialize the new topology indices
  katana::do_all(
      katana::iterate(uint64_t{0}, topology.num_nodes()),
      [&](uint64_t n) { out_indices[n] = uint64_t{0}; }, katana::no_stats());

  // Keep a copy of old destinaton ids and compute number of
  // in-coming edges for the new prefix sum of out_indices.
  katana::do_all(
      katana::iterate(topology.all_edges()),
      [&](auto e) {
        // Counting outgoing edges in the tranpose graph by
        // counting incoming edges in the original graph
        auto dest = topology.edge_dest(e);
        __sync_add_and_fetch(&(out_indices[dest]), 1);
      },
      katana::no_stats());

  // Prefix sum calculation of the edge index array
  katana::ParallelSTL::partial_sum(
      out_indices.begin(), out_indices.end(), out_indices.begin());

  katana::NUMAArray<uint64_t> out_dests_offset;
  out_dests_offset.allocateInterleaved(topology.num_nodes());

  // temporary buffer for storing the starting point of each node's transpose
  // adjacency
  out_dests_offset[0] = 0;
  katana::do_all(
      katana::iterate(uint64_t{1}, topology.num_nodes()),
      [&](uint64_t n) { out_dests_offset[n] = out_indices[n - 1]; },
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
        }
      },
      katana::no_stats());

  GraphTopology transpose_topo{std::move(out_indices), std::move(out_dests)};
  auto transpose_pg =
      std::make_unique<katana::PropertyGraph>(std::move(transpose_topo));

  return std::unique_ptr<PropertyGraph>(std::move(transpose_pg));
}
