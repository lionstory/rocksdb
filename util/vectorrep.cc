#include "rocksdb/memtablerep.h"

#include <unordered_set>
#include <set>
#include <memory>
#include <algorithm>
#include <type_traits>

#include "rocksdb/arena.h"
#include "port/port.h"
#include "util/mutexlock.h"
#include "util/stl_wrappers.h"

namespace rocksdb {
namespace {

using namespace stl_wrappers;

class VectorRep : public MemTableRep {
 public:
  VectorRep(const KeyComparator& compare, Arena* arena, size_t count);

  // Insert key into the collection. (The caller will pack key and value into a
  // single buffer and pass that in as the parameter to Insert)
  // REQUIRES: nothing that compares equal to key is currently in the
  // collection.
  virtual void Insert(const char* key) override;

  // Returns true iff an entry that compares equal to key is in the collection.
  virtual bool Contains(const char* key) const override;

  virtual void MarkReadOnly() override;

  virtual size_t ApproximateMemoryUsage() override;

  virtual ~VectorRep() override { }

  class Iterator : public MemTableRep::Iterator {
    class VectorRep* vrep_;
    std::shared_ptr<std::vector<const char*>> bucket_;
    typename std::vector<const char*>::const_iterator mutable cit_;
    const KeyComparator& compare_;
    bool mutable sorted_;
    void DoSort() const;
   public:
    explicit Iterator(class VectorRep* vrep,
      std::shared_ptr<std::vector<const char*>> bucket,
      const KeyComparator& compare);

    // Initialize an iterator over the specified collection.
    // The returned iterator is not valid.
    // explicit Iterator(const MemTableRep* collection);
    virtual ~Iterator() override { };

    // Returns true iff the iterator is positioned at a valid node.
    virtual bool Valid() const override;

    // Returns the key at the current position.
    // REQUIRES: Valid()
    virtual const char* key() const override;

    // Advances to the next position.
    // REQUIRES: Valid()
    virtual void Next() override;

    // Advances to the previous position.
    // REQUIRES: Valid()
    virtual void Prev() override;

    // Advance to the first entry with a key >= target
    virtual void Seek(const char* target) override;

    // Position at the first entry in collection.
    // Final state of iterator is Valid() iff collection is not empty.
    virtual void SeekToFirst() override;

    // Position at the last entry in collection.
    // Final state of iterator is Valid() iff collection is not empty.
    virtual void SeekToLast() override;
  };

  // Unhide default implementations of GetIterator()
  using MemTableRep::GetIterator;

  // Return an iterator over the keys in this representation.
  virtual std::shared_ptr<MemTableRep::Iterator> GetIterator() override;

 private:
  friend class Iterator;
  typedef std::vector<const char*> Bucket;
  std::shared_ptr<Bucket> bucket_;
  mutable port::RWMutex rwlock_;
  bool immutable_;
  bool sorted_;
  const KeyComparator& compare_;
};

void VectorRep::Insert(const char* key) {
  assert(!Contains(key));
  WriteLock l(&rwlock_);
  assert(!immutable_);
  bucket_->push_back(key);
}

// Returns true iff an entry that compares equal to key is in the collection.
bool VectorRep::Contains(const char* key) const {
  ReadLock l(&rwlock_);
  return std::find(bucket_->begin(), bucket_->end(), key) != bucket_->end();
}

void VectorRep::MarkReadOnly() {
  WriteLock l(&rwlock_);
  immutable_ = true;
}

size_t VectorRep::ApproximateMemoryUsage() {
  return
    sizeof(bucket_) + sizeof(*bucket_) +
    bucket_->size() *
    sizeof(
      std::remove_reference<decltype(*bucket_)>::type::value_type
    );
}

VectorRep::VectorRep(const KeyComparator& compare, Arena* arena, size_t count)
  : bucket_(new Bucket()),
    immutable_(false),
    sorted_(false),
    compare_(compare) { bucket_.get()->reserve(count); }

VectorRep::Iterator::Iterator(class VectorRep* vrep,
                   std::shared_ptr<std::vector<const char*>> bucket,
                   const KeyComparator& compare)
: vrep_(vrep),
  bucket_(bucket),
  cit_(nullptr),
  compare_(compare),
  sorted_(false) { }

void VectorRep::Iterator::DoSort() const {
  // vrep is non-null means that we are working on an immutable memtable
  if (!sorted_ && vrep_ != nullptr) {
    WriteLock l(&vrep_->rwlock_);
    if (!vrep_->sorted_) {
      std::sort(bucket_->begin(), bucket_->end(), Compare(compare_));
      cit_ = bucket_->begin();
      vrep_->sorted_ = true;
    }
    sorted_ = true;
  }
  if (!sorted_) {
    std::sort(bucket_->begin(), bucket_->end(), Compare(compare_));
    cit_ = bucket_->begin();
    sorted_ = true;
  }
  assert(sorted_);
  assert(vrep_ == nullptr || vrep_->sorted_);
}

// Returns true iff the iterator is positioned at a valid node.
bool VectorRep::Iterator::Valid() const {
  DoSort();
  return cit_ != bucket_->end();
}

// Returns the key at the current position.
// REQUIRES: Valid()
const char* VectorRep::Iterator::key() const {
  assert(Valid());
  return *cit_;
}

// Advances to the next position.
// REQUIRES: Valid()
void VectorRep::Iterator::Next() {
  assert(Valid());
  if (cit_ == bucket_->end()) {
    return;
  }
  ++cit_;
}

// Advances to the previous position.
// REQUIRES: Valid()
void VectorRep::Iterator::Prev() {
  assert(Valid());
  if (cit_ == bucket_->begin()) {
    // If you try to go back from the first element, the iterator should be
    // invalidated. So we set it to past-the-end. This means that you can
    // treat the container circularly.
    cit_ = bucket_->end();
  } else {
    --cit_;
  }
}

// Advance to the first entry with a key >= target
void VectorRep::Iterator::Seek(const char* target) {
  DoSort();
  // Do binary search to find first value not less than the target
  cit_ = std::equal_range(bucket_->begin(),
                          bucket_->end(),
                          target,
                          [this] (const char* a, const char* b) {
                            return compare_(a, b) < 0;
                          }).first;
}

// Position at the first entry in collection.
// Final state of iterator is Valid() iff collection is not empty.
void VectorRep::Iterator::SeekToFirst() {
  DoSort();
  cit_ = bucket_->begin();
}

// Position at the last entry in collection.
// Final state of iterator is Valid() iff collection is not empty.
void VectorRep::Iterator::SeekToLast() {
  DoSort();
  cit_ = bucket_->end();
  if (bucket_->size() != 0) {
    --cit_;
  }
}

std::shared_ptr<MemTableRep::Iterator> VectorRep::GetIterator() {
  ReadLock l(&rwlock_);
  // Do not sort here. The sorting would be done the first time
  // a Seek is performed on the iterator.
  if (immutable_) {
    return std::make_shared<Iterator>(this, bucket_, compare_);
  } else {
    std::shared_ptr<Bucket> tmp;
    tmp.reset(new Bucket(*bucket_)); // make a copy
    return std::make_shared<Iterator>(nullptr, tmp, compare_);
  }
}
} // anon namespace

std::shared_ptr<MemTableRep> VectorRepFactory::CreateMemTableRep(
  MemTableRep::KeyComparator& compare, Arena* arena) {
  return std::make_shared<VectorRep>(compare, arena, count_);
}
} // namespace rocksdb