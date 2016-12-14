////////////////////////////////////////////////////////////////////////////////
/// @brief test suite for RocksDBMap
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2012 triagens GmbH, Cologne, Germany
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
/// Copyright holder is triAGENS GmbH, Cologne, Germany
///
/// @author Dan Larkin
/// @author Copyright 2014, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "Basics/Common.h"

#define BOOST_TEST_INCLUDED
#include <boost/test/unit_test.hpp>

#include "Basics/BTreeMap.h"
#include "Basics/conversions.h"
#include "Basics/tri-strings.h"

using namespace std;

// -----------------------------------------------------------------------------
// --SECTION--                                                    private macros
// -----------------------------------------------------------------------------

#define INIT_MAP(name)                                                \
  arangodb::basics::BTreeMap<Key, Element> name(                      \
      ExtractKey, IsEqualKeyElement, IsEqualElementElement,           \
      IsEqualElementElementByKey, []() -> std::string { return ""; }, \
      KeyToString, ElementToString)

#define DESTROY_MAP(name)

#define ELEMENT(name, k, v) Element name(k, v)
#define KEY(name, k) Key name(k)

#define POSITION(name) arangodb::basics::BTreePosition<Key, Element> name
#define REV_POSITION(name) arangodb::basics::BTreeRevPosition<Key, Element> name

struct Key {
  uint64_t k;
  Key() : k(0) {}
  Key(uint64_t i) : k(i) {}
  Key(Key const& other) : k(other.k) {}
  bool empty() const { return k == 0; }
};

struct Value {
  uint64_t v;
  Value() : v(0) {}
  Value(uint64_t i) : v(i) {}
  Value(Value const& other) : v(other.v) {}
  bool empty() const { return v == 0; }
};

struct Element {
  Key k;
  Value v;
  Element() : k(0), v(0) {}
  Element(uint64_t key, uint64_t value) : k(key), v(value) {}
  Element(Key const& otherK, Value const& otherV) : k(otherK), v(otherV) {}
  Element(Element const& other) : k(other.k), v(other.v) {}
  bool empty() const { return k.empty() || v.empty(); }
  Value value() { return v; }
  bool operator==(Element const other) const {
    return ((k.k == other.k.k) && (v.v == other.v.v));
  }
};

static inline std::ostream& operator<<(std::ostream& os, Element const& e) {
  os << "(" << e.k.k << ", " << e.v.v << ")";
  return os;
}

namespace boost {
namespace test_tools {
namespace tt_detail {
template <>
struct print_log_value<Element> {
  void operator()(std::ostream& os, Element const& ts) { ::operator<<(os, ts); }
};
}
}
}

namespace std {
template <>
struct less<Key> {
  bool operator()(Key const& a, Key const& b) const { return a.k < b.k; }
};
}

static Key const ExtractKey(void* userData, Element const& e) {
  return Key(e.k);
}

static bool IsEqualKeyElement(void* userData, Key const* k, uint64_t hash,
                              Element const& e) {
  return k->k == e.k.k;
}

static bool IsEqualElementElement(void* userData, Element const& l,
                                  Element const& r) {
  return ((l.k.k == r.k.k) && (l.v.v == r.v.v));
}

static bool IsEqualElementElementByKey(void* userData, Element const& l,
                                       Element const& r) {
  return (l.k.k == r.k.k);
}

static bool CallbackElement(Element const& e) { return true; }

static std::string KeyToString(void*, Key const& k) {
  return std::to_string(k.k);
}

static std::string ElementToString(void*, Element const& e) {
  return std::string("(")
      .append(std::to_string(e.k.k))
      .append(", ")
      .append(std::to_string(e.v.v))
      .append(")");
}

// -----------------------------------------------------------------------------
// --SECTION--                                                 private constants
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                 setup / tear-down
// -----------------------------------------------------------------------------

struct BTreeMapSetup {
  BTreeMapSetup() { BOOST_TEST_MESSAGE("setup BTreeMap"); }

  ~BTreeMapSetup() { BOOST_TEST_MESSAGE("tear-down BTreeMap"); }
};

// -----------------------------------------------------------------------------
// --SECTION--                                                        test suite
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief setup
////////////////////////////////////////////////////////////////////////////////

BOOST_FIXTURE_TEST_SUITE(BTreeMapTest, BTreeMapSetup)

////////////////////////////////////////////////////////////////////////////////
/// @brief test initialization
////////////////////////////////////////////////////////////////////////////////

