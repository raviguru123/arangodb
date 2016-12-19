#ifndef CUCKOO_HELPERS_H
#define CUCKOO_HELPERS_H 1

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>
#include <mutex>

// A default hash function:
uint64_t fasthash64(const void* buf, size_t len, uint64_t seed);

// C++ wrapper for the hash function:
template <class T, uint64_t Seed>
class HashWithSeed {
 public:
  uint64_t operator()(T const& t) const {
    // Some implementation like Fnv or xxhash looking at bytes in type T,
    // taking the seed into account.
    auto p = reinterpret_cast<void const*>(&t);
    return fasthash64(p, sizeof(T), Seed);
  }
};

// C++ wrapper for the hash function:
template <uint64_t Seed>
class HashStringWithSeed {
 public:
  uint64_t operator()(std::string const& t) const {
    // Some implementation like Fnv or xxhash looking at bytes in type T,
    // taking the seed into account.
    auto p = reinterpret_cast<void const*>(t.data());
    return fasthash64(p, t.size(), Seed);
  }
};

class MyMutexGuard {
  std::mutex& _mutex;
  bool _locked;

 public:
  MyMutexGuard(std::mutex& m);
  ~MyMutexGuard();
  void release();
};

int remove_directory(const char* path);

#endif
