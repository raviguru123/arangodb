////////////////////////////////////////////////////////////////////////////////
/// @brief Implementation of Traversal Execution Node
///
/// @file arangod/Aql/TraversalNode.cpp
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

#include "TraversalNode.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/Ast.h"
#include "Aql/SortCondition.h"
#include "Cluster/ClusterComm.h"
#include "Indexes/Index.h"
#include "Utils/CollectionNameResolver.h"
#include "VocBase/ticks.h"
#include "VocBase/TraverserOptions.h"

#include <velocypack/Iterator.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb::basics;
using namespace arangodb::aql;
using namespace arangodb::traverser;

TraversalNode::EdgeConditionBuilder::EdgeConditionBuilder(
    TraversalNode const* tn)
    : _tn(tn), _modCondition(nullptr), _containsCondition(false) {
  _modCondition =
      _tn->_plan->getAst()->createNodeNaryOperator(NODE_TYPE_OPERATOR_NARY_AND);
}

TraversalNode::EdgeConditionBuilder::EdgeConditionBuilder(
    TraversalNode const* tn, arangodb::velocypack::Slice const& condition)
    : _tn(tn), _modCondition(nullptr), _containsCondition(false) {
  _modCondition = new AstNode(_tn->_plan->getAst(), condition);
  TRI_ASSERT(_modCondition != nullptr);
  TRI_ASSERT(_modCondition->type == NODE_TYPE_OPERATOR_NARY_AND);
}

TraversalNode::EdgeConditionBuilder::EdgeConditionBuilder(
    TraversalNode const* tn, EdgeConditionBuilder const* other)
    : _tn(tn),
      _modCondition(other->_modCondition),
      _containsCondition(other->_containsCondition) {}

void TraversalNode::EdgeConditionBuilder::addConditionPart(
    AstNode const* part) {
  _modCondition->addMember(part);
}

AstNode* TraversalNode::EdgeConditionBuilder::getOutboundCondition() {
  if (_containsCondition) {
    _modCondition->changeMember(_modCondition->numMembers() - 1,
                                _tn->_fromCondition);
  } else {
    for (auto& it : _tn->_globalEdgeConditions) {
      _modCondition->addMember(it);
    }
    TRI_ASSERT(_tn->_fromCondition != nullptr);
    TRI_ASSERT(_tn->_fromCondition->type == NODE_TYPE_OPERATOR_BINARY_EQ);
    _modCondition->addMember(_tn->_fromCondition);
    _containsCondition = true;
  }
  TRI_ASSERT(_modCondition->numMembers() > 0);
  return _modCondition;
};

AstNode* TraversalNode::EdgeConditionBuilder::getInboundCondition() {
  if (_containsCondition) {
    _modCondition->changeMember(_modCondition->numMembers() - 1, _tn->_toCondition);
  } else {
    for (auto& it : _tn->_globalEdgeConditions) {
      _modCondition->addMember(it);
    }
    TRI_ASSERT(_tn->_toCondition != nullptr);
    TRI_ASSERT(_tn->_toCondition->type == NODE_TYPE_OPERATOR_BINARY_EQ);
    _modCondition->addMember(_tn->_toCondition);
    _containsCondition = true;
  }
  TRI_ASSERT(_modCondition->numMembers() > 0);
  return _modCondition;
};

void TraversalNode::EdgeConditionBuilder::toVelocyPack(VPackBuilder& builder, bool verbose) const {
  if (_containsCondition) {
    _modCondition->removeMemberUnchecked(_modCondition->numMembers() - 1);
  }
  _modCondition->toVelocyPack(builder, verbose);
}