BOOST_AUTO_TEST_CASE(tst_init_one) {
  INIT_MAP(m1);

  BOOST_CHECK_EQUAL((uint64_t)0, m1.size());

  DESTROY_MAP(m1);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief test initialization of multiple maps
////////////////////////////////////////////////////////////////////////////////

BOOST_AUTO_TEST_CASE(tst_init_two) {
  INIT_MAP(m1);
  INIT_MAP(m2);

  BOOST_CHECK_EQUAL((uint64_t)0, m1.size());
  BOOST_CHECK_EQUAL((uint64_t)0, m2.size());

  DESTROY_MAP(m1);
  DESTROY_MAP(m2);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief test unique insertion
////////////////////////////////////////////////////////////////////////////////

BOOST_AUTO_TEST_CASE(tst_insert_few) {
  INIT_MAP(m1);

  ELEMENT(empty, 0, 0);
  ELEMENT(e1, 1, 123);
  KEY(k1, 1);
  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m1.insert(nullptr, e1));
  BOOST_CHECK_EQUAL((uint64_t)1, m1.size());
  BOOST_CHECK_EQUAL(e1, m1.find(nullptr, e1));
  BOOST_CHECK_EQUAL(e1, m1.findByKey(nullptr, &k1));

  BOOST_CHECK_EQUAL(e1, m1.remove(nullptr, e1));
  BOOST_CHECK_EQUAL((uint64_t)0, m1.size());
  BOOST_CHECK_EQUAL(empty, m1.find(nullptr, e1));

  DESTROY_MAP(m1);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief test unique insertion
////////////////////////////////////////////////////////////////////////////////

BOOST_AUTO_TEST_CASE(tst_insert_unique_violation) {
  INIT_MAP(m1);

  ELEMENT(empty, 0, 0);
  ELEMENT(e1, 2, 123);
  ELEMENT(e2, 2, 123);
  ELEMENT(e3, 2, 456);

  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m1.insert(nullptr, e1));
  BOOST_CHECK_EQUAL((uint64_t)1, m1.size());
  BOOST_CHECK_EQUAL(e1, m1.find(nullptr, e1));

  BOOST_CHECK_EQUAL(TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED,
                    m1.insert(nullptr, e2));
  BOOST_CHECK_EQUAL((uint64_t)1, m1.size());

  BOOST_CHECK_EQUAL(TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED,
                    m1.insert(nullptr, e3));
  BOOST_CHECK_EQUAL((uint64_t)1, m1.size());
  BOOST_CHECK_EQUAL(empty, m1.find(nullptr, e3));

  BOOST_CHECK_EQUAL(e1, m1.remove(nullptr, e1));
  BOOST_CHECK_EQUAL((uint64_t)0, m1.size());
  BOOST_CHECK_EQUAL(empty, m1.find(nullptr, e1));

  DESTROY_MAP(m1);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief test unique reinsertion
////////////////////////////////////////////////////////////////////////////////

BOOST_AUTO_TEST_CASE(tst_reinsert) {
  INIT_MAP(m1);

  ELEMENT(empty, 0, 0);
  ELEMENT(e1, 1, 123);
  KEY(k1, 1);
  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m1.insert(nullptr, e1));
  BOOST_CHECK_EQUAL((uint64_t)1, m1.size());
  BOOST_CHECK_EQUAL(e1, m1.find(nullptr, e1));
  BOOST_CHECK_EQUAL(e1, m1.findByKey(nullptr, &k1));

  BOOST_CHECK_EQUAL(e1, m1.remove(nullptr, e1));
  BOOST_CHECK_EQUAL((uint64_t)0, m1.size());
  BOOST_CHECK_EQUAL(empty, m1.find(nullptr, e1));

  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m1.insert(nullptr, e1));
  BOOST_CHECK_EQUAL((uint64_t)1, m1.size());
  BOOST_CHECK_EQUAL(e1, m1.find(nullptr, e1));
  BOOST_CHECK_EQUAL(e1, m1.findByKey(nullptr, &k1));

  m1.truncate(CallbackElement);
  BOOST_CHECK_EQUAL((uint64_t)0, m1.size());

  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m1.insert(nullptr, e1));
  BOOST_CHECK_EQUAL((uint64_t)1, m1.size());
  BOOST_CHECK_EQUAL(e1, m1.find(nullptr, e1));
  BOOST_CHECK_EQUAL(e1, m1.findByKey(nullptr, &k1));

  BOOST_CHECK_EQUAL(e1, m1.remove(nullptr, e1));
  BOOST_CHECK_EQUAL((uint64_t)0, m1.size());
  BOOST_CHECK_EQUAL(empty, m1.find(nullptr, e1));

  DESTROY_MAP(m1);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief test truncation with a single map
