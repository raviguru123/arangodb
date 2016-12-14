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

#include "ShortestPathBlock.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionPlan.h"
#include "Cluster/ClusterComm.h"
#ifdef USE_ENTERPRISE
#include "Enterprise/Cluster/SmartGraphPathFinder.h"
#endif
#include "Indexes/IndexElement.h"
#include "Utils/AqlTransaction.h"
#include "Utils/OperationCursor.h"
#include "Utils/Transaction.h"
#include "VocBase/EdgeCollectionInfo.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/ManagedDocumentResult.h"
#include "VocBase/ticks.h"

#include <velocypack/Iterator.h>
#include <velocypack/velocypack-aliases.h>

////////////////////////////////////////////////////////////////////////////////
/// @brief typedef the template instantiation of the PathFinder
////////////////////////////////////////////////////////////////////////////////

typedef arangodb::basics::DynamicDistanceFinder<
    VPackSlice, VPackSlice, double, arangodb::traverser::ShortestPath>
    ArangoDBPathFinder;

typedef arangodb::basics::ConstDistanceFinder<
    VPackSlice, VPackSlice, arangodb::basics::VelocyPackHelper::VPackStringHash,
    arangodb::basics::VelocyPackHelper::VPackStringEqual,
    arangodb::traverser::ShortestPath>
    ArangoDBConstDistancePathFinder;

#ifdef USE_ENTERPRISE
using SmartGraphConstDistanceFinder = arangodb::traverser::SmartGraphConstDistanceFinder;
#endif

using namespace arangodb::aql;

////////////////////////////////////////////////////////////////////////////////
/// @brief Local class to expand edges.
///        Will be handed over to the path finder
////////////////////////////////////////////////////////////////////////////////

namespace arangodb {
namespace aql {
struct ConstDistanceExpanderLocal {
 private:
  /// @brief reference to the Block
  ShortestPathBlock const* _block; 

  /// @brief Defines if this expander follows the edges in reverse
  bool _isReverse;

  /// @brief Local cursor vector
  std::vector<IndexLookupResult> _cursor;

 public:
  ConstDistanceExpanderLocal(ShortestPathBlock const* block,
                             bool isReverse)
      : _block(block), _isReverse(isReverse) {}

  void operator()(VPackSlice const& v, std::vector<VPackSlice>& resEdges,
                  std::vector<VPackSlice>& neighbors) {
    ManagedDocumentResult* mmdr = _block->_mmdr.get();
    std::unique_ptr<arangodb::OperationCursor> edgeCursor;
    for (auto const& edgeCollection : _block->_collectionInfos) {
      TRI_ASSERT(edgeCollection != nullptr);
      if (_isReverse) {
        edgeCursor = edgeCollection->getReverseEdges(v, mmdr);
      } else {
        edgeCursor = edgeCollection->getEdges(v, mmdr);
      }
    
      // Clear the local cursor before using the
      // next edge cursor.
      // While iterating over the edge cursor, _cursor
      // has to stay intact.
      _cursor.clear();
      LogicalCollection* collection = edgeCursor->collection();
      while (edgeCursor->hasMore()) {
        edgeCursor->getMoreMptr(_cursor, UINT64_MAX);
        for (auto const& element : _cursor) {
          TRI_voc_rid_t revisionId = element.revisionId();
          if (collection->readRevision(_block->transaction(), *mmdr, revisionId)) {
            VPackSlice edge(mmdr->vpack());
            VPackSlice from =
                arangodb::Transaction::extractFromFromDocument(edge);
            if (from == v) {
              VPackSlice to = arangodb::Transaction::extractToFromDocument(edge);
              if (to != v) {
                resEdges.emplace_back(edge);
                neighbors.emplace_back(to);
              }
            } else {
              resEdges.emplace_back(edge);
              neighbors.emplace_back(from);
            }
          }
        }
      }
    }
  }
};

////////////////////////////////////////////////////////////////////////////////
/// @brief Cluster class to expand edges.
///        Will be handed over to the path finder
////////////////////////////////////////////////////////////////////////////////

struct ConstDistanceExpanderCluster {
 private:

