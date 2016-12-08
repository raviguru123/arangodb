/// @brief Implementation of Shortest Path Execution Node
///
/// @file arangod/Aql/ShortestPathNode.cpp
///
/// DISCLAIMER
///
/// Copyright 2010-2014 triagens GmbH, Cologne, Germany
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
/// @author Copyright 2015, ArangoDB GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "GraphNode.h"
#include "Aql/Ast.h"
#include "Aql/ExecutionPlan.h"
#include "Cluster/ClusterInfo.h"
#include "Utils/CollectionNameResolver.h"

using namespace arangodb::basics;
using namespace arangodb::aql;

static TRI_edge_direction_e parseDirection (AstNode const* node) {
  TRI_ASSERT(node->isIntValue());
  auto dirNum = node->getIntValue();

  switch (dirNum) {
    case 0:
      return TRI_EDGE_ANY;
    case 1:
      return TRI_EDGE_IN;
    case 2:
      return TRI_EDGE_OUT;
    default:
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_QUERY_PARSE,
          "direction can only be INBOUND, OUTBOUND or ANY");
  }
}

GraphNode::GraphNode(ExecutionPlan* plan, size_t id, TRI_vocbase_t* vocbase,
       AstNode const* direction, AstNode const* graph, traverser::BaseTraverserOptions* options) :
      ExecutionNode(plan, id),
      _vocbase(vocbase),
      _vertexOutVariable(nullptr),
      _edgeOutVariable(nullptr),
      _graphObj(nullptr),
      _tmpObjVariable(_plan->getAst()->variables()->createTemporaryVariable()),
      _tmpObjVarNode(_plan->getAst()->createNodeReference(_tmpObjVariable)),
      _tmpIdNode(_plan->getAst()->createNodeValueString("", 0)),
      _fromCondition(nullptr),
      _toCondition(nullptr),
      _optionsBuild(false),
      _isSmart(false),
      _options(options) {
  TRI_ASSERT(_vocbase != nullptr);
  TRI_ASSERT(graph != nullptr);
  TRI_ASSERT(options != nullptr);
  TRI_ASSERT(_options.get() != nullptr);

  TRI_edge_direction_e baseDirection = parseDirection(direction);

  std::unordered_map<std::string, TRI_edge_direction_e> seenCollections;


  auto addEdgeColl = [&](std::string const& n, TRI_edge_direction_e dir) -> void {
    if (_isSmart) {
      if (n.compare(0, 6, "_from_") == 0) {
        if (dir != TRI_EDGE_IN) {
          _directions.emplace_back(TRI_EDGE_OUT);
          _edgeColls.emplace_back(std::make_unique<aql::Collection>(
              n, _vocbase, TRI_TRANSACTION_READ));
        }
        return;
      } else if (n.compare(0, 4, "_to_") == 0) {
        if (dir != TRI_EDGE_OUT) {
          _directions.emplace_back(TRI_EDGE_IN);
          _edgeColls.emplace_back(std::make_unique<aql::Collection>(
              n, _vocbase, TRI_TRANSACTION_READ));
        }
        return;
      }
    }

    if (dir == TRI_EDGE_ANY) {
      _directions.emplace_back(TRI_EDGE_OUT);
      _edgeColls.emplace_back(std::make_unique<aql::Collection>(
          n, _vocbase, TRI_TRANSACTION_READ));

      _directions.emplace_back(TRI_EDGE_IN);
      _edgeColls.emplace_back(std::make_unique<aql::Collection>(
          n, _vocbase, TRI_TRANSACTION_READ));
    } else {
      _directions.emplace_back(dir);
      _edgeColls.emplace_back(std::make_unique<aql::Collection>(
          n, _vocbase, TRI_TRANSACTION_READ));
    }
  };

  if (graph->type == NODE_TYPE_COLLECTION_LIST) {
    size_t edgeCollectionCount = graph->numMembers();

    _graphInfo.openArray();
    _edgeColls.reserve(edgeCollectionCount);
    _directions.reserve(edgeCollectionCount);

    // First determine whether all edge collections are smart and sharded
    // like a common collection:
    auto ci = ClusterInfo::instance();
    if (ServerState::instance()->isRunningInCluster()) {
      _isSmart = true;
      std::string distributeShardsLike;
      for (size_t i = 0; i < edgeCollectionCount; ++i) {
        auto col = graph->getMember(i);
        if (col->type == NODE_TYPE_DIRECTION) {
          col = col->getMember(1); // The first member always is the collection
        }
        std::string n = col->getString();
        auto c = ci->getCollection(_vocbase->name(), n);
        if (!c->isSmart() || c->distributeShardsLike().empty()) {
          _isSmart = false;
          break;
        }
        if (distributeShardsLike.empty()) {
          distributeShardsLike = c->distributeShardsLike();
        } else if (distributeShardsLike != c->distributeShardsLike()) {
          _isSmart = false;
          break;
        }
      }
    }
   
    auto resolver = std::make_unique<CollectionNameResolver>(vocbase);
    // List of edge collection names
    for (size_t i = 0; i < edgeCollectionCount; ++i) {
      auto col = graph->getMember(i);
      TRI_edge_direction_e dir = TRI_EDGE_ANY;
      
      if (col->type == NODE_TYPE_DIRECTION) {
        // We have a collection with special direction.
        dir = parseDirection(col->getMember(0));
        col = col->getMember(1);
      } else {
        dir = baseDirection;
      }
        
      std::string eColName = col->getString();
      
      // now do some uniqueness checks for the specified collections
      auto it = seenCollections.find(eColName);
      if (it != seenCollections.end()) {
        if ((*it).second != dir) {
          std::string msg("conflicting directions specified for collection '" +
                          std::string(eColName));
          THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_COLLECTION_TYPE_INVALID,
                                         msg);
        }
        // do not re-add the same collection!
        continue;
      }
      seenCollections.emplace(eColName, dir);
      
      if (resolver->getCollectionTypeCluster(eColName) != TRI_COL_TYPE_EDGE) {
        std::string msg("collection type invalid for collection '" +
                        std::string(eColName) +
                        ": expecting collection type 'edge'");
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_COLLECTION_TYPE_INVALID,
                                       msg);
      }
      
      _graphInfo.add(VPackValue(eColName));
      if (ServerState::instance()->isRunningInCluster()) {
        auto c = ci->getCollection(_vocbase->name(), eColName);
        if (!c->isSmart()) {
          addEdgeColl(eColName, dir);
        } else {
          std::vector<std::string> names;
          if (_isSmart) {
            names = c->realNames();
          } else {
            names = c->realNamesForRead();
          }
          for (auto const& name : names) {
            addEdgeColl(name, dir);
          }
        }
      } else {
        addEdgeColl(eColName, dir);
      }
    }
    _graphInfo.close();
  } else {
    if (_edgeColls.empty()) {
      if (graph->isStringValue()) {
        std::string graphName = graph->getString();
        _graphInfo.add(VPackValue(graphName));
        _graphObj = plan->getAst()->query()->lookupGraphByName(graphName);

        if (_graphObj == nullptr) {
          THROW_ARANGO_EXCEPTION(TRI_ERROR_GRAPH_NOT_FOUND);
        }

        auto eColls = _graphObj->edgeCollections();
        size_t length = eColls.size();
        if (length == 0) {
          THROW_ARANGO_EXCEPTION(TRI_ERROR_GRAPH_EMPTY);
        }

        // First determine whether all edge collections are smart and sharded
        // like a common collection:
        auto ci = ClusterInfo::instance();
        if (ServerState::instance()->isRunningInCluster()) {
          _isSmart = true;
          std::string distributeShardsLike;
          for (auto const& n : eColls) {
            auto c = ci->getCollection(_vocbase->name(), n);
            if (!c->isSmart() || c->distributeShardsLike().empty()) {
              _isSmart = false;
              break;
            }
            if (distributeShardsLike.empty()) {
              distributeShardsLike = c->distributeShardsLike();
            } else if (distributeShardsLike != c->distributeShardsLike()) {
              _isSmart = false;
              break;
            }
          }
        }
        
        for (const auto& n : eColls) {
          if (ServerState::instance()->isRunningInCluster()) {
            auto c = ci->getCollection(_vocbase->name(), n);
            if (!c->isSmart()) {
              addEdgeColl(n, baseDirection);
            } else {
              std::vector<std::string> names;
              if (_isSmart) {
                names = c->realNames();
              } else {
                names = c->realNamesForRead();
              }
              for (auto const& name : names) {
                addEdgeColl(name, baseDirection);
              }
            }
          } else {
            addEdgeColl(n, baseDirection);
          }
        }

        auto vColls = _graphObj->vertexCollections();
        length = vColls.size();
        if (length == 0) {
          THROW_ARANGO_EXCEPTION(TRI_ERROR_GRAPH_EMPTY);
        }
        _vertexColls.reserve(length);
        for (auto const& v : vColls) {
          _vertexColls.emplace_back(std::make_unique<aql::Collection>(
              v, _vocbase, TRI_TRANSACTION_READ));
        }
      }
    }
  }
}