////////////////////////////////////////////////////////////////////////////////

BOOST_AUTO_TEST_CASE(tst_truncate_one) {
  INIT_MAP(m1);

  ELEMENT(empty, 0, 0);
  ELEMENT(e1, 1, 123);
  ELEMENT(e2, 2, 456);
  ELEMENT(e3, 3, 789);

  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m1.insert(nullptr, e1));
  BOOST_CHECK_EQUAL((uint64_t)1, m1.size());
  BOOST_CHECK_EQUAL(e1, m1.find(nullptr, e1));

  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m1.insert(nullptr, e2));
  BOOST_CHECK_EQUAL((uint64_t)2, m1.size());
  BOOST_CHECK_EQUAL(e2, m1.find(nullptr, e2));

  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m1.insert(nullptr, e3));
  BOOST_CHECK_EQUAL((uint64_t)3, m1.size());
  BOOST_CHECK_EQUAL(e3, m1.find(nullptr, e3));

  m1.truncate(CallbackElement);
  BOOST_CHECK_EQUAL((uint64_t)0, m1.size());
  BOOST_CHECK_EQUAL(empty, m1.find(nullptr, e1));
  BOOST_CHECK_EQUAL(empty, m1.find(nullptr, e2));
  BOOST_CHECK_EQUAL(empty, m1.find(nullptr, e3));

  DESTROY_MAP(m1);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief test truncation with a two maps
////////////////////////////////////////////////////////////////////////////////

BOOST_AUTO_TEST_CASE(tst_truncate_two) {
  INIT_MAP(m1);
  INIT_MAP(m2);

  ELEMENT(empty, 0, 0);
  ELEMENT(e1, 1, 123);
  ELEMENT(e2, 2, 456);
  ELEMENT(e3, 3, 789);

  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m1.insert(nullptr, e1));
  BOOST_CHECK_EQUAL((uint64_t)1, m1.size());
  BOOST_CHECK_EQUAL(e1, m1.find(nullptr, e1));

  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m1.insert(nullptr, e2));
  BOOST_CHECK_EQUAL((uint64_t)2, m1.size());
  BOOST_CHECK_EQUAL(e2, m1.find(nullptr, e2));

  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m1.insert(nullptr, e3));
  BOOST_CHECK_EQUAL((uint64_t)3, m1.size());
  BOOST_CHECK_EQUAL(e3, m1.find(nullptr, e3));

  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m2.insert(nullptr, e1));
  BOOST_CHECK_EQUAL((uint64_t)1, m2.size());
  BOOST_CHECK_EQUAL(e1, m2.find(nullptr, e1));

  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m2.insert(nullptr, e2));
  BOOST_CHECK_EQUAL((uint64_t)2, m2.size());
  BOOST_CHECK_EQUAL(e2, m2.find(nullptr, e2));

  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m2.insert(nullptr, e3));
  BOOST_CHECK_EQUAL((uint64_t)3, m2.size());
  BOOST_CHECK_EQUAL(e3, m2.find(nullptr, e3));

  m1.truncate(CallbackElement);
  BOOST_CHECK_EQUAL((uint64_t)0, m1.size());
  BOOST_CHECK_EQUAL(empty, m1.find(nullptr, e1));
  BOOST_CHECK_EQUAL(empty, m1.find(nullptr, e2));
  BOOST_CHECK_EQUAL(empty, m1.find(nullptr, e3));
  BOOST_CHECK_EQUAL((uint64_t)3, m2.size());
  BOOST_CHECK_EQUAL(e1, m2.find(nullptr, e1));
  BOOST_CHECK_EQUAL(e2, m2.find(nullptr, e2));
  BOOST_CHECK_EQUAL(e3, m2.find(nullptr, e3));
  m2.truncate(CallbackElement);
  BOOST_CHECK_EQUAL((uint64_t)0, m2.size());
  BOOST_CHECK_EQUAL(empty, m2.find(nullptr, e1));
  BOOST_CHECK_EQUAL(empty, m2.find(nullptr, e2));
  BOOST_CHECK_EQUAL(empty, m2.find(nullptr, e3));

  DESTROY_MAP(m1);
  DESTROY_MAP(m2);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief test forward iteration with a single map
////////////////////////////////////////////////////////////////////////////////