TraversalNode::TraversalNode(ExecutionPlan* plan, size_t id,
                             TRI_vocbase_t* vocbase, AstNode const* direction,
                             AstNode const* start, AstNode const* graph,
                             TraverserOptions* options)
    : GraphNode(plan, id, vocbase, direction, graph, options),
      _pathOutVariable(nullptr),
      _inVariable(nullptr),
      _condition(nullptr) {
  TRI_ASSERT(start != nullptr);

  // Let us build the conditions on _from and _to. Just in case we need them.
  auto ast = _plan->getAst();
  {
    auto const* access = ast->createNodeAttributeAccess(
        _tmpObjVarNode, StaticStrings::FromString.c_str(),
        StaticStrings::FromString.length());
    _fromCondition = ast->createNodeBinaryOperator(
        NODE_TYPE_OPERATOR_BINARY_EQ, access, _tmpIdNode);
  }
  TRI_ASSERT(_fromCondition != nullptr);
  TRI_ASSERT(_fromCondition->type == NODE_TYPE_OPERATOR_BINARY_EQ);

  {
    auto const* access = ast->createNodeAttributeAccess(
        _tmpObjVarNode, StaticStrings::ToString.c_str(),
        StaticStrings::ToString.length());
    _toCondition = ast->createNodeBinaryOperator(NODE_TYPE_OPERATOR_BINARY_EQ,
                                                  access, _tmpIdNode);
  }
  TRI_ASSERT(_toCondition != nullptr);
  TRI_ASSERT(_toCondition->type == NODE_TYPE_OPERATOR_BINARY_EQ);

  // Parse start node
  switch (start->type) {
    case NODE_TYPE_REFERENCE:
      _inVariable = static_cast<Variable*>(start->getData());
      _vertexId = "";
      break;
    case NODE_TYPE_VALUE:
      if (start->value.type != VALUE_TYPE_STRING) {
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_PARSE,
                                       "invalid start vertex. Must either be "
                                       "an _id string or an object with _id.");
      }
      _inVariable = nullptr;
      _vertexId = start->getString();
      break;
    default:
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_PARSE,
                                     "invalid start vertex. Must either be an "
                                     "_id string or an object with _id.");
  }

  // Parse options node

#ifdef TRI_ENABLE_MAINTAINER_MODE
  checkConditionsDefined();
#endif
}

/// @brief Internal constructor to clone the node.
TraversalNode::TraversalNode(
    ExecutionPlan* plan, size_t id, TRI_vocbase_t* vocbase,
    std::vector<std::unique_ptr<Collection>> const& edgeColls,
    std::vector<std::unique_ptr<Collection>> const& vertexColls,
    Variable const* inVariable, std::string const& vertexId,
    std::vector<TRI_edge_direction_e> const& directions,
    TraverserOptions* options)
    : GraphNode(plan, id, vocbase, edgeColls, vertexColls, directions, options),
      _pathOutVariable(nullptr),
      _inVariable(inVariable),
      _vertexId(vertexId),
      _condition(nullptr) {
}

TraversalNode::TraversalNode(ExecutionPlan* plan,
                             arangodb::velocypack::Slice const& base)
    : GraphNode(plan, base),
      _pathOutVariable(nullptr),
      _inVariable(nullptr),
      _condition(nullptr) {
  _options = std::make_unique<traverser::TraverserOptions>(
      _plan->getAst()->query()->trx(), base);
  // In Vertex
  if (base.hasKey("inVariable")) {
    _inVariable = varFromVPack(plan->getAst(), base, "inVariable");
  } else {
    VPackSlice v = base.get("vertexId");
    if (!v.isString()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                     "start vertex must be a string");
    }
    _vertexId = v.copyString();
    if (_vertexId.empty()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                     "start vertex mustn't be empty");
    }
  }

  if (base.hasKey("condition")) {
    VPackSlice condition = base.get("condition");
    if (!condition.isObject()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                     "condition must be an object");
    }
    _condition = Condition::fromVPack(plan, condition);
  }
  auto list = base.get("conditionVariables");

  if (list.isArray()) {
    for (auto const& v : VPackArrayIterator(list)) {
      _conditionVariables.emplace(
          _plan->getAst()->variables()->createVariable(v));
    }
  }

  // Out variables
  if (base.hasKey("pathOutVariable")) {
    _pathOutVariable = varFromVPack(plan->getAst(), base, "pathOutVariable");
  }

  list = base.get("globalEdgeConditions");
  if (list.isArray()) {
    for (auto const& cond : VPackArrayIterator(list)) {
      _globalEdgeConditions.emplace_back(new AstNode(plan->getAst(), cond));
    }
  }

  list = base.get("globalVertexConditions");
  if (list.isArray()) {
    for (auto const& cond : VPackArrayIterator(list)) {
      _globalVertexConditions.emplace_back(new AstNode(plan->getAst(), cond));
    }
  }

  list = base.get("vertexConditions");
  if (list.isObject()) {
    for (auto const& cond : VPackObjectIterator(list)) {
      std::string key = cond.key.copyString();
      _vertexConditions.emplace(StringUtils::uint64(key),
                                new AstNode(plan->getAst(), cond.value));
    }
  }


  list = base.get("edgeConditions");
  if (list.isObject()) {
    for (auto const& cond : VPackObjectIterator(list)) {
      std::string key = cond.key.copyString();
      auto ecbuilder = std::make_unique<EdgeConditionBuilder>(this, cond.value);
      _edgeConditions.emplace(StringUtils::uint64(key), std::move(ecbuilder));
    }
  }