  /// @brief reference to the Block
  ShortestPathBlock* _block; 

  /// @brief Defines if this expander follows the edges in reverse
  bool _isReverse;

 public:
  ConstDistanceExpanderCluster(ShortestPathBlock* block, bool isReverse)
      : _block(block), _isReverse(isReverse) {}

  void operator()(VPackSlice const& v, std::vector<VPackSlice>& resEdges,
                  std::vector<VPackSlice>& neighbors) {
    int res = TRI_ERROR_NO_ERROR;
    for (auto const& edgeCollection : _block->_collectionInfos) {
      VPackBuilder result;
      TRI_ASSERT(edgeCollection != nullptr);
      if (_isReverse) {
        res = edgeCollection->getReverseEdgesCoordinator(v, result);
      } else {
        res = edgeCollection->getEdgesCoordinator(v, result);
      }

      if (res != TRI_ERROR_NO_ERROR) {
        THROW_ARANGO_EXCEPTION(res);
      }

      VPackSlice edges = result.slice().get("edges");
      for (auto const& edge : VPackArrayIterator(edges)) {
        VPackSlice from = arangodb::Transaction::extractFromFromDocument(edge);
        if (from == v) {
          VPackSlice to = arangodb::Transaction::extractToFromDocument(edge);
          if (to != v) {
            resEdges.emplace_back(edge);
            neighbors.emplace_back(to);
          }
        } else {
          resEdges.emplace_back(edge);
          neighbors.emplace_back(from);
        }
      }
      // Make sure the data Slices are pointing to is not running out of scope.
      // This is not thread-safe!
      _block->_coordinatorCache.emplace_back(result.steal());
    }
  }
};

////////////////////////////////////////////////////////////////////////////////
/// @brief Expander for weighted edges
////////////////////////////////////////////////////////////////////////////////

struct EdgeWeightExpanderLocal {

 private:

  /// @brief reference to the Block
  ShortestPathBlock const* _block; 

  /// @brief Defines if this expander follows the edges in reverse
  bool _reverse;

 public:
  EdgeWeightExpanderLocal(
      ShortestPathBlock const* block, bool reverse)
      : _block(block), _reverse(reverse) {}

  void inserter(std::unordered_map<VPackSlice, size_t>& candidates,
                std::vector<ArangoDBPathFinder::Step*>& result,
                VPackSlice const& s, VPackSlice const& t, double currentWeight,
                VPackSlice edge) {
    auto cand = candidates.find(t);
    if (cand == candidates.end()) {
      // Add weight
      auto step = std::make_unique<ArangoDBPathFinder::Step>(
          t, s, currentWeight, edge);
      result.emplace_back(step.release());
      candidates.emplace(t, result.size() - 1);
    } else {
      // Compare weight
      auto old = result[cand->second];
      auto oldWeight = old->weight();
      if (currentWeight < oldWeight) {
        old->setWeight(currentWeight);
        old->_predecessor = s;
        old->_edge = edge;
      }
    }
  }

