#ifndef CUCKOO_FILTER_CUCKOO_FILTER_H_
#define CUCKOO_FILTER_CUCKOO_FILTER_H_

#include <assert.h>
#include <math.h>
#include <algorithm>

#include <bits/stdc++.h>
#include "debug.h"
#include "hashutil.h"
#include "packedtable.h"
#include "printutil.h"
#include "singletable.h"

namespace cuckoofilter {
// status returned by a cuckoo filter operation
enum Status {
  Ok = 0,
  NotFound = 1,
  NotEnoughSpace = 2,
  NotSupported = 3,
};

// maximum number of cuckoo kicks before claiming failure
const size_t kMaxCuckooCount = 500;

// A cuckoo filter class exposes a Bloomier filter interface,
// providing methods of Add, Delete, Contain. It takes three
// template parameters:
//   ItemType:  the type of item you want to insert
//   bits_per_item: how many bits each item is hashed into
//   TableType: the storage of table, SingleTable by default, and
// PackedTable to enable semi-sorting
template <typename ItemType, size_t bits_per_item,
          template <size_t> class TableType = SingleTable,
          // typename HashFamily = TwoIndependentMultiplyShift>
          typename HashFamily = std::hash<ItemType>>
class CuckooFilter {
  // Storage of items
  TableType<bits_per_item> *table_;

  // Number of items stored
  size_t num_items_;

  typedef struct {
    size_t index;
    uint32_t tag;
    bool used;
  } VictimCache;

  VictimCache victim_;

  HashFamily hasher_;

  size_t *indices_;  // config rehashing

  bool paired_;

  inline size_t IndexHash(uint32_t hv) const {
    // table_->num_buckets is always a power of two, so modulo can be replaced
    // with
    // bitwise-and:
    return hv & (table_->NumBuckets() - 1);
  }

  inline uint32_t TagHash(uint32_t hv) const {
    uint32_t tag;
    // tag = (static_cast<uint32_t>(hv)^static_cast<uint32_t>(hv >>
    // bits_per_item));
    tag = hv & ((1ULL << bits_per_item) - 1);
    tag += (tag == 0);
    return tag;
  }

  inline void GenerateTagHash(const ItemType &item, uint32_t *tag) const {
    const uint64_t hash = hasher_(item);
    // std::cout << "CF hashed key: " << hash << "\n";
    *tag = TagHash(hash);
  }

  inline void GenerateIndexTagHash(const ItemType &item, size_t *index,
                                   uint32_t *tag) const {
    const uint64_t hash = hasher_(item);
    *index = IndexHash(hash >> 32);
    // if (!paired_) {
    //   *index = IndexHash(hash >> 32);
    // } else {
    //   // updated to match hashtable (hm = (table_->NumBuckets() - 1))
    //   *index = IndexHash(hash);
    // }
    *tag = TagHash(hash);
  }

  // if paired cuckoo hashing, find alt using hashed item/key
  inline size_t AltIndex(const size_t index, const uint32_t tag,
                         const uint64_t hv = 0) const {
    // NOTE(binfan): originally we use:
    // index ^ HashUtil::BobHash((const void*) (&tag), 4)) & table_->INDEXMASK;
    // now doing a quick-n-dirty way:
    // 0x5bd1e995 is the hash constant from MurmurHash2
    if (paired_) {
      size_t hp = log2(table_->NumBuckets());
      // size_t hm = table_->NumBuckets() - 1;
      const size_t tag = (hv >> hp) + 1;

      return IndexHash((uint32_t)(index ^ (tag * 0xc6a4a7935bd1e995)));
    } else
      return IndexHash((uint32_t)(index ^ (tag * 0x5bd1e995)));
  }

  // takes fully hashed key, rather than tag of fixed number of bits
  // inline size_t AltIndexItem(const size_t index, const uint64_t hv) const {
  //   size_t hp = log2(table_->NumBuckets());
  //   size_t hm = table_->NumBuckets() - 1;
  //   const size_t tag = (hv >> hp) + 1;
  //   // std::cout << "CF hp: " << hp << ", right shift: " << tag << " tag: "
  //   << hv
  //   //           << "\n";
  //   // std::cout << "CF hashmask (hm): " << hm << "\n";

  //   // ^ (bitwise XOR), & (bitwise AND)
  //   return (index ^ (tag * 0xc6a4a7935bd1e995)) & hm;
  // }

  Status PairedInsertImpl(std::stack<std::pair<size_t, size_t>> &trail,
                          const uint32_t tag);

  Status AddImpl(const size_t i, const uint32_t tag);

