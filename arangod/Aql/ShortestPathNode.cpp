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

#include "ShortestPathNode.h"
#include "Aql/Ast.h"
#include "Aql/Collection.h"
#include "Aql/ExecutionPlan.h"
#include "Indexes/Index.h"
#include "Utils/CollectionNameResolver.h"

#include <velocypack/Iterator.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb::basics;
using namespace arangodb::aql;

static void parseNodeInput(AstNode const* node, std::string& id,
                           Variable const*& variable) {
  switch (node->type) {
    case NODE_TYPE_REFERENCE:
      variable = static_cast<Variable*>(node->getData());
      id = "";
      break;
    case NODE_TYPE_VALUE:
      if (node->value.type != VALUE_TYPE_STRING) {
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_PARSE,
                                       "invalid start vertex. Must either be "
                                       "an _id string or an object with _id.");
      }
      variable = nullptr;
      id = node->getString();
      break;
    default:
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_PARSE,
                                     "invalid start vertex. Must either be an "
                                     "_id string or an object with _id.");
  }
}

static TRI_edge_direction_e parseDirection (uint64_t dirNum) {
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

ShortestPathNode::ShortestPathNode(
    ExecutionPlan* plan, size_t id, TRI_vocbase_t* vocbase, uint64_t direction,
    AstNode const* start, AstNode const* target, AstNode const* graph,
    std::unique_ptr<traverser::ShortestPathOptions>& options)
    : ExecutionNode(plan, id),
      _vocbase(vocbase),
      _vertexOutVariable(nullptr),
      _edgeOutVariable(nullptr),
      _inStartVariable(nullptr),
      _inTargetVariable(nullptr),
      _graphObj(nullptr),
      _tmpObjVariable(_plan->getAst()->variables()->createTemporaryVariable()),
      _tmpObjVarNode(_plan->getAst()->createNodeReference(_tmpObjVariable)),
      _tmpIdNode(_plan->getAst()->createNodeValueString("", 0)),
      _fromCondition(nullptr),
      _toCondition(nullptr),
      _optionsBuild(false),
      _options(options.release()) {

  TRI_ASSERT(_vocbase != nullptr);
  TRI_ASSERT(start != nullptr);
  TRI_ASSERT(target != nullptr);
  TRI_ASSERT(graph != nullptr);
  TRI_ASSERT(_options.get() != nullptr);

  auto ast = _plan->getAst();
  // Let us build the conditions on _from and _to. Just in case we need them.
  {
    auto const* access = ast->createNodeAttributeAccess(
        _tmpObjVarNode, StaticStrings::FromString.c_str(),
        StaticStrings::FromString.length());
    auto const* condition = ast->createNodeBinaryOperator(
        NODE_TYPE_OPERATOR_BINARY_EQ, access, _tmpIdNode);
    _fromCondition = ast->createNodeNaryOperator(NODE_TYPE_OPERATOR_NARY_AND);
    _fromCondition->addMember(condition);

  }
  TRI_ASSERT(_fromCondition != nullptr);

  {
    auto const* access = ast->createNodeAttributeAccess(
        _tmpObjVarNode, StaticStrings::ToString.c_str(),
        StaticStrings::ToString.length());
    auto const* condition = ast->createNodeBinaryOperator(
        NODE_TYPE_OPERATOR_BINARY_EQ, access, _tmpIdNode);
    _toCondition = ast->createNodeNaryOperator(NODE_TYPE_OPERATOR_NARY_AND);
    _toCondition->addMember(condition);
  }
  TRI_ASSERT(_toCondition != nullptr);

  TRI_edge_direction_e baseDirection = parseDirection(direction);

  if (graph->type == NODE_TYPE_COLLECTION_LIST) {
    size_t edgeCollectionCount = graph->numMembers();
    auto resolver = std::make_unique<CollectionNameResolver>(vocbase);
    _graphInfo.openArray();
    _edgeColls.reserve(edgeCollectionCount);
    _directions.reserve(edgeCollectionCount);
    // List of edge collection names
    for (size_t i = 0; i < edgeCollectionCount; ++i) {
      auto col = graph->getMember(i);
      if (col->type == NODE_TYPE_DIRECTION) {
        // We have a collection with special direction.
        TRI_ASSERT(col->getMember(0)->isIntValue());
        TRI_edge_direction_e dir = parseDirection(col->getMember(0)->getIntValue());
        _directions.emplace_back(dir);
        col = col->getMember(1);
      } else {
        _directions.emplace_back(baseDirection);
      }

      std::string eColName = col->getString();
      auto eColType = resolver->getCollectionTypeCluster(eColName);
      if (eColType != TRI_COL_TYPE_EDGE) {
        std::string msg("collection type invalid for collection '" +
                        std::string(eColName) +
                        ": expecting collection type 'edge'");
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_COLLECTION_TYPE_INVALID,
                                       msg);
      }
      _graphInfo.add(VPackValue(eColName));
      _edgeColls.emplace_back(std::make_unique<aql::Collection>(
          eColName, _vocbase, TRI_TRANSACTION_READ));
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
        _edgeColls.reserve(length);
        _directions.reserve(length);

        for (const auto& n : eColls) {
          _edgeColls.emplace_back(std::make_unique<aql::Collection>(
              n, _vocbase, TRI_TRANSACTION_READ));
          _directions.emplace_back(baseDirection);
        }
      }
    }
  }

  parseNodeInput(start, _startVertexId, _inStartVariable);
  parseNodeInput(target, _targetVertexId, _inTargetVariable);
}