  void operator()(VPackSlice const& source,
                  std::vector<ArangoDBPathFinder::Step*>& result) {
    ManagedDocumentResult* mmdr = _block->_mmdr.get();
    std::vector<IndexLookupResult> cursor;
    std::unique_ptr<arangodb::OperationCursor> edgeCursor;
    std::unordered_map<VPackSlice, size_t> candidates;
    for (auto const& edgeCollection : _block->_collectionInfos) {
      TRI_ASSERT(edgeCollection != nullptr);
      if (_reverse) {
        edgeCursor = edgeCollection->getReverseEdges(source, mmdr);
      } else {
        edgeCursor = edgeCollection->getEdges(source, mmdr);
      }
      
      candidates.clear();

      // Clear the local cursor before using the
      // next edge cursor.
      // While iterating over the edge cursor, _cursor
      // has to stay intact.
      cursor.clear();
      LogicalCollection* collection = edgeCursor->collection();
      while (edgeCursor->hasMore()) {
        edgeCursor->getMoreMptr(cursor, UINT64_MAX);
        for (auto const& element : cursor) {
          TRI_voc_rid_t revisionId = element.revisionId();
          if (collection->readRevision(_block->transaction(), *mmdr, revisionId)) {
            VPackSlice edge(mmdr->vpack());
            VPackSlice from =
                arangodb::Transaction::extractFromFromDocument(edge);
            VPackSlice to = arangodb::Transaction::extractToFromDocument(edge);
            double currentWeight = edgeCollection->weightEdge(edge);
            if (from == source) {
              inserter(candidates, result, from, to, currentWeight, edge);
            } else {
              inserter(candidates, result, to, from, currentWeight, edge);
            }
          }
        }
      }
    }
  }
};

////////////////////////////////////////////////////////////////////////////////
/// @brief Expander for weighted edges
////////////////////////////////////////////////////////////////////////////////

struct EdgeWeightExpanderCluster {

 private:

  /// @brief reference to the Block
  ShortestPathBlock* _block;

  /// @brief Defines if this expander follows the edges in reverse
  bool _reverse;

 public:
  EdgeWeightExpanderCluster(ShortestPathBlock* block, bool reverse)
      : _block(block), _reverse(reverse) {}

  void operator()(VPackSlice const& source,
                  std::vector<ArangoDBPathFinder::Step*>& result) {
    int res = TRI_ERROR_NO_ERROR;
    std::unordered_map<VPackSlice, size_t> candidates;

    for (auto const& edgeCollection : _block->_collectionInfos) {
      TRI_ASSERT(edgeCollection != nullptr);
      VPackBuilder edgesBuilder;
      if (_reverse) {
        res = edgeCollection->getReverseEdgesCoordinator(source, edgesBuilder);
      } else {
        res = edgeCollection->getEdgesCoordinator(source, edgesBuilder);
      }

      if (res != TRI_ERROR_NO_ERROR) {
        THROW_ARANGO_EXCEPTION(res);
      }

      candidates.clear();

      auto inserter = [&](VPackSlice const& s, VPackSlice const& t,
                          double currentWeight, VPackSlice const& edge) {
        auto cand = candidates.find(t);
        if (cand == candidates.end()) {
          // Add weight
          auto step = std::make_unique<ArangoDBPathFinder::Step>(
              t, s, currentWeight, edge);
          result.emplace_back(step.release());
          candidates.emplace(t, result.size() - 1);
        } else {
          // Compare weight
          auto old = result[cand->second];
          auto oldWeight = old->weight();
          if (currentWeight < oldWeight) {
            old->setWeight(currentWeight);
            old->_predecessor = s;
            old->_edge = edge;
          }
        }
      };

      VPackSlice edges = edgesBuilder.slice().get("edges");
      for (auto const& edge : VPackArrayIterator(edges)) {
        VPackSlice from = arangodb::Transaction::extractFromFromDocument(edge);
        VPackSlice to = arangodb::Transaction::extractToFromDocument(edge);
        double currentWeight = edgeCollection->weightEdge(edge);
        if (from == source) {
          inserter(from, to, currentWeight, edge);
        } else {
          inserter(to, from, currentWeight, edge);
        }
      }
      _block->_coordinatorCache.emplace_back(edgesBuilder.steal());
    }
  }
};
}
}