GraphNode::GraphNode(ExecutionPlan* plan,
                     arangodb::velocypack::Slice const& base)
    : ExecutionNode(plan, base),
      _vocbase(plan->getAst()->query()->vocbase()),
      _vertexOutVariable(nullptr),
      _edgeOutVariable(nullptr),
      _graphObj(nullptr),
      _tmpObjVariable(nullptr),
      _tmpObjVarNode(nullptr),
      _tmpIdNode(nullptr),
      _fromCondition(nullptr),
      _toCondition(nullptr),
      _optionsBuild(false),
      _options(nullptr) {
  // NOTE: options have to be created by subclass. They differ
  // Directions
  VPackSlice dirList = base.get("directions");
  for (auto const& it : VPackArrayIterator(dirList)) {
    uint64_t dir = arangodb::basics::VelocyPackHelper::stringUInt64(it);
    TRI_edge_direction_e d;
    switch (dir) {
      case 0:
        TRI_ASSERT(false);
        break;
      case 1:
        d = TRI_EDGE_IN;
        break;
      case 2:
        d = TRI_EDGE_OUT;
        break;
      default:
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                       "Invalid direction value");
        break;
    }
    _directions.emplace_back(d);
  }

  // TODO: Can we remove this?
  std::string graphName;
  if (base.hasKey("graph") && (base.get("graph").isString())) {
    graphName = base.get("graph").copyString();
    if (base.hasKey("graphDefinition")) {
      _graphObj = plan->getAst()->query()->lookupGraphByName(graphName);

      if (_graphObj == nullptr) {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_GRAPH_NOT_FOUND);
      }
    } else {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                     "missing graphDefinition.");
    }
  } else {
    _graphInfo.add(base.get("graph"));
    if (!_graphInfo.slice().isArray()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                     "graph has to be an array.");
    }
  }

  VPackSlice list = base.get("edgeCollections");
  if (!list.isArray()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                   "traverser needs an array of edge collections.");
  }

  for (auto const& it : VPackArrayIterator(list)) {
    std::string e = arangodb::basics::VelocyPackHelper::getStringValue(it, "");
    _edgeColls.emplace_back(
        std::make_unique<aql::Collection>(e, _vocbase, TRI_TRANSACTION_READ));
  }

  list = base.get("vertexCollections");

  if (!list.isArray()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                   "traverser needs an array of vertex collections.");
  }

  for (auto const& it : VPackArrayIterator(list)) {
    std::string v = arangodb::basics::VelocyPackHelper::getStringValue(it, "");
    _vertexColls.emplace_back(
        std::make_unique<aql::Collection>(v, _vocbase, TRI_TRANSACTION_READ));
  }


  // Out variables
  if (base.hasKey("vertexOutVariable")) {
    _vertexOutVariable = varFromVPack(plan->getAst(), base, "vertexOutVariable");
  }
  if (base.hasKey("edgeOutVariable")) {
    _edgeOutVariable = varFromVPack(plan->getAst(), base, "edgeOutVariable");
  }

  // Temporary Filter Objects
  TRI_ASSERT(base.hasKey("tmpObjVariable"));
  _tmpObjVariable = varFromVPack(plan->getAst(), base, "tmpObjVariable");

  TRI_ASSERT(base.hasKey("tmpObjVarNode"));
  _tmpObjVarNode = new AstNode(plan->getAst(), base.get("tmpObjVarNode"));

  TRI_ASSERT(base.hasKey("tmpIdNode"));
  _tmpIdNode = new AstNode(plan->getAst(), base.get("tmpIdNode"));

  // Filter Condition Parts
  TRI_ASSERT(base.hasKey("fromCondition"));
  _fromCondition = new AstNode(plan->getAst(), base.get("fromCondition"));

  TRI_ASSERT(base.hasKey("toCondition"));
  _toCondition = new AstNode(plan->getAst(), base.get("toCondition"));
}