BOOST_AUTO_TEST_CASE(tst_iter_fwd_one) {
  INIT_MAP(m1);

  ELEMENT(empty, 0, 0);
  ELEMENT(e1, 1, 123);
  ELEMENT(e2, 2, 456);
  ELEMENT(e3, 3, 789);

  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m1.insert(nullptr, e1));
  BOOST_CHECK_EQUAL((uint64_t)1, m1.size());
  BOOST_CHECK_EQUAL(e1, m1.find(nullptr, e1));

  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m1.insert(nullptr, e2));
  BOOST_CHECK_EQUAL((uint64_t)2, m1.size());
  BOOST_CHECK_EQUAL(e2, m1.find(nullptr, e2));

  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m1.insert(nullptr, e3));
  BOOST_CHECK_EQUAL((uint64_t)3, m1.size());
  BOOST_CHECK_EQUAL(e3, m1.find(nullptr, e3));

  POSITION(p1);
  uint64_t total;
  BOOST_CHECK_EQUAL(e1, m1.findSequential(nullptr, p1, total));
  BOOST_CHECK_EQUAL(e2, m1.findSequential(nullptr, p1, total));
  BOOST_CHECK_EQUAL(e3, m1.findSequential(nullptr, p1, total));
  BOOST_CHECK_EQUAL(empty, m1.findSequential(nullptr, p1, total));
  BOOST_CHECK_EQUAL(empty, m1.findSequential(nullptr, p1, total));

  DESTROY_MAP(m1);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief test forward iteration with two maps
////////////////////////////////////////////////////////////////////////////////

BOOST_AUTO_TEST_CASE(tst_iter_fwd_two) {
  INIT_MAP(m1);
  INIT_MAP(m2);

  ELEMENT(empty, 0, 0);
  ELEMENT(e1, 1, 123);
  ELEMENT(e2, 2, 456);
  ELEMENT(e3, 3, 789);
  ELEMENT(e4, 4, 123);
  ELEMENT(e5, 5, 456);
  ELEMENT(e6, 6, 789);

  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m1.insert(nullptr, e1));
  BOOST_CHECK_EQUAL((uint64_t)1, m1.size());
  BOOST_CHECK_EQUAL(e1, m1.find(nullptr, e1));

  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m2.insert(nullptr, e2));
  BOOST_CHECK_EQUAL((uint64_t)1, m2.size());
  BOOST_CHECK_EQUAL(e2, m2.find(nullptr, e2));

  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m1.insert(nullptr, e3));
  BOOST_CHECK_EQUAL((uint64_t)2, m1.size());
  BOOST_CHECK_EQUAL(e3, m1.find(nullptr, e3));

  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m2.insert(nullptr, e4));
  BOOST_CHECK_EQUAL((uint64_t)2, m2.size());
  BOOST_CHECK_EQUAL(e4, m2.find(nullptr, e4));

  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m1.insert(nullptr, e5));
  BOOST_CHECK_EQUAL((uint64_t)3, m1.size());
  BOOST_CHECK_EQUAL(e5, m1.find(nullptr, e5));

  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m2.insert(nullptr, e6));
  BOOST_CHECK_EQUAL((uint64_t)3, m2.size());
  BOOST_CHECK_EQUAL(e6, m2.find(nullptr, e6));

  POSITION(p1);
  POSITION(p2);
  uint64_t total1;
  uint64_t total2;
  BOOST_CHECK_EQUAL(e1, m1.findSequential(nullptr, p1, total1));
  BOOST_CHECK_EQUAL(e2, m2.findSequential(nullptr, p2, total2));
  BOOST_CHECK_EQUAL(e3, m1.findSequential(nullptr, p1, total1));
  BOOST_CHECK_EQUAL(e4, m2.findSequential(nullptr, p2, total2));
  BOOST_CHECK_EQUAL(e5, m1.findSequential(nullptr, p1, total1));
  BOOST_CHECK_EQUAL(e6, m2.findSequential(nullptr, p2, total2));
  BOOST_CHECK_EQUAL(empty, m1.findSequential(nullptr, p1, total1));
  BOOST_CHECK_EQUAL(empty, m1.findSequential(nullptr, p1, total1));
  BOOST_CHECK_EQUAL(empty, m2.findSequential(nullptr, p2, total2));
  BOOST_CHECK_EQUAL(empty, m2.findSequential(nullptr, p2, total2));

  DESTROY_MAP(m1);
  DESTROY_MAP(m2);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief test reverse iteration with a single map
////////////////////////////////////////////////////////////////////////////////