  // load factor is the fraction of occupancy
  double LoadFactor() const { return 1.0 * Size() / table_->SizeInTags(); }

  double BitsPerItem() const { return 8.0 * table_->SizeInBytes() / Size(); }

 public:
  // original constructor
  explicit CuckooFilter(const size_t max_num_keys)
      : num_items_(0), victim_(), hasher_() {
    // std::cout << "CF orig. constructor\n";
    paired_ = false;
    size_t assoc = 4;
    size_t num_buckets =
        upperpower2(std::max<uint64_t>(1, max_num_keys / assoc));
    double frac = (double)max_num_keys / num_buckets / assoc;
    if (frac > 0.96) {
      num_buckets <<= 1;
    }
    victim_.used = false;
    table_ = new TableType<bits_per_item>(num_buckets);
  }

  // modified for paired cuckoo stuff
  explicit CuckooFilter(const size_t max_num_keys, const HashFamily &hf,
                        const size_t bucket_count)
      : num_items_(0), victim_(), hasher_(hf) {  // max_num_keys
    // std::cout << "CF mod constructor\n";
    paired_ = true;
    size_t assoc = 4;
    // size_t num_buckets = upperpower2(std::max<uint64_t>(1, max_num_keys /
    // assoc));
    size_t num_buckets = bucket_count;
    // double frac = (double)max_num_keys / num_buckets / assoc;
    // if (frac > 0.96) {
    //   num_buckets <<= 1;
    // }
    victim_.used = false;
    indices_ = new size_t[num_buckets]();  // () initializes all values to 0
    // for (int i = 0; i < sizeof(indices); i++) {
    //   std::cout << indices[i] << " ";
    // }
    // std::cout << "\nindices array size: "
    //           << (sizeof(indices) / sizeof(*indices)) << "\n";
    table_ = new TableType<bits_per_item>(num_buckets);
  }

  // ~CuckooFilter() { delete table_; }
  ~CuckooFilter() {
    delete table_;
    delete[] indices_;
  }

  // Inserts an item to the filter at a specific location,
  // given index and slot.
  Status PairedInsert(const ItemType &item,
                      std::stack<std::pair<size_t, size_t>> &trail);

  // Add an item to the filter.
  Status Add(const ItemType &item);

  // Report if the item is inserted, with false positive rate.
  Status Contain(const ItemType &item) const;

  // Delete an key from the filter
  Status Delete(const ItemType &item);

  /* methods for providing stats  */
  // summary infomation
  std::string Info() const;

  // number of current inserted items;
  size_t Size() const { return num_items_; }