ShortestPathBlock::ShortestPathBlock(ExecutionEngine* engine,
                                     ShortestPathNode const* ep)
    : ExecutionBlock(engine, ep),
      _vertexVar(nullptr),
      _edgeVar(nullptr),
      _opts(ep->options()),
      _posInPath(0),
      _pathLength(0),
      _path(nullptr),
      _useStartRegister(false),
      _useTargetRegister(false),
      _usedConstant(false) {
  _mmdr.reset(new ManagedDocumentResult(_trx));

  size_t count = ep->_edgeColls.size();
  TRI_ASSERT(ep->_directions.size());
  _collectionInfos.reserve(count);

  for (size_t j = 0; j < count; ++j) {
    auto info = std::make_unique<arangodb::traverser::EdgeCollectionInfo>(
        _trx, ep->_edgeColls[j]->getName(), ep->_directions[j],
        _opts->weightAttribute(), _opts->defaultWeight());
    _collectionInfos.emplace_back(info.get());
    info.release();
  }

  if (!ep->usesStartInVariable()) {
    _startVertexId = ep->getStartVertex();
  } else {
    auto it = ep->getRegisterPlan()->varInfo.find(ep->startInVariable()->id);
    TRI_ASSERT(it != ep->getRegisterPlan()->varInfo.end());
    _startReg = it->second.registerId;
    _useStartRegister = true;
  }

  if (!ep->usesTargetInVariable()) {
    _targetVertexId = ep->getTargetVertex();
  } else {
    auto it = ep->getRegisterPlan()->varInfo.find(ep->targetInVariable()->id);
    TRI_ASSERT(it != ep->getRegisterPlan()->varInfo.end());
    _targetReg = it->second.registerId;
    _useTargetRegister = true;
  }

  if (ep->usesVertexOutVariable()) {
    _vertexVar = ep->vertexOutVariable();
  }

  if (ep->usesEdgeOutVariable()) {
    _edgeVar = ep->edgeOutVariable();
  }
  _path = std::make_unique<arangodb::traverser::ShortestPath>();

  if (arangodb::ServerState::instance()->isCoordinator()) {
    _engines = ep->engines();

    if (_opts->usesWeight()) {
      _finder.reset(new ArangoDBPathFinder(
          EdgeWeightExpanderCluster(this, false),
          EdgeWeightExpanderCluster(this, true), true));
    } else {
#ifdef USE_ENTERPRISE
      if (ep->isSmart()) {
        _finder.reset(new SmartGraphConstDistanceFinder(
            _opts, _engines, engine->getQuery()->trx()->resolver()));
      } else {
#endif
        _finder.reset(new ArangoDBConstDistancePathFinder(
            ConstDistanceExpanderCluster(this, false),
            ConstDistanceExpanderCluster(this, true)));
#ifdef USE_ENTERPRISE
    }
#endif
    }
  } else {
    if (_opts->usesWeight()) {
      _finder.reset(new ArangoDBPathFinder(
          EdgeWeightExpanderLocal(this, false),
          EdgeWeightExpanderLocal(this, true), true));
    } else {
      _finder.reset(new ArangoDBConstDistancePathFinder(
          ConstDistanceExpanderLocal(this, false),
          ConstDistanceExpanderLocal(this, true)));
    }
  }
}

ShortestPathBlock::~ShortestPathBlock() {
  for (auto& it : _collectionInfos) {
    delete it;
  }
}

int ShortestPathBlock::initialize() {
  DEBUG_BEGIN_BLOCK();
  int res = ExecutionBlock::initialize();
  auto varInfo = getPlanNode()->getRegisterPlan()->varInfo;

  if (usesVertexOutput()) {
    TRI_ASSERT(_vertexVar != nullptr);
    auto it = varInfo.find(_vertexVar->id);
    TRI_ASSERT(it != varInfo.end());
    TRI_ASSERT(it->second.registerId < ExecutionNode::MaxRegisterId);
    _vertexReg = it->second.registerId;
  }
  if (usesEdgeOutput()) {
    TRI_ASSERT(_edgeVar != nullptr);
    auto it = varInfo.find(_edgeVar->id);
    TRI_ASSERT(it != varInfo.end());
    TRI_ASSERT(it->second.registerId < ExecutionNode::MaxRegisterId);
    _edgeReg = it->second.registerId;
  }

  return res;

  // cppcheck-suppress style
  DEBUG_END_BLOCK();
}

