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

#ifndef ARANGODB_BASICS_ROCKSDB_INSTANCE_H
#define ARANGODB_BASICS_ROCKSDB_INSTANCE_H 1

#include <iostream>

#include "Basics/Common.h"

#include "Basics/MutexLocker.h"
#include "VocBase/voc-types.h"

#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/slice_transform.h>
#include <rocksdb/table.h>

#include <string>

namespace arangodb {
namespace basics {

class RocksDBInstance {
 private:
  static arangodb::Mutex _rocksDbMutex;
  static rocksdb::DB* _db;                      // single global instance
  static std::atomic<uint64_t> _instanceCount;  // number of active maps
  static std::string _dbFolder;
  static size_t const _prefixLength = sizeof(uint8_t) + sizeof(TRI_voc_cid_t);

 public:
  RocksDBInstance();
  ~RocksDBInstance();

  rocksdb::DB* db();
};
}  // namespace basics
}  // namespace arangodb

#endif