GraphNode::GraphNode(
    ExecutionPlan* plan, size_t id, TRI_vocbase_t* vocbase,
    std::vector<std::unique_ptr<aql::Collection>> const& edgeColls,
    std::vector<std::unique_ptr<aql::Collection>> const& vertexColls,
    std::vector<TRI_edge_direction_e> const& directions,
    traverser::BaseTraverserOptions* options)
    : ExecutionNode(plan, id),
      _vocbase(vocbase),
      _vertexOutVariable(nullptr),
      _edgeOutVariable(nullptr),
      _directions(directions),
      _graphObj(nullptr),
      _tmpObjVariable(nullptr),
      _tmpObjVarNode(nullptr),
      _tmpIdNode(nullptr),
      _fromCondition(nullptr),
      _toCondition(nullptr),
      _optionsBuild(false),
      _options(options) {
  _graphInfo.openArray();
  for (auto const& it : edgeColls) {
    // Collections cannot be copied. So we need to create new ones to prevent leaks
    _edgeColls.emplace_back(std::make_unique<aql::Collection>(
        it->getName(), _vocbase, TRI_TRANSACTION_READ));
    _graphInfo.add(VPackValue(it->getName()));
  }
  for (auto& it : vertexColls) {
    // Collections cannot be copied. So we need to create new ones to prevent leaks
    _vertexColls.emplace_back(std::make_unique<aql::Collection>(
        it->getName(), _vocbase, TRI_TRANSACTION_READ));
  }

  _graphInfo.close();
}
