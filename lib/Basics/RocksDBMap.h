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

#ifndef ARANGODB_BASICS_ROCKSDB_MAP_H
#define ARANGODB_BASICS_ROCKSDB_MAP_H 1

#define ROCKSDB_MAP_TYPE_REVISIONS_CACHE 0

#include "Basics/Common.h"

#include <velocypack/Builder.h>
#include <velocypack/velocypack-aliases.h>
#include <cassert>
#include <iostream>
#include <thread>

#include "Basics/MutexLocker.h"
#include "Basics/gcd.h"
#include "Basics/memory-map.h"
#include "Basics/prime-numbers.h"
#include "Logger/Logger.h"
#include "Random/RandomGenerator.h"
#include "VocBase/voc-types.h"

#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/slice_transform.h>
#include <rocksdb/table.h>

#include <string>

static arangodb::Mutex _rocksDbMutex;
static rocksdb::DB* _db;        // single global instance
static uint64_t _mapCount = 0;  // number of active maps

namespace arangodb {
namespace basics {

struct RocksDBPosition {
  size_t bucketId;
  uint64_t position;

  RocksDBPosition() : bucketId(SIZE_MAX), position(0) {}

  void reset() {
    bucketId = SIZE_MAX - 1;
    position = 0;
  }

  bool operator==(RocksDBPosition const& other) const {
    return position == other.position && bucketId == other.bucketId;
  }
};

////////////////////////////////////////////////////////////////////////////////
/// @brief RocksDB-backed map implementation
////////////////////////////////////////////////////////////////////////////////

template <class Key, class Element>
class RocksDBMap {
 private:
  typedef void UserData;

 public:
  typedef std::function<Key const(UserData*, Element const&)>
      ExtractKeyFuncType;
  typedef std::function<bool(UserData*, Key const*, uint64_t hash,
                             Element const&)>
      IsEqualKeyElementFuncType;
  typedef std::function<bool(UserData*, Element const&, Element const&)>
      IsEqualElementElementFuncType;

  typedef std::function<bool(Element&)> CallbackElementFuncType;

 private:
  ExtractKeyFuncType const _extractKey;
  IsEqualKeyElementFuncType const _isEqualKeyElement;
  IsEqualElementElementFuncType const _isEqualElementElement;
  IsEqualElementElementFuncType const _isEqualElementElementByKey;

  std::string _mapPrefix;
  std::string _dbFolder;
  std::function<std::string()> _contextCallback;
  std::function<std::string(Key const&)> _keyToString;
  std::function<std::string(Element const&)> _elementToString;

  uint64_t _size;

 public:
  RocksDBMap(ExtractKeyFuncType extractKey,
             IsEqualKeyElementFuncType isEqualKeyElement,
             IsEqualElementElementFuncType isEqualElementElement,
             IsEqualElementElementFuncType isEqualElementElementByKey,
             std::string mapPrefix,
             std::function<std::string()> contextCallback =
                 []() -> std::string { return ""; },
             std::function<std::string(Key const&)> keyToString =
                 [](Key const&) -> std::string { return ""; },
             std::function<std::string(Element const&)> elementToString =
                 [](Element const&) -> std::string { return ""; })
      : _extractKey(extractKey),
        _isEqualKeyElement(isEqualKeyElement),
        _isEqualElementElement(isEqualElementElement),
        _isEqualElementElementByKey(isEqualElementElementByKey),
        _mapPrefix(mapPrefix),
        _dbFolder("/tmp/test_rocksdbmap"),
        _contextCallback(contextCallback),
        _keyToString(keyToString),
        _elementToString(elementToString),
        _size(0) {
    MUTEX_LOCKER(locker, _rocksDbMutex);
    if (_db == nullptr) {
      rocksdb::BlockBasedTableOptions table_options;
      table_options.block_cache =
          rocksdb::NewLRUCache(100 * 1048576);  // 100MB uncompressed cache

      rocksdb::Options options;
      options.table_factory.reset(
          rocksdb::NewBlockBasedTableFactory(table_options));
      options.create_if_missing = true;
      options.prefix_extractor.reset(
          rocksdb::NewFixedPrefixTransform(_mapPrefix.length()));

      auto status = rocksdb::DB::Open(options, _dbFolder, &_db);
      TRI_ASSERT(status.ok());
      assert(status.ok());
    }
    _mapCount++;
  }