BOOST_AUTO_TEST_CASE(tst_iter_rev_one) {
  INIT_MAP(m1);

  ELEMENT(empty, 0, 0);
  ELEMENT(e1, 1, 123);
  ELEMENT(e2, 2, 456);
  ELEMENT(e3, 3, 789);

  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m1.insert(nullptr, e1));
  BOOST_CHECK_EQUAL((uint64_t)1, m1.size());
  BOOST_CHECK_EQUAL(e1, m1.find(nullptr, e1));

  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m1.insert(nullptr, e2));
  BOOST_CHECK_EQUAL((uint64_t)2, m1.size());
  BOOST_CHECK_EQUAL(e2, m1.find(nullptr, e2));

  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m1.insert(nullptr, e3));
  BOOST_CHECK_EQUAL((uint64_t)3, m1.size());
  BOOST_CHECK_EQUAL(e3, m1.find(nullptr, e3));

  REV_POSITION(p1);
  BOOST_CHECK_EQUAL(e3, m1.findSequentialReverse(nullptr, p1));
  BOOST_CHECK_EQUAL(e2, m1.findSequentialReverse(nullptr, p1));
  BOOST_CHECK_EQUAL(e1, m1.findSequentialReverse(nullptr, p1));
  BOOST_CHECK_EQUAL(empty, m1.findSequentialReverse(nullptr, p1));
  BOOST_CHECK_EQUAL(empty, m1.findSequentialReverse(nullptr, p1));

  DESTROY_MAP(m1);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief test reverse iteration with two maps
////////////////////////////////////////////////////////////////////////////////

BOOST_AUTO_TEST_CASE(tst_iter_rev_two) {
  INIT_MAP(m1);
  INIT_MAP(m2);

  ELEMENT(empty, 0, 0);
  ELEMENT(e1, 1, 123);
  ELEMENT(e2, 2, 456);
  ELEMENT(e3, 3, 789);
  ELEMENT(e4, 4, 123);
  ELEMENT(e5, 5, 456);
  ELEMENT(e6, 6, 789);

  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m1.insert(nullptr, e1));
  BOOST_CHECK_EQUAL((uint64_t)1, m1.size());
  BOOST_CHECK_EQUAL(e1, m1.find(nullptr, e1));

  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m2.insert(nullptr, e2));
  BOOST_CHECK_EQUAL((uint64_t)1, m2.size());
  BOOST_CHECK_EQUAL(e2, m2.find(nullptr, e2));

  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m1.insert(nullptr, e3));
  BOOST_CHECK_EQUAL((uint64_t)2, m1.size());
  BOOST_CHECK_EQUAL(e3, m1.find(nullptr, e3));

  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m2.insert(nullptr, e4));
  BOOST_CHECK_EQUAL((uint64_t)2, m2.size());
  BOOST_CHECK_EQUAL(e4, m2.find(nullptr, e4));

  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m1.insert(nullptr, e5));
  BOOST_CHECK_EQUAL((uint64_t)3, m1.size());
  BOOST_CHECK_EQUAL(e5, m1.find(nullptr, e5));

  BOOST_CHECK_EQUAL(TRI_ERROR_NO_ERROR, m2.insert(nullptr, e6));
  BOOST_CHECK_EQUAL((uint64_t)3, m2.size());
  BOOST_CHECK_EQUAL(e6, m2.find(nullptr, e6));

  REV_POSITION(p1);
  REV_POSITION(p2);
  BOOST_CHECK_EQUAL(e5, m1.findSequentialReverse(nullptr, p1));
  BOOST_CHECK_EQUAL(e6, m2.findSequentialReverse(nullptr, p2));
  BOOST_CHECK_EQUAL(e3, m1.findSequentialReverse(nullptr, p1));
  BOOST_CHECK_EQUAL(e4, m2.findSequentialReverse(nullptr, p2));
  BOOST_CHECK_EQUAL(e1, m1.findSequentialReverse(nullptr, p1));
  BOOST_CHECK_EQUAL(e2, m2.findSequentialReverse(nullptr, p2));
  BOOST_CHECK_EQUAL(empty, m1.findSequentialReverse(nullptr, p1));
  BOOST_CHECK_EQUAL(empty, m1.findSequentialReverse(nullptr, p1));
  BOOST_CHECK_EQUAL(empty, m2.findSequentialReverse(nullptr, p2));
  BOOST_CHECK_EQUAL(empty, m2.findSequentialReverse(nullptr, p2));

  DESTROY_MAP(m1);
  DESTROY_MAP(m2);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief generate tests
////////////////////////////////////////////////////////////////////////////////

BOOST_AUTO_TEST_SUITE_END()

// Local Variables:
// mode: outline-minor
// outline-regexp: "^\\(/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|//
// --SECTION--\\|/// @\\}\\)"
// End:
