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
/// @author Dr. Frank Celler
/// @author Martin Schoenert
/// @author Michael Hackstein
/// @author Dan Larkin
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_BASICS_BTREE_MAP_H
#define ARANGODB_BASICS_BTREE_MAP_H 1

#include "Basics/Common.h"

#include <velocypack/Builder.h>
#include <velocypack/velocypack-aliases.h>
#include <cassert>
#include <iostream>
#include <thread>

#include "Basics/MutexLocker.h"
#include "Basics/RocksDBInstance.h"
#include "Basics/gcd.h"
#include "Basics/memory-map.h"
#include "Basics/prime-numbers.h"
#include "Logger/Logger.h"
#include "Random/RandomGenerator.h"
#include "VocBase/voc-types.h"

#include <safe_btree_map.h>

#include <string>

namespace arangodb {
namespace basics {

template <class Key, class Element>
struct BTreePosition {
 private:
  typedef typename btree::safe_btree_map<Key, Element>::const_iterator
      BTreeIterator;

 public:
  size_t bucketId;
  BTreeIterator* it;

  BTreePosition() : bucketId(SIZE_MAX), it(nullptr) {}
  ~BTreePosition() {
    if (it != nullptr) {
      delete it;
    }
  }

  void reset() { bucketId = SIZE_MAX - 1; }
  void transfer(BTreeIterator const& other) {
    if (it != nullptr) {
      delete it;
    }
    it = new BTreeIterator(other);
  }
  bool operator==(BTreePosition const& other) const {
    return it == other.it && bucketId == other.bucketId;
  }
};

template <class Key, class Element>
struct BTreeRevPosition {
 private:
  typedef typename btree::safe_btree_map<Key, Element>::const_reverse_iterator
      BTreeRevIterator;

 public:
  size_t bucketId;
  BTreeRevIterator* it;

  BTreeRevPosition() : bucketId(SIZE_MAX), it(nullptr) {}
  ~BTreeRevPosition() {
    if (it != nullptr) {
      delete it;
    }
  }

  void reset() { bucketId = SIZE_MAX - 1; }
  void transfer(BTreeRevIterator const& other) {
    if (it != nullptr) {
      delete it;
    }
    it = new BTreeRevIterator(other);
  }

  bool operator==(BTreeRevPosition const& other) const {
    return it == other.it && bucketId == other.bucketId;
  }
};

////////////////////////////////////////////////////////////////////////////////
/// @brief BTree-backed map implementation
////////////////////////////////////////////////////////////////////////////////

template <class Key, class Element>
class BTreeMap {
 private:
  typedef void UserData;
  typedef btree::safe_btree_map<Key, Element> BTree;

 public:
  typedef std::function<Key const(UserData*, Element const&)>
      ExtractKeyFuncType;
  typedef std::function<bool(UserData*, Key const*, uint64_t hash,
                             Element const&)>
      IsEqualKeyElementFuncType;
  typedef std::function<bool(UserData*, Element const&, Element const&)>
      IsEqualElementElementFuncType;
  typedef std::function<std::string()> ContextCallbackFuncType;
  typedef std::function<std::string(void*, Key const&)> KeyToStringFuncType;
  typedef std::function<std::string(void*, Element const&)>
      ElementToStringFuncType;

  typedef std::function<bool(Element&)> CallbackElementFuncType;

 private:
  ExtractKeyFuncType const _extractKey;
  IsEqualKeyElementFuncType const _isEqualKeyElement;
  IsEqualElementElementFuncType const _isEqualElementElement;
  IsEqualElementElementFuncType const _isEqualElementElementByKey;

  ContextCallbackFuncType const _contextCallback;
  KeyToStringFuncType const _keyToString;
  ElementToStringFuncType const _elementToString;

  BTree _tree;
  uint64_t _size;

 public:
  BTreeMap(ExtractKeyFuncType extractKey,
           IsEqualKeyElementFuncType isEqualKeyElement,
           IsEqualElementElementFuncType isEqualElementElement,
           IsEqualElementElementFuncType isEqualElementElementByKey,
           ContextCallbackFuncType contextCallback = []() -> std::string {
             return "";
           },
           KeyToStringFuncType keyToString =
               [](void*, Key const&) -> std::string { return ""; },
           ElementToStringFuncType elementToString =
               [](void*, Element const&) -> std::string { return ""; })
      : _extractKey(extractKey),
        _isEqualKeyElement(isEqualKeyElement),
        _isEqualElementElement(isEqualElementElement),
        _isEqualElementElementByKey(isEqualElementElementByKey),
        _contextCallback(contextCallback),
        _keyToString(keyToString),
        _elementToString(elementToString),
        _tree(),
        _size(0) {}