  // size of the filter in bytes.
  size_t SizeInBytes() const { return table_->SizeInBytes(); }
};

template <typename ItemType, size_t bits_per_item,
          template <size_t> class TableType, typename HashFamily>
Status
CuckooFilter<ItemType, bits_per_item, TableType, HashFamily>::PairedInsert(
    const ItemType &item, std::stack<std::pair<size_t, size_t>> &trail) {
  size_t i;
  uint32_t tag;
  GenerateTagHash(item, &tag);
  // GenerateIndexTagHash(item, &i, &tag);
  // std::cout << "trail index: " << trail.top().first << " genIndex: " << i <<
  // "\n";

  // if (trail.size() > 1)
  return PairedInsertImpl(trail, tag);
  // else
  // return AddImpl(i, tag);
}

template <typename ItemType, size_t bits_per_item,
          template <size_t> class TableType, typename HashFamily>
Status CuckooFilter<ItemType, bits_per_item, TableType,
                    HashFamily>::PairedInsertImpl(  // const size_t i,
    std::stack<std::pair<size_t, size_t>> &trail, const uint32_t genTag) {
  // size_t index = i;
  uint32_t tag = genTag;
  uint32_t oldtag;

  // if (trail.size() > 1)
  //   std::cout << "cuckoo trail length: " << trail.size() << "\n";

  while (!trail.empty()) {
    bool kickout = trail.size() > 1;
    std::pair<size_t, size_t> location = trail.top();
    size_t index = location.first;
    size_t slot = location.second;

    // std::cout << "inserting tag " << tag << ": " << index << ", " << slot
    //           << "\t";
    if (table_->PairedInsertTagToBucket(index, slot, tag, kickout, oldtag)) {
      num_items_++;
      // std::cout << "\n";
      return Ok;
    }
    if (kickout) {
      tag = oldtag;
    }
    // index = location.first;
    // slot = location.second;
    trail.pop();
  }

  // victim_.index = index;
  // victim_.tag = tag;
  // victim_.used = true;
  return Ok;
  // } else {
  //   return NotSupported;
  // }
}

template <typename ItemType, size_t bits_per_item,
          template <size_t> class TableType, typename HashFamily>
Status CuckooFilter<ItemType, bits_per_item, TableType, HashFamily>::Add(
    const ItemType &item) {
  size_t i;
  uint32_t tag;

  if (victim_.used) {
    return NotEnoughSpace;
  }

  GenerateIndexTagHash(item, &i, &tag);
  return AddImpl(i, tag);
}

template <typename ItemType, size_t bits_per_item,
          template <size_t> class TableType, typename HashFamily>
Status CuckooFilter<ItemType, bits_per_item, TableType, HashFamily>::AddImpl(
    const size_t i, const uint32_t tag) {
  size_t curindex = i;
  uint32_t curtag = tag;
  uint32_t oldtag;

  for (uint32_t count = 0; count < kMaxCuckooCount; count++) {
    bool kickout = count > 0;
    oldtag = 0;
    if (table_->InsertTagToBucket(curindex, curtag, kickout, oldtag)) {
      num_items_++;
      return Ok;
    }
    if (kickout) {
      curtag = oldtag;
    }
    curindex = AltIndex(curindex, curtag);
  }

  victim_.index = curindex;
  victim_.tag = curtag;
  victim_.used = true;
  return Ok;
}

template <typename ItemType, size_t bits_per_item,
          template <size_t> class TableType, typename HashFamily>
Status CuckooFilter<ItemType, bits_per_item, TableType, HashFamily>::Contain(
    const ItemType &key) const {
  bool found = false;
  size_t i1, i2;
  uint32_t tag;
  uint64_t hv;

  GenerateIndexTagHash(key, &i1, &tag);

  if (paired_) {
    // use standard cuckoo hashing with item
    hv = hasher_(key);
    i2 = AltIndex(i1, tag, hv);
    assert(i1 == AltIndex(i2, tag, hv));

  } else {
    // partial cuckoo hashing with tag only
    i2 = AltIndex(i1, tag);
    assert(i1 == AltIndex(i2, tag));
  }

  found = victim_.used && (tag == victim_.tag) &&
          (i1 == victim_.index || i2 == victim_.index);

  if (found || table_->FindTagInBuckets(i1, i2, tag)) {
    // std::cout << "CF Contain: " << key;
    // if (paired_) {
    //   std::cout << " hv: " << hv;
    // }
    // std::cout << " tag: " << tag << " found in either bucket " << i1 << " or "
    //           << i2 << "\n\n";

    return Ok;
  } else {
    return NotFound;
  }
}

template <typename ItemType, size_t bits_per_item,
          template <size_t> class TableType, typename HashFamily>
Status CuckooFilter<ItemType, bits_per_item, TableType, HashFamily>::Delete(
    const ItemType &key) {
  size_t i1, i2;
  uint32_t tag;

  GenerateIndexTagHash(key, &i1, &tag);
  i2 = AltIndex(i1, tag);

  if (table_->DeleteTagFromBucket(i1, tag)) {
    num_items_--;
    goto TryEliminateVictim;
  } else if (table_->DeleteTagFromBucket(i2, tag)) {
    num_items_--;
    goto TryEliminateVictim;
  } else if (victim_.used && tag == victim_.tag &&
             (i1 == victim_.index || i2 == victim_.index)) {
    // num_items_--;
    victim_.used = false;
    return Ok;
  } else {
    return NotFound;
  }
TryEliminateVictim:
  if (victim_.used) {
    victim_.used = false;
    size_t i = victim_.index;
    uint32_t tag = victim_.tag;
    AddImpl(i, tag);
  }
  return Ok;
}

template <typename ItemType, size_t bits_per_item,
          template <size_t> class TableType, typename HashFamily>
std::string CuckooFilter<ItemType, bits_per_item, TableType, HashFamily>::Info()
    const {
  std::stringstream ss;
  ss << "CuckooFilter Status:\n"
     << table_->Info() << "\n"
     << "\t\tKeys stored: " << Size() << "\n"
     << "\t\tLoad factor: " << LoadFactor() << "\n"
     << "\t\tHashtable size: " << (table_->SizeInBytes() >> 10) << " KB\n";
  if (Size() > 0) {
    ss << "\t\tbit/key:   " << BitsPerItem() << "\n";
  } else {
    ss << "\t\tbit/key:   N/A\n";
  }
  return ss.str();
}
}  // namespace cuckoofilter
#endif  // CUCKOO_FILTER_CUCKOO_FILTER_H_