#ifdef TRI_ENABLE_MAINTAINER_MODE
  checkConditionsDefined();
#endif
}

TraversalNode::~TraversalNode() {
  if (_condition != nullptr) {
    delete _condition;
  }
}

int TraversalNode::checkIsOutVariable(size_t variableId) const {
  if (_vertexOutVariable != nullptr && _vertexOutVariable->id == variableId) {
    return 0;
  }
  if (_edgeOutVariable != nullptr && _edgeOutVariable->id == variableId) {
    return 1;
  }
  if (_pathOutVariable != nullptr && _pathOutVariable->id == variableId) {
    return 2;
  }
  return -1;
}

/// @brief check whether an access is inside the specified range
bool TraversalNode::isInRange(uint64_t depth, bool isEdge) const {
  uint64_t max = static_cast<TraverserOptions*>(_options.get())->maxDepth;
  if (isEdge) {
    return (depth < max);
  }
  return (depth <= max);
}

/// @brief check if all directions are equal
bool TraversalNode::allDirectionsEqual() const {
  if (_directions.empty()) {
    // no directions!
    return false;
  }
  size_t const n = _directions.size();
  TRI_edge_direction_e const expected = _directions[0];

  for (size_t i = 1; i < n; ++i) {
    if (_directions[i] != expected) {
      return false;
    }
  }
  return true;
}