  ~BTreeMap() {
    truncate([](Element&) -> bool { return true; });
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief adhere to the rule of five
  //////////////////////////////////////////////////////////////////////////////

  BTreeMap(BTreeMap const&) = delete;             // copy constructor
  BTreeMap(BTreeMap&&) = delete;                  // move constructor
  BTreeMap& operator=(BTreeMap const&) = delete;  // op =
  BTreeMap& operator=(BTreeMap&&) = delete;       // op =

 private:
  std::pair<Key, Element> wrapPair(Key const& k, Element const& e) const {
    return std::pair<Key, Element>(k, e);
  }

 public:
  void truncate(CallbackElementFuncType callback) {
    invokeOnAllElements(callback);
    _tree.clear();
    _size = 0;
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief checks if this index is empty
  //////////////////////////////////////////////////////////////////////////////

  bool isEmpty() const { return (size == 0); }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief get the hash array's memory usage
  //////////////////////////////////////////////////////////////////////////////

  size_t memoryUsage() const {
    return 0;  // TODO
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief get the number of elements in the hash
  //////////////////////////////////////////////////////////////////////////////

  size_t size() const { return _size; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief resizes the hash table
  //////////////////////////////////////////////////////////////////////////////

  int resize(UserData* userData, size_t size) { return TRI_ERROR_NO_ERROR; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief Appends information about statistics in the given VPackBuilder
  //////////////////////////////////////////////////////////////////////////////

  void appendToVelocyPack(VPackBuilder& builder) {
    // TODO: come up with something
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief finds an element equal to the given element.
  //////////////////////////////////////////////////////////////////////////////

  Element find(UserData* userData, Element const& element) const {
    Key k = _extractKey(userData, element);
    Element found = findByKey(userData, &k);
    return _isEqualElementElement(userData, element, found) ? found : Element();
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief finds an element given a key, returns a default-constructed Element
  /// if not found
  //////////////////////////////////////////////////////////////////////////////

  Element findByKey(UserData* userData, Key const* key) const {
    auto result = _tree.find(*key);
    if (result != _tree.end()) {
      Element e = (*result).second;
      return e;
    } else {
      return Element();
    }
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief adds an element to the array
  //////////////////////////////////////////////////////////////////////////////

  int insert(UserData* userData, Element const& element) {
    Key key = _extractKey(userData, element);

    Element found = findByKey(userData, &key);
    if (_isEqualElementElementByKey(userData, element, found)) {
      return TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED;
    }

    auto pair = wrapPair(key, element);
    auto result = _tree.insert(pair);
    if (result.second) {
      _size++;
      return TRI_ERROR_NO_ERROR;
    } else {
      return TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED;  // wrong error
    }
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief adds an element to the array
  //////////////////////////////////////////////////////////////////////////////

  int update(UserData* userData, Element const& element) {
    Key key = _extractKey(userData, element);

    auto removed = _tree.erase(key);
    if (removed <= 0) {
      return TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED;  // wrong error
    }

    auto pair = wrapPair(key, element);
    auto inserted = _tree.insert(pair);
    if (inserted.second) {
      return TRI_ERROR_NO_ERROR;
    } else {
      return TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED;  // wrong error
    }
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief adds multiple elements to the array
  //////////////////////////////////////////////////////////////////////////////

  int batchInsert(std::function<void*()> const& contextCreator,
                  std::function<void(void*)> const& contextDestroyer,
                  std::vector<Element> const* data, size_t numThreads) {
    if (data->empty()) {
      // nothing to do
      return TRI_ERROR_NO_ERROR;
    }

    std::atomic<int> res(TRI_ERROR_NO_ERROR);
    std::vector<Element> const& elements = *(data);

    auto userData = contextCreator();

    for (auto i = 0; i < elements.length(); i++) {
      auto status = insert(userData, elements[i]);
      if (status != TRI_ERROR_NO_ERROR) {
        res = status;
        break;
      }
    }

    contextDestroyer(userData);

    if (res.load() != TRI_ERROR_NO_ERROR) {
      // Rollback such that the data can be deleted outside
      void* userData = contextCreator();
      try {
        for (auto const& d : *data) {
          remove(userData, d);
        }
      } catch (...) {
      }
      contextDestroyer(userData);
    }
    return res.load();
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief removes an element from the array based on its key,
  /// returns nullptr if the element
  /// was not found and the old value, if it was successfully removed
  //////////////////////////////////////////////////////////////////////////////

  Element removeByKey(UserData* userData, Key const* key) {
    auto result = _tree.find(*key);
    if (result != _tree.end()) {
      Element e = (*result).second;
      auto status = _tree.erase(*key);
      if (status > 0) {
        _size--;
        return e;
      } else {
        return Element();
      }
    } else {
      return Element();
    }
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief removes an element from the array, returns nullptr if the element
  /// was not found and the old value, if it was successfully removed
  //////////////////////////////////////////////////////////////////////////////

  Element remove(UserData* userData, Element const& element) {
    Key k = _extractKey(userData, element);
    Element found = findByKey(userData, &k);
    return _isEqualElementElement(userData, element, found)
               ? removeByKey(userData, &k)
               : Element();
  }

  /// @brief a method to iterate over all elements in the hash. this method
  /// can NOT be used for deleting elements
  void invokeOnAllElements(CallbackElementFuncType const& callback) {
    for (auto it = _tree.begin(); it != _tree.end(); it++) {
      Element e = (*it).second;
      if (!callback(e)) {
        return;
      }
    }
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief a method to iterate over all elements in the hash. this method
  /// can be used for deleting elements as well
  //////////////////////////////////////////////////////////////////////////////

  void invokeOnAllElementsForRemoval(CallbackElementFuncType callback) {
    auto it = _tree.begin();
    while (it != _tree.end()) {
      Element e = (*it).second;
      Key lastKey = (*it).first;
      auto oldSize = _size;
      if (!callback(e)) {
        return;
      }
      if (_size == 0) {
        return;
      }
      // find next element
      auto next = _tree.lower_bound(lastKey);
      if (_size == oldSize && next != _tree.end()) {
        next++;
      }
      it = next;
    }
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief a method to iterate over all elements in the index in
  ///        a sequential order.
  ///        Returns nullptr if all documents have been returned.
  ///        Convention: position.bucketId == SIZE_MAX indicates a new start.
  ///        Convention: position.bucketId == SIZE_MAX - 1 indicates a
  ///        restart.
  ///        During a continue the total will not be modified.
  //////////////////////////////////////////////////////////////////////////////

  Element findSequential(UserData* userData,
                         BTreePosition<Key, Element>& position,
                         uint64_t& total) const {
    if (position.bucketId == SIZE_MAX || position.bucketId == SIZE_MAX - 1) {
      position.transfer(_tree.begin());
      total = _size;
      position.bucketId = 0;
    } else {
      if (position.it == nullptr || *(position.it) == _tree.end()) {
        return Element();
      }
      (*(position.it))++;
    }

    if (*(position.it) != _tree.end()) {
      return (**(position.it)).second;
    } else {
      return Element();
    }
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief a method to iterate over all elements in the index in
  ///        reversed sequential order.
  ///        Returns nullptr if all documents have been returned.
  ///        Convention: position === UINT64_MAX indicates a new start.
  //////////////////////////////////////////////////////////////////////////////

  Element findSequentialReverse(
      UserData* userData, BTreeRevPosition<Key, Element>& position) const {
    if (position.bucketId == SIZE_MAX || position.bucketId == SIZE_MAX - 1) {
      position.transfer(_tree.rbegin());
      position.bucketId = 0;
    } else {
      if (position.it == nullptr || *(position.it) == _tree.rend()) {
        return Element();
      }
      (*(position.it))++;
    }

    if (*(position.it) != _tree.rend()) {
      return (**(position.it)).second;
    } else {
      return Element();
    }
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief a method to iterate over all elements in the index in
  ///        a random order.
  ///        Returns nullptr if all documents have been returned.
  ///        Convention: *step === 0 indicates a new start.
  //////////////////////////////////////////////////////////////////////////////

  Element findRandom(UserData* userData,
                     BTreePosition<Key, Element>& initialPosition,
                     BTreePosition<Key, Element>& position, uint64_t& step,
                     uint64_t& total) const {
    if (position.bucketId == SIZE_MAX || position.bucketId == SIZE_MAX - 1) {
      position.transfer(_tree.begin());
      total = _size;
      position.bucketId = 0;
    } else {
      if (position.it == nullptr || *(position.it) == _tree.end()) {
        return Element();
      }
      (*(position.it))++;
    }

    if (*(position.it) != _tree.end()) {
      return (**(position.it)).second;
    } else {
      return Element();
    }
  }
};
}  // namespace basics
}  // namespace arangodb

#endif