int ShortestPathBlock::initializeCursor(AqlItemBlock* items, size_t pos) {
  _posInPath = 0;
  _pathLength = 0;
  _usedConstant = false;
  return ExecutionBlock::initializeCursor(items, pos);
}

/// @brief shutdown: Inform all traverser Engines to destroy themselves
int ShortestPathBlock::shutdown(int errorCode) {
  DEBUG_BEGIN_BLOCK();
  // We have to clean up the engines in Coordinator Case.
  if (arangodb::ServerState::instance()->isCoordinator()) {
    auto cc = arangodb::ClusterComm::instance();
    std::string const url(
        "/_db/" + arangodb::basics::StringUtils::urlEncode(_trx->vocbase()->name()) +
        "/_internal/traverser/");
    for (auto const& it : *_engines) {
      arangodb::CoordTransactionID coordTransactionID = TRI_NewTickServer();
      std::unordered_map<std::string, std::string> headers;
      auto res = cc->syncRequest(
          "", coordTransactionID, "server:" + it.first, RequestType::DELETE_REQ,
          url + arangodb::basics::StringUtils::itoa(it.second), "", headers,
          30.0);
      if (res->status != CL_COMM_SENT) {
        // Note If there was an error on server side we do not have CL_COMM_SENT
        std::string message("Could not destruct all traversal engines");
        if (res->errorMessage.length() > 0) {
          message += std::string(" : ") + res->errorMessage;
        }
        LOG(ERR) << message;
      }
    }
  }

  return ExecutionBlock::shutdown(errorCode);

  // cppcheck-suppress style
  DEBUG_END_BLOCK();
}

bool ShortestPathBlock::nextPath(AqlItemBlock const* items) {
  if (_usedConstant) {
    // Both source and target are constant.
    // Just one path to compute
    return false;
  }
  _path->clear();
  if (!_useStartRegister && !_useTargetRegister) {
    // Both are constant, after this computation we are done
    _usedConstant = true;
  }
  startBuilder.clear();
  targetBuilder.clear();
  
  VPackSlice start;
  VPackSlice end;
  if (!_useStartRegister) {
    auto pos = _startVertexId.find('/');
    if (pos == std::string::npos) {
      _engine->getQuery()->registerWarning(TRI_ERROR_BAD_PARAMETER,
                                           "Invalid input for Shortest Path: "
                                           "Only id strings or objects with "
                                           "_id are allowed");
      return false;
    } else {
      startBuilder.add(VPackValue(_startVertexId));
      start = startBuilder.slice();
    }
  } else {
    AqlValue const& in = items->getValueReference(_pos, _startReg);
    if (in.isObject()) {
      try {
        startBuilder.add(VPackValue(_trx->extractIdString(in.slice())));
        start = startBuilder.slice();
      }
      catch (...) {
        // _id or _key not present... ignore this error and fall through
        // returning no path
        return false;
      }
    } else if (in.isString()) {
      start = in.slice();
    } else {
      _engine->getQuery()->registerWarning(
          TRI_ERROR_BAD_PARAMETER, "Invalid input for Shortest Path: Only "
                                       "id strings or objects with _id are "
                                       "allowed");
      return false;
    }

  }

  if (!_useTargetRegister) {
    auto pos = _targetVertexId.find('/');
    if (pos == std::string::npos) {
      _engine->getQuery()->registerWarning(
          TRI_ERROR_BAD_PARAMETER, "Invalid input for Shortest Path: "
                                       "Only id strings or objects with "
                                       "_id are allowed");
      return false;
    } else {
      targetBuilder.add(VPackValue(_targetVertexId));
      end = targetBuilder.slice();
    }
  } else {
    AqlValue const& in = items->getValueReference(_pos, _targetReg);
    if (in.isObject()) {
      try {
        targetBuilder.add(VPackValue(_trx->extractIdString(in.slice())));
        end = targetBuilder.slice();
      }
      catch (...) {
        // _id or _key not present... ignore this error and fall through
        // returning no path
        return false;
      }
    } else if (in.isString()) {
      end = in.slice();
    } else {
      _engine->getQuery()->registerWarning(
          TRI_ERROR_BAD_PARAMETER, "Invalid input for Shortest Path: Only "
                                       "id strings or objects with _id are "
                                       "allowed");
      return false;
    }
  }

  TRI_ASSERT(_finder != nullptr);
  // We do not need this data anymore. Result has been processed.
  // Save some memory.
  _coordinatorCache.clear();
  bool hasPath = _finder->shortestPath(start, end, *_path, [this]() { throwIfKilled(); });

  if (hasPath) {
    _posInPath = 0;
    _pathLength = _path->length();
  }

  return hasPath;
}