  ~RocksDBMap() {
    truncate([](Element&) -> bool { return true; });
    MUTEX_LOCKER(locker, _rocksDbMutex);
    _mapCount--;
    if (_mapCount == 0) {
      delete _db;
      _db = nullptr;
    }
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief adhere to the rule of five
  //////////////////////////////////////////////////////////////////////////////

  RocksDBMap(RocksDBMap const&) = delete;             // copy constructor
  RocksDBMap(RocksDBMap&&) = delete;                  // move constructor
  RocksDBMap& operator=(RocksDBMap const&) = delete;  // op =
  RocksDBMap& operator=(RocksDBMap&&) = delete;       // op =

  static std::string buildPrefix(uint8_t type, TRI_voc_cid_t collectionId) {
    std::string value;
    value.append(reinterpret_cast<char const*>(&type), sizeof(uint8_t));
    value.append(reinterpret_cast<char const*>(&collectionId),
                 sizeof(TRI_voc_cid_t));
    return value;
  }

 private:
  std::string prefixKey(Key const* k) const {
    std::string buf(_mapPrefix.data(), _mapPrefix.size());
    buf.append(reinterpret_cast<char const*>(k), sizeof(Key));
    return buf;
  }

  rocksdb::Slice wrapElement(Element const* e) const {
    return rocksdb::Slice(reinterpret_cast<char const*>(e), sizeof(Element));
  }

  Element unwrapElement(std::string const* eSlice) const {
    return Element(*(reinterpret_cast<Element const*>(eSlice->data())));
  }

 public:
  void truncate(CallbackElementFuncType callback) {
    auto it = _db->NewIterator(rocksdb::ReadOptions());
    for (it->Seek(_mapPrefix); it->Valid() && it->key().starts_with(_mapPrefix);
         it->Next()) {
      // TODO: invoke callback on all elements, then delete them
      Element e(*reinterpret_cast<Element const*>(it->value().data()));
      callback(e);
      auto status = _db->Delete(rocksdb::WriteOptions(), it->key());
      TRI_ASSERT(status.ok());
      _size--;
    }
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
    std::string prefixedKey = prefixKey(key);
    rocksdb::Slice kSlice(prefixedKey);
    std::string eSlice(sizeof(Element), '\0');
    auto status = _db->Get(rocksdb::ReadOptions(), kSlice, &eSlice);
    if (status.ok()) {
      return unwrapElement(&eSlice);
    } else {
      Element noE;
      return noE;
    }
  }

  Element* findByKeyRef(UserData* userData, Key const* key) const {
    // TODO: do this to support primary index
    return reinterpret_cast<Element*>(nullptr);
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

    std::string prefixedKey = prefixKey(&key);
    rocksdb::Slice kSlice(prefixedKey);
    auto eSlice = wrapElement(&element);
    auto status = _db->Put(rocksdb::WriteOptions(), kSlice, eSlice);
    if (status.ok()) {
      _size++;
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
    // TODO: do this for indexes
    return TRI_ERROR_NO_ERROR;
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief removes an element from the array based on its key,
  /// returns nullptr if the element
  /// was not found and the old value, if it was successfully removed
  //////////////////////////////////////////////////////////////////////////////

  Element removeByKey(UserData* userData, Key const* key) {
    std::string prefixedKey = prefixKey(key);
    rocksdb::Slice kSlice(prefixedKey);
    std::string eSlice(sizeof(Element), '\0');
    auto status = _db->Get(rocksdb::ReadOptions(), kSlice, &eSlice);
    if (status.ok()) {
      status = _db->Delete(rocksdb::WriteOptions(), kSlice);
      if (status.ok()) {
        _size--;
      } else {
        eSlice.replace(0, eSlice.length(), 1, '\0');
      }
    }
    return unwrapElement(&eSlice);
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
    // TODO: do this
  }

  /// @brief a method to iterate over all elements in a bucket. this method
  /// can NOT be used for deleting elements
  bool invokeOnAllElements(CallbackElementFuncType const& callback,
                           Element& e) {
    // TODO: do this
    return true;
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief a method to iterate over all elements in the hash. this method
  /// can be used for deleting elements as well
  //////////////////////////////////////////////////////////////////////////////

  void invokeOnAllElementsForRemoval(CallbackElementFuncType callback) {
    // TODO: do this
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief a method to iterate over all elements in the index in
  ///        a sequential order.
  ///        Returns nullptr if all documents have been returned.
  ///        Convention: position.bucketId == SIZE_MAX indicates a new start.
  ///        Convention: position.bucketId == SIZE_MAX - 1 indicates a restart.
  ///        During a continue the total will not be modified.
  //////////////////////////////////////////////////////////////////////////////

  Element findSequential(UserData* userData, RocksDBPosition& position,
                         uint64_t& total) const {
    // TODO: do this for indexes
    Element e;
    return e;
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief a method to iterate over all elements in the index in
  ///        reversed sequential order.
  ///        Returns nullptr if all documents have been returned.
  ///        Convention: position === UINT64_MAX indicates a new start.
  //////////////////////////////////////////////////////////////////////////////

  Element findSequentialReverse(UserData* userData,
                                RocksDBPosition& position) const {
    // TODO: do this for indexes
    Element e;
    return e;
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief a method to iterate over all elements in the index in
  ///        a random order.
  ///        Returns nullptr if all documents have been returned.
  ///        Convention: *step === 0 indicates a new start.
  //////////////////////////////////////////////////////////////////////////////

  Element findRandom(UserData* userData, RocksDBPosition& initialPosition,
                     RocksDBPosition& position, uint64_t& step,
                     uint64_t& total) const {
    // TODO: do this for indexes
    Element e;
    return e;
  }
};
}  // namespace basics
}  // namespace arangodb

#endif