ShortestPathNode::ShortestPathNode(
    ExecutionPlan* plan, size_t id, TRI_vocbase_t* vocbase,
    std::vector<std::unique_ptr<aql::Collection>> const& edgeColls,
    std::vector<TRI_edge_direction_e> const& directions,
    Variable const* inStartVariable, std::string const& startVertexId,
    Variable const* inTargetVariable, std::string const& targetVertexId,
    std::unique_ptr<traverser::ShortestPathOptions>& options)
    : ExecutionNode(plan, id),
      _vocbase(vocbase),
      _vertexOutVariable(nullptr),
      _edgeOutVariable(nullptr),
      _inStartVariable(inStartVariable),
      _startVertexId(startVertexId),
      _inTargetVariable(inTargetVariable),
      _targetVertexId(targetVertexId),
      _directions(directions),
      _graphObj(nullptr),
      _tmpObjVariable(nullptr),
      _tmpObjVarNode(nullptr),
      _tmpIdNode(nullptr),
      _fromCondition(nullptr),
      _toCondition(nullptr),
      _optionsBuild(false),
      _options(options.release()) {
  _graphInfo.openArray();
  for (auto const& it : edgeColls) {
    // Collections cannot be copied. So we need to create new ones to prevent leaks
    _edgeColls.emplace_back(std::make_unique<aql::Collection>(
        it->getName(), _vocbase, TRI_TRANSACTION_READ));
    _graphInfo.add(VPackValue(it->getName()));
  }

  _graphInfo.close();
}

arangodb::traverser::ShortestPathOptions* ShortestPathNode::options()
    const {
  return _options.get();
}

void ShortestPathNode::enhanceEngineInfo(arangodb::velocypack::Builder&) const {
}

void ShortestPathNode::addEngine(traverser::TraverserEngineID const&, ServerID const&) {
}

ShortestPathNode::ShortestPathNode(ExecutionPlan* plan,
                                   arangodb::velocypack::Slice const& base)
    : ExecutionNode(plan, base),
      _vocbase(plan->getAst()->query()->vocbase()),
      _vertexOutVariable(nullptr),
      _edgeOutVariable(nullptr),
      _inStartVariable(nullptr),
      _inTargetVariable(nullptr),
      _graphObj(nullptr),
      _tmpObjVariable(nullptr),
      _tmpObjVarNode(nullptr),
      _tmpIdNode(nullptr),
      _fromCondition(nullptr),
      _toCondition(nullptr),
      _optionsBuild(false) {
  _options = std::make_unique<arangodb::traverser::ShortestPathOptions>(
      plan->getAst()->query()->trx());
  // Directions
  VPackSlice dirList = base.get("directions");
  for (auto const& it : VPackArrayIterator(dirList)) {
    uint64_t dir = arangodb::basics::VelocyPackHelper::stringUInt64(it);
    TRI_edge_direction_e d;
    switch (dir) {
      case 0:
        d = TRI_EDGE_ANY;
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

  // Start Vertex
  if (base.hasKey("startInVariable")) {
    _inStartVariable = varFromVPack(plan->getAst(), base, "startInVariable");
  } else {
    VPackSlice v = base.get("startVertexId");
    if (!v.isString()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                     "start vertex must be a string");
    }
    _startVertexId = v.copyString();

    if (_startVertexId.empty()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                     "start vertex mustn't be empty");
    }
  }

  // Target Vertex
  if (base.hasKey("targetInVariable")) {
    _inTargetVariable = varFromVPack(plan->getAst(), base, "targetInVariable");
  } else {
    VPackSlice v = base.get("targetVertexId");
    if (!v.isString()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                     "target vertex must be a string");
    }
    _targetVertexId = v.copyString();
    if (_targetVertexId.empty()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                     "target vertex mustn't be empty");
    }
  }

  std::string graphName;
  if (base.hasKey("graph") && (base.get("graph").isString())) {
    graphName = base.get("graph").copyString();
    if (base.hasKey("graphDefinition")) {
      _graphObj = plan->getAst()->query()->lookupGraphByName(graphName);

      if (_graphObj == nullptr) {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_GRAPH_NOT_FOUND);
      }

      auto eColls = _graphObj->edgeCollections();
      for (const auto& n : eColls) {
        _edgeColls.emplace_back(std::make_unique<aql::Collection>(
            n, _vocbase, TRI_TRANSACTION_READ));
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
    // List of edge collection names
    for (auto const& it : VPackArrayIterator(_graphInfo.slice())) {
      if (!it.isString()) {
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                       "graph has to be an array of strings.");
      }
      std::string e = arangodb::basics::VelocyPackHelper::getStringValue(it, "");
      _edgeColls.emplace_back(
          std::make_unique<aql::Collection>(e, _vocbase, TRI_TRANSACTION_READ));
    }
    if (_edgeColls.empty()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_QUERY_BAD_JSON_PLAN,
          "graph has to be a non empty array of strings.");
    }
  }

  // Out variables
  if (base.hasKey("vertexOutVariable")) {
    _vertexOutVariable = varFromVPack(plan->getAst(), base, "vertexOutVariable");
  }
  if (base.hasKey("edgeOutVariable")) {
    _edgeOutVariable = varFromVPack(plan->getAst(), base, "edgeOutVariable");
  }

  // Flags
  if (base.hasKey("shortestPathFlags")) {
    // _options = ShortestPathOptions(base);
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

void ShortestPathNode::toVelocyPackHelper(VPackBuilder& nodes,
                                          bool verbose) const {
  ExecutionNode::toVelocyPackHelperGeneric(nodes,
                                           verbose);  // call base class method
  nodes.add("database", VPackValue(_vocbase->name()));
  nodes.add("graph", _graphInfo.slice());
  nodes.add(VPackValue("directions"));
  {
    VPackArrayBuilder guard(&nodes);
    for (auto const& d : _directions) {
      nodes.add(VPackValue(d));
    }
  }

  // In variables
  if (usesStartInVariable()) {
    nodes.add(VPackValue("startInVariable"));
    startInVariable()->toVelocyPack(nodes);
  } else {
    nodes.add("startVertexId", VPackValue(_startVertexId));
  }

  if (usesTargetInVariable()) {
    nodes.add(VPackValue("targetInVariable"));
    targetInVariable()->toVelocyPack(nodes);
  } else {
    nodes.add("targetVertexId", VPackValue(_targetVertexId));
  }

  if (_graphObj != nullptr) {
    nodes.add(VPackValue("graphDefinition"));
    _graphObj->toVelocyPack(nodes, verbose);
  }

  // Out variables
  if (usesVertexOutVariable()) {
    nodes.add(VPackValue("vertexOutVariable"));
    vertexOutVariable()->toVelocyPack(nodes);
  }
  if (usesEdgeOutVariable()) {
    nodes.add(VPackValue("edgeOutVariable"));
    edgeOutVariable()->toVelocyPack(nodes);
  }

  // nodes.add(VPackValue("shortestPathFlags"));
  // _options.toVelocyPack(nodes);

  // Traversal Filter Conditions

  TRI_ASSERT(_tmpObjVariable != nullptr);
  nodes.add(VPackValue("tmpObjVariable"));
  _tmpObjVariable->toVelocyPack(nodes);

  TRI_ASSERT(_tmpObjVarNode != nullptr);
  nodes.add(VPackValue("tmpObjVarNode"));
  _tmpObjVarNode->toVelocyPack(nodes, verbose);

  TRI_ASSERT(_tmpIdNode != nullptr);
  nodes.add(VPackValue("tmpIdNode"));
  _tmpIdNode->toVelocyPack(nodes, verbose);

  TRI_ASSERT(_fromCondition != nullptr);
  nodes.add(VPackValue("fromCondition"));
  _fromCondition->toVelocyPack(nodes, verbose);

  TRI_ASSERT(_toCondition != nullptr);
  nodes.add(VPackValue("toCondition"));
  _toCondition->toVelocyPack(nodes, verbose);


  // And close it:
  nodes.close();
}

ExecutionNode* ShortestPathNode::clone(ExecutionPlan* plan,
                                       bool withDependencies,
                                       bool withProperties) const {
  auto tmp =
      std::make_unique<arangodb::traverser::ShortestPathOptions>(*_options.get());
  auto c = new ShortestPathNode(plan, _id, _vocbase, _edgeColls, _directions,
                                _inStartVariable, _startVertexId,
                                _inTargetVariable, _targetVertexId, tmp);

  if (usesVertexOutVariable()) {
    auto vertexOutVariable = _vertexOutVariable;
    if (withProperties) {
      vertexOutVariable =
          plan->getAst()->variables()->createVariable(vertexOutVariable);
    }
    TRI_ASSERT(vertexOutVariable != nullptr);
    c->setVertexOutput(vertexOutVariable);
  }

  if (usesEdgeOutVariable()) {
    auto edgeOutVariable = _edgeOutVariable;
    if (withProperties) {
      edgeOutVariable =
          plan->getAst()->variables()->createVariable(edgeOutVariable);
    }
    TRI_ASSERT(edgeOutVariable != nullptr);
    c->setEdgeOutput(edgeOutVariable);
  }

  // Temporary Filter Objects
  c->_tmpObjVariable = _tmpObjVariable;
  c->_tmpObjVarNode = _tmpObjVarNode;
  c->_tmpIdNode = _tmpIdNode;

  // Filter Condition Parts
  c->_fromCondition = _fromCondition->clone(_plan->getAst());
  c->_toCondition = _toCondition->clone(_plan->getAst());
 
  cloneHelper(c, plan, withDependencies, withProperties);

  return static_cast<ExecutionNode*>(c);
}

double ShortestPathNode::estimateCost(size_t& nrItems) const {
  // Standard estimation for Shortest path is O(|E| + |V|*LOG(|V|))
  // At this point we know |E| but do not know |V|.
  size_t incoming = 0;
  double depCost = _dependencies.at(0)->getCost(incoming);
  auto trx = _plan->getAst()->query()->trx();
  size_t edgesCount = 0;
  double nodesEstimate = 0;
  auto collections = _plan->getAst()->query()->collections();

  for (auto const& it : _edgeColls) {

    auto collection = collections->get(it->getName());

    if (collection == nullptr) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                     "unexpected pointer for collection");
    }

    TRI_ASSERT(collection != nullptr);

    size_t edges = collection->count();

    auto indexes = trx->indexesForCollection(collection->name);
    for (auto const& index : indexes) {
      if (index->type() == arangodb::Index::IndexType::TRI_IDX_TYPE_EDGE_INDEX) {
        // We can only use Edge Index
        if (index->hasSelectivityEstimate()) {
          nodesEstimate += edges * index->selectivityEstimate();
        } else {
          // Hard-coded fallback should not happen
          nodesEstimate += edges * 0.01;
        }
        break;
      }
    }

    edgesCount += edges;
  }
  nrItems = edgesCount + static_cast<size_t>(std::log2(nodesEstimate) * nodesEstimate);
  return depCost + nrItems;
}

void ShortestPathNode::prepareOptions() {
  if (_optionsBuild) {
    return;
  }

  size_t numEdgeColls = _edgeColls.size();
  TRI_ASSERT(!_optionsBuild);
  _options->setVariable(_tmpObjVariable);

  Ast* ast = _plan->getAst();
  // FIXME: _options->_baseLookupInfos.reserve(numEdgeColls);
  // Compute Edge Indexes. First default indexes:
  for (size_t i = 0; i < numEdgeColls; ++i) {
    switch (_directions[i]) {
      case TRI_EDGE_IN:
        _options->addLookupInfo(
            ast, _edgeColls[i]->getName(), StaticStrings::ToString,
            _toCondition);
        break;
      case TRI_EDGE_OUT:
        _options->addLookupInfo(
            ast, _edgeColls[i]->getName(), StaticStrings::FromString,
            _fromCondition);
        break;
      case TRI_EDGE_ANY:
        TRI_ASSERT(false);
        break;
    }
  }

  // TODO add reverse direction.
  _optionsBuild = true;
}
