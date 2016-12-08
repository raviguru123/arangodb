////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_AQL_GRAPH_NODE_H
#define ARANGOD_AQL_GRAPH_NODE_H 1

#include "Aql/Collection.h"
#include "Aql/ExecutionNode.h"
#include "Aql/Graphs.h"
#include "Cluster/TraverserEngineRegistry.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/TraverserOptions.h"

/// NOTE: This Node is purely virtual and is used to unify graph parsing for
///       Traversal and ShortestPath node. It shall never be part of any plan
///       nor will their be a Block to implement it.

namespace arangodb {

namespace aql {

class GraphNode : public ExecutionNode {
 protected:
  /// @brief Constructor for a new node parsed from AQL
  GraphNode(ExecutionPlan* plan, size_t id, TRI_vocbase_t* vocbase,
            AstNode const* direction, AstNode const* graph,
            traverser::BaseTraverserOptions* options);

  /// @brief Deserializer for node from VPack
  GraphNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base);

  /// @brief Internal constructor to clone the node.
  GraphNode(ExecutionPlan* plan, size_t id, TRI_vocbase_t* vocbase,
            std::vector<std::unique_ptr<aql::Collection>> const& edgeColls,
            std::vector<std::unique_ptr<aql::Collection>> const& vertexColls,
            std::vector<TRI_edge_direction_e> const& directions,
            traverser::BaseTraverserOptions* options);

 public:
  virtual ~GraphNode() {}

  ////////////////////////////////////////////////////////////////////////////////
  /// SECTION Shared subclass variables
  ////////////////////////////////////////////////////////////////////////////////
 protected:
  /// @brief the database
  TRI_vocbase_t* _vocbase;

  /// @brief vertex output variable
  Variable const* _vertexOutVariable;

  /// @brief edge output variable
  Variable const* _edgeOutVariable;

  /// @brief input graphInfo only used for serialisation & info
  arangodb::velocypack::Builder _graphInfo;

  /// @brief The directions edges are followed
  std::vector<TRI_edge_direction_e> _directions;

  /// @brief the edge collections
  std::vector<std::unique_ptr<aql::Collection>> _edgeColls;

  /// @brief the vertex collection names
  std::vector<std::unique_ptr<aql::Collection>> _vertexColls;

  /// @brief our graph...
  Graph const* _graphObj;

  /// @brief Temporary pseudo variable for the currently traversed object.
  Variable const* _tmpObjVariable;

  /// @brief Reference to the pseudo variable
  AstNode* _tmpObjVarNode;

  /// @brief Pseudo string value node to hold the last visted vertex id.
  AstNode* _tmpIdNode;

  /// @brief The hard coded condition on _from
  /// NOTE: Created by sub classes, as it differs for class
  AstNode* _fromCondition;

  /// @brief The hard coded condition on _to
  /// NOTE: Created by sub classes, as it differs for class
  AstNode* _toCondition;

  /// @brief Flag if options are already prepared. After
  ///        this flag was set the node cannot be cloned
  ///        any more.
  bool _optionsBuild;

  /// @brief The list of traverser engines grouped by server.
  std::unordered_map<ServerID, traverser::TraverserEngineID> _engines;

  /// @brief flag, if traversal is smart (enterprise edition only!)
  bool _isSmart;

  std::unique_ptr<traverser::BaseTraverserOptions> _options;

};
}
}
#endif
