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

#include "Basics/RocksDBInstance.h"
#include "Basics/Common.h"

namespace arangodb {
namespace basics {
arangodb::Mutex RocksDBInstance::_rocksDbMutex;
rocksdb::DB* RocksDBInstance::_db = nullptr;
std::atomic<uint64_t> RocksDBInstance::_instanceCount(0);
std::string RocksDBInstance::_dbFolder("/tmp/test_rocksdbinstance");

RocksDBInstance::RocksDBInstance() {
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
        rocksdb::NewFixedPrefixTransform(_prefixLength));

    auto status = rocksdb::DB::Open(options, _dbFolder, &_db);
    TRI_ASSERT(status.ok());
    if (!status.ok()) {
      std::cerr << status.ToString() << std::endl;
    }
    assert(status.ok());
  }
  _instanceCount++;
}

RocksDBInstance::~RocksDBInstance() {
  MUTEX_LOCKER(locker, _rocksDbMutex);
  _instanceCount--;
  if (_instanceCount.load() == 0) {
    delete _db;
    _db = nullptr;
  }
}

rocksdb::DB* RocksDBInstance::db() { return _db; }
}
}