AqlItemBlock* ShortestPathBlock::getSome(size_t, size_t atMost) {
  DEBUG_BEGIN_BLOCK();
  traceGetSomeBegin();
  if (_done) {
    traceGetSomeEnd(nullptr);
    return nullptr;
  }

  if (_buffer.empty()) {
    size_t toFetch = (std::min)(DefaultBatchSize(), atMost);
    if (!ExecutionBlock::getBlock(toFetch, toFetch)) {
      _done = true;
      traceGetSomeEnd(nullptr);
      return nullptr;
    }
    _pos = 0;  // this is in the first block
  }

  // If we get here, we do have _buffer.front()
  AqlItemBlock* cur = _buffer.front();
  size_t const curRegs = cur->getNrRegs();

  // Collect the next path:
  if (_posInPath >= _pathLength) {
    if (!nextPath(cur)) {
      // This input does not have any path. maybe the next one has.
      // we can only return nullptr iff the buffer is empty.
      if (++_pos >= cur->size()) {
        _buffer.pop_front();  // does not throw
        delete cur;
        _pos = 0;
      }
      auto r = getSome(atMost, atMost);
      traceGetSomeEnd(r);
      return r;
    }
  }

  size_t available = _pathLength - _posInPath;
  size_t toSend = (std::min)(atMost, available);

  RegisterId nrRegs =
      getPlanNode()->getRegisterPlan()->nrRegs[getPlanNode()->getDepth()];
  std::unique_ptr<AqlItemBlock> res(requestBlock(toSend, nrRegs));
  // automatically freed if we throw
  TRI_ASSERT(curRegs <= res->getNrRegs());

  // only copy 1st row of registers inherited from previous frame(s)
  inheritRegisters(cur, res.get(), _pos);

  // TODO: lease builder?
  VPackBuilder resultBuilder;
  for (size_t j = 0; j < toSend; j++) {
    if (usesVertexOutput()) {
      resultBuilder.clear();
      _path->vertexToVelocyPack(_trx, _mmdr.get(), _posInPath, resultBuilder);
      res->setValue(j, _vertexReg, AqlValue(resultBuilder.slice()));
    }
    if (usesEdgeOutput()) {
      resultBuilder.clear();
      _path->edgeToVelocyPack(_trx, _mmdr.get(), _posInPath, resultBuilder);
      res->setValue(j, _edgeReg, AqlValue(resultBuilder.slice()));
    }
    if (j > 0) {
      // re-use already copied aqlvalues
      res->copyValuesFromFirstRow(j, static_cast<RegisterId>(curRegs));
    }
    ++_posInPath;
  }

  if (_posInPath >= _pathLength) {
    // Advance read position for next call
    if (++_pos >= cur->size()) {
      _buffer.pop_front();  // does not throw
      delete cur;
      _pos = 0;
    }
  }

  // Clear out registers no longer needed later:
  clearRegisters(res.get());
  traceGetSomeEnd(res.get());
  return res.release();

  // cppcheck-suppress style
  DEBUG_END_BLOCK();
}

size_t ShortestPathBlock::skipSome(size_t, size_t atMost) {
  return 0;
}