void TraversalNode::toVelocyPackHelper(arangodb::velocypack::Builder& nodes,
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

  nodes.add(VPackValue("edgeCollections"));
  {
    VPackArrayBuilder guard(&nodes);
    for (auto const& e : _edgeColls) {
      nodes.add(VPackValue(e->getName()));
    }
  }

  nodes.add(VPackValue("vertexCollections"));
  {
    VPackArrayBuilder guard(&nodes);
    for (auto const& v : _vertexColls) {
      nodes.add(VPackValue(v->getName()));
    }
  }

  // In variable
  if (usesInVariable()) {
    nodes.add(VPackValue("inVariable"));
    inVariable()->toVelocyPack(nodes);
  } else {
    nodes.add("vertexId", VPackValue(_vertexId));
  }

  if (_condition != nullptr) {
    nodes.add(VPackValue("condition"));
    _condition->toVelocyPack(nodes, verbose);
  }

  if (!_conditionVariables.empty()) {
    nodes.add(VPackValue("conditionVariables"));
    nodes.openArray();
    for (auto const& it : _conditionVariables) {
      it->toVelocyPack(nodes);
    }
    nodes.close();
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
  if (usesPathOutVariable()) {
    nodes.add(VPackValue("pathOutVariable"));
    pathOutVariable()->toVelocyPack(nodes);
  }

  nodes.add(VPackValue("traversalFlags"));
  _options->toVelocyPack(nodes);

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

  if (!_globalEdgeConditions.empty()) {
    nodes.add(VPackValue("globalEdgeConditions"));
    nodes.openArray();
    for (auto const& it : _globalEdgeConditions) {
      it->toVelocyPack(nodes, verbose);
    }
    nodes.close();
  }

  if (!_globalVertexConditions.empty()) {
    nodes.add(VPackValue("globalVertexConditions"));
    nodes.openArray();
    for (auto const& it : _globalVertexConditions) {
      it->toVelocyPack(nodes, verbose);
    }
  }

  if (!_vertexConditions.empty()) {
    nodes.add(VPackValue("vertexConditions"));
    nodes.openObject();
    for (auto const& it : _vertexConditions) {
      nodes.add(VPackValue(basics::StringUtils::itoa(it.first)));
      it.second->toVelocyPack(nodes, verbose);
    }
    nodes.close();
  }

  if (!_edgeConditions.empty()) {
    nodes.add(VPackValue("edgeConditions"));
    nodes.openObject();
    for (auto const& it : _edgeConditions) {
      nodes.add(VPackValue(basics::StringUtils::itoa(it.first)));
      it.second->toVelocyPack(nodes, verbose);
    }
    nodes.close();
  }

  nodes.add(VPackValue("indexes"));
  _options->toVelocyPackIndexes(nodes);

  // And close it:
  nodes.close();
}

/// @brief clone ExecutionNode recursively
ExecutionNode* TraversalNode::clone(ExecutionPlan* plan, bool withDependencies,
                                    bool withProperties) const {
  TRI_ASSERT(!_optionsBuild);
  auto tmp =
      std::make_unique<arangodb::traverser::TraverserOptions>(*options());
  auto c = new TraversalNode(plan, _id, _vocbase, _edgeColls, _vertexColls,
                             _inVariable, _vertexId, _directions, tmp.get());
  tmp.release();

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

  if (usesPathOutVariable()) {
    auto pathOutVariable = _pathOutVariable;
    if (withProperties) {
      pathOutVariable =
          plan->getAst()->variables()->createVariable(pathOutVariable);
    }
    TRI_ASSERT(pathOutVariable != nullptr);
    c->setPathOutput(pathOutVariable);
  }

  c->_conditionVariables.reserve(_conditionVariables.size());
  for (auto const& it: _conditionVariables) {
    c->_conditionVariables.emplace(it->clone());
  }

#ifdef TRI_ENABLE_MAINTAINER_MODE
  checkConditionsDefined();
#endif

  // Temporary Filter Objects
  c->_tmpObjVariable = _tmpObjVariable;
  c->_tmpObjVarNode = _tmpObjVarNode;
  c->_tmpIdNode = _tmpIdNode;

  // Filter Condition Parts
  c->_fromCondition = _fromCondition->clone(_plan->getAst());
  c->_toCondition = _toCondition->clone(_plan->getAst());
  c->_globalEdgeConditions.insert(c->_globalEdgeConditions.end(),
                                  _globalEdgeConditions.begin(),
                                  _globalEdgeConditions.end());
  c->_globalVertexConditions.insert(c->_globalVertexConditions.end(),
                                    _globalVertexConditions.begin(),
                                    _globalVertexConditions.end());

  for (auto const& it : _edgeConditions) {
    // Copy the builder
    auto ecBuilder = std::make_unique<EdgeConditionBuilder>(this, it.second.get());
    c->_edgeConditions.emplace(it.first, std::move(ecBuilder));
  }

  for (auto const& it : _vertexConditions) {
    c->_vertexConditions.emplace(it.first, it.second->clone(_plan->getAst()));
  }

#ifdef TRI_ENABLE_MAINTAINER_MODE
  c->checkConditionsDefined();
#endif



  cloneHelper(c, plan, withDependencies, withProperties);

  return static_cast<ExecutionNode*>(c);
}

/// @brief the cost of a traversal node
double TraversalNode::estimateCost(size_t& nrItems) const {
  size_t incoming = 0;
  double depCost = _dependencies.at(0)->getCost(incoming);
  double expectedEdgesPerDepth = 0.0;
  auto trx = _plan->getAst()->query()->trx();
  auto collections = _plan->getAst()->query()->collections();

  TRI_ASSERT(collections != nullptr);

  for (auto const& it : _edgeColls) {
    auto collection = collections->get(it->getName());

    if (collection == nullptr) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                     "unexpected pointer for collection");
    }

    TRI_ASSERT(collection != nullptr);

    auto indexes = trx->indexesForCollection(collection->name);
    for (auto const& index : indexes) {
      if (index->type() == arangodb::Index::IndexType::TRI_IDX_TYPE_EDGE_INDEX) {
        // We can only use Edge Index
        if (index->hasSelectivityEstimate()) {
          expectedEdgesPerDepth += 1 / index->selectivityEstimate();
        } else {
          expectedEdgesPerDepth += 1000;  // Hard-coded
        }
        break;
      }
    }
  }

  uint64_t max = static_cast<double>(
      static_cast<TraverserOptions*>(_options.get())->maxDepth);
  nrItems =
      static_cast<size_t>(incoming * std::pow(expectedEdgesPerDepth, max));
  if (nrItems == 0 && incoming > 0) {
    nrItems = 1;  // min value
  }
  return depCost + nrItems;
}

void TraversalNode::prepareOptions() {
  if (_optionsBuild) {
    return;
  }
  TRI_ASSERT(!_optionsBuild);
  _options->setVariable(_tmpObjVariable);

  size_t numEdgeColls = _edgeColls.size();
  bool res = false;
  EdgeConditionBuilder globalEdgeConditionBuilder(this);
  Ast* ast = _plan->getAst();
  auto trx = ast->query()->trx();

  // FIXME: _options->_baseLookupInfos.reserve(numEdgeColls);
  // Compute Edge Indexes. First default indexes:
  for (size_t i = 0; i < numEdgeColls; ++i) {
    switch (_directions[i]) {
      case TRI_EDGE_IN:
        _options->addLookupInfo(
            ast, _edgeColls[i]->getName(), StaticStrings::ToString,
            globalEdgeConditionBuilder.getInboundCondition()->clone(ast));
        break;
      case TRI_EDGE_OUT:
        _options->addLookupInfo(
            ast, _edgeColls[i]->getName(), StaticStrings::FromString,
            globalEdgeConditionBuilder.getOutboundCondition()->clone(ast));
        break;
      case TRI_EDGE_ANY:
        TRI_ASSERT(false);
        break;
    }
  }

  auto opts = static_cast<traverser::TraverserOptions*>(_options.get());
  for (auto& it : _edgeConditions) {
    auto ins = opts->_depthLookupInfo.emplace(
        it.first, std::vector<traverser::TraverserOptions::LookupInfo>());
    // We probably have to adopt minDepth. We cannot fulfill a condition of larger depth anyway
    TRI_ASSERT(ins.second);
    auto& infos = ins.first->second;
    infos.reserve(numEdgeColls);
    auto& builder = it.second;

    for (size_t i = 0; i < numEdgeColls; ++i) {
      std::string usedField;
      auto dir = _directions[i];
      // TODO we can optimize here. indexCondition and Expression could be
      // made non-overlapping.
      traverser::TraverserOptions::LookupInfo info;
      switch (dir) {
        case TRI_EDGE_IN:
          usedField = StaticStrings::ToString;
          info.indexCondition = builder->getInboundCondition()->clone(ast);
          break;
        case TRI_EDGE_OUT:
          usedField = StaticStrings::FromString;
          info.indexCondition = builder->getOutboundCondition()->clone(ast);
          break;
        case TRI_EDGE_ANY:
          TRI_ASSERT(false);
          break;
      }

      info.expression = new Expression(ast, info.indexCondition->clone(ast));
      res = trx->getBestIndexHandleForFilterCondition(
          _edgeColls[i]->getName(), info.indexCondition, _tmpObjVariable, 1000,
          info.idxHandles[0]);
      TRI_ASSERT(res);  // Right now we have an enforced edge index which will
                        // always fit.
      if (!res) {
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, "expected edge index not found");
      }

      // We now have to check if we need _from / _to inside the index lookup and which position
      // it is used in. Such that the traverser can update the respective string value
      // in-place
      // TODO This place can be optimized.
      if (info.idxHandles[0].isEdgeIndex()) {
        // Special case for edge index....
        // It serves two attributes, but can only be asked for one of them...
        info.conditionNeedUpdate = true;
        info.conditionMemberToUpdate = 0;
      } else {
        std::vector<std::vector<std::string>> fieldNames =
            info.idxHandles[0].fieldNames();
        for (size_t i = 0; i < fieldNames.size(); ++i) {
          auto f = fieldNames[i];
          if (f.size() == 1 && f[0] == usedField) {
            // we only work for _from and _to not _from.foo which would be null anyways...
            info.conditionNeedUpdate = true;
            info.conditionMemberToUpdate = i;
            break;
          }
        }
      }

      infos.emplace_back(std::move(info));
    }
  }

  for (auto& it : _vertexConditions) {
    // We inject the base conditions as well here.
    for (auto const& jt : _globalVertexConditions) {
      it.second->addMember(jt);
    }
    opts->_vertexExpressions.emplace(it.first, new Expression(ast, it.second));
    TRI_ASSERT(!opts->_vertexExpressions[it.first]->isV8());
  }
  if (!_globalVertexConditions.empty()) {
    auto cond = _plan->getAst()->createNodeNaryOperator(NODE_TYPE_OPERATOR_NARY_AND);
    for (auto const& it : _globalVertexConditions) {
      cond->addMember(it);
    }
    opts->_baseVertexExpression = new Expression(ast, cond);
    TRI_ASSERT(!opts->_baseVertexExpression->isV8());

  }
  _optionsBuild = true;
}

void TraversalNode::addEngine(TraverserEngineID const& engine,
                              arangodb::ServerID const& server) {
  TRI_ASSERT(arangodb::ServerState::instance()->isCoordinator());
  _engines.emplace(server, engine);
}

/// @brief remember the condition to execute for early traversal abortion.
void TraversalNode::setCondition(arangodb::aql::Condition* condition) {
  std::unordered_set<Variable const*> varsUsedByCondition;

  Ast::getReferencedVariables(condition->root(), varsUsedByCondition);

  for (auto const& oneVar : varsUsedByCondition) {
    if ((_vertexOutVariable == nullptr || oneVar->id != _vertexOutVariable->id) &&
        (_edgeOutVariable == nullptr || oneVar->id != _edgeOutVariable->id) &&
        (_pathOutVariable == nullptr || oneVar->id != _pathOutVariable->id) &&
        (_inVariable == nullptr || oneVar->id != _inVariable->id)) {
      _conditionVariables.emplace(oneVar);
    }
  }

  _condition = condition;
}

void TraversalNode::registerCondition(bool isConditionOnEdge,
                                      size_t conditionLevel,
                                      AstNode const* condition) {
  Ast::getReferencedVariables(condition, _conditionVariables);
  if (isConditionOnEdge) {
    auto const& it = _edgeConditions.find(conditionLevel);
    if (it == _edgeConditions.end()) {
      auto builder = std::make_unique<EdgeConditionBuilder>(this);
      builder->addConditionPart(condition);
      _edgeConditions.emplace(conditionLevel, std::move(builder));
    } else {
      it->second->addConditionPart(condition);
    }
  } else {
    auto const& it = _vertexConditions.find(conditionLevel);
    if (it == _vertexConditions.end()) {
      auto cond = _plan->getAst()->createNodeNaryOperator(NODE_TYPE_OPERATOR_NARY_AND);
      cond->addMember(condition);
      _vertexConditions.emplace(conditionLevel, cond);
    } else {
      it->second->addMember(condition);
    }
  }
}

void TraversalNode::registerGlobalCondition(bool isConditionOnEdge,
                                            AstNode const* condition) {
  Ast::getReferencedVariables(condition, _conditionVariables);
  if (isConditionOnEdge) {
    _globalEdgeConditions.emplace_back(condition);
  } else {
    _globalVertexConditions.emplace_back(condition);
  }
}

arangodb::traverser::TraverserOptions* TraversalNode::options() const {
  return static_cast<traverser::TraverserOptions*>(_options.get());
}

AstNode* TraversalNode::getTemporaryRefNode() const {
  return _tmpObjVarNode;
}

Variable const* TraversalNode::getTemporaryVariable() const {
  return _tmpObjVariable;
}

void TraversalNode::getConditionVariables(
    std::vector<Variable const*>& res) const {
  for (auto const& it : _conditionVariables) {
    if (it != _tmpObjVariable) {
      res.emplace_back(it);
    }
  }
}

#ifndef USE_ENTERPRISE
void TraversalNode::enhanceEngineInfo(VPackBuilder& builder) const {
  if (_graphObj != nullptr) {
    _graphObj->enhanceEngineInfo(builder);
  } else {
    // TODO enhance the Info based on EdgeCollections.
  }
}
#endif

#ifdef TRI_ENABLE_MAINTAINER_MODE
void TraversalNode::checkConditionsDefined() const {
  TRI_ASSERT(_tmpObjVariable != nullptr);
  TRI_ASSERT(_tmpObjVarNode != nullptr);
  TRI_ASSERT(_tmpIdNode != nullptr);

  TRI_ASSERT(_fromCondition != nullptr);
  TRI_ASSERT(_fromCondition->type == NODE_TYPE_OPERATOR_BINARY_EQ);

  TRI_ASSERT(_toCondition != nullptr);
  TRI_ASSERT(_toCondition->type == NODE_TYPE_OPERATOR_BINARY_EQ);
}
#endif
