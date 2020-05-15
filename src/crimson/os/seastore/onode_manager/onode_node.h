// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 smarttab

#include <algorithm>
#include <cstdint>
#include <variant>

#include "common/hobject.h"
#include "crimson/os/seastore/onode.h"
#include "crimson/os/seastore/seastore_types.h"
#include "layout.h"
#include "onode_delta.h"

namespace asci = absl::container_internal;

namespace boost::beast {
  template<class T>
  bool operator==(const span<T>& lhs, const span<T>& rhs) {
    return std::equal(
        lhs.begin(), lhs.end(),
        rhs.begin(), rhs.end());
  }
}

// on-disk onode
// it only keeps the bits necessary to rebuild an in-memory onode
struct [[gnu::packed]] onode_t {
  onode_t& operator=(const onode_t& onode) {
    ver = onode.ver;
    len = onode.len;
    std::memcpy(data, onode.data, len);
    return *this;
  }
  size_t size() const {
    return sizeof(*this) + len;
  }
  OnodeRef decode() const {
    return new crimson::os::seastore::Onode(std::string_view{data, len});
  }

  uint8_t ver;
  uint16_t len;
  char data[];
};

static inline std::ostream& operator<<(std::ostream& os, const onode_t& onode) {
  return os << *onode.decode();
}

using crimson::os::seastore::laddr_t;

struct [[gnu::packed]] child_addr_t {
  laddr_t data;
  child_addr_t(laddr_t data)
    : data{data}
  {}
  child_addr_t& operator=(laddr_t addr) {
    data = addr;
    return *this;
  }
  laddr_t get() const {
    return data;
  }
  operator laddr_t() const {
    return data;
  }
  size_t size() const {
    return sizeof(laddr_t);
  }
};

// poor man's operator<=>
enum class ordering_t {
  less,
  equivalent,
  greater,
};

template<class L, class R>
ordering_t compare_element(const L& x, const R& y)
{
  if constexpr (std::is_arithmetic_v<L>) {
    static_assert(std::is_arithmetic_v<R>);
    if (x < y) {
      return ordering_t::less;
    } else if (x > y) {
      return ordering_t::greater;
    } else {
      return ordering_t::equivalent;
    }
  } else {
    // string_view::compare(), string::compare(), ...
    auto result = x.compare(y);
    if (result < 0) {
      return ordering_t::less;
    } else if (result > 0) {
      return ordering_t::greater;
    } else {
      return ordering_t::equivalent;
    }
  }
}

template<typename L, typename R>
constexpr ordering_t tuple_cmp(const L&, const R&, std::index_sequence<>)
{
  return ordering_t::equivalent;
}

template<typename L, typename R,
         size_t Head, size_t... Tail>
constexpr ordering_t tuple_cmp(const L& x, const R& y,
                               std::index_sequence<Head, Tail...>)
{
  auto ordering = compare_element(std::get<Head>(x), std::get<Head>(y));
  if (ordering != ordering_t::equivalent) {
    return ordering;
  } else {
    return tuple_cmp(x, y, std::index_sequence<Tail...>());
  }
}

template<typename... Ls, typename... Rs>
constexpr ordering_t cmp(const std::tuple<Ls...>& x,
                         const std::tuple<Rs...>& y)
{
  static_assert(sizeof...(Ls) == sizeof...(Rs));
  return tuple_cmp(x, y, std::index_sequence_for<Ls...>());
}

enum class likes_t {
 yes,
 no,
 maybe,
};

struct [[gnu::packed]] variable_sized_key_t {
  uint64_t snap;
  uint64_t gen;
  uint8_t nspace_len;
  uint8_t name_len;
  char data[];
  struct index_t {
    enum {
      nspace_data = 0,
      name_data = 1,
    };
  };
  using layout_type = asci::Layout<char, char>;
  layout_type cell_layout() const {
    return layout_type{nspace_len, name_len};
  }
  void set(const ghobject_t& oid) {
    snap = oid.hobj.snap;
    gen = oid.generation;
    nspace_len = oid.hobj.nspace.size();
    name_len = oid.hobj.oid.name.size();
    auto layout = cell_layout();
    std::memcpy(layout.Pointer<index_t::nspace_data>(data),
                oid.hobj.nspace.data(), oid.hobj.nspace.size());
    std::memcpy(layout.Pointer<index_t::name_data>(data),
                oid.hobj.oid.name.data(), oid.hobj.oid.name.size());
  }

  void update_oid(ghobject_t* oid) const {
    oid->hobj.snap = snap;
    oid->generation = gen;
    oid->hobj.nspace = nspace();
    oid->hobj.oid.name = name();
  }

  variable_sized_key_t& operator=(const variable_sized_key_t& key) {
    snap = key.snap;
    gen = key.gen;
    auto layout = cell_layout();
    auto nspace = key.nspace();
    std::copy_n(nspace.data(), nspace.size(),
                layout.Pointer<index_t::nspace_data>(data));
    auto name = key.name();
    std::copy_n(name.data(), name.size(),
                layout.Pointer<index_t::name_data>(data));
    return *this;
  }
  const std::string_view nspace() const {
    auto layout = cell_layout();
    auto nspace = layout.Slice<index_t::nspace_data>(data);
    return {nspace.data(), nspace.size()};
  }
  const std::string_view name() const {
    auto layout = cell_layout();
    auto name = layout.Slice<index_t::name_data>(data);
    return {name.data(), name.size()};
  }
  size_t size() const {
    return sizeof(*this) + nspace_len + name_len;
  }
  static size_t size_from(const ghobject_t& oid) {
    return (sizeof(variable_sized_key_t) +
            oid.hobj.nspace.size() +
            oid.hobj.oid.name.size());
  }
  ordering_t compare(const ghobject_t& oid) const {
    return cmp(std::tie(nspace(), name(), snap, gen),
               std::tie(oid.hobj.nspace, oid.hobj.oid.name, oid.hobj.snap.val,
                        oid.generation));
  }
  bool likes(const variable_sized_key_t& key) const {
    return nspace() == key.nspace() && name() == key.name();
  }
};

static inline std::ostream& operator<<(std::ostream& os, const variable_sized_key_t& k) {
  if (k.snap != CEPH_NOSNAP) {
    os << "s" << k.snap << ",";
  }
  if (k.gen != ghobject_t::NO_GEN) {
    os << "g" << k.gen << ",";
  }
  return os << k.nspace() << "/" << k.name();
}

// should use [[no_unique_address]] in C++20
struct empty_key_t {
  static constexpr ordering_t compare(const ghobject_t&) {
    return ordering_t::equivalent;
  }
  static void set(const ghobject_t&) {}
  static constexpr size_t size() {
    return 0;
  }
  static size_t size_from(const ghobject_t&) {
    return 0;
  }
  static void update_oid(ghobject_t*) {}
};

static inline std::ostream& operator<<(std::ostream& os, const empty_key_t&)
{
  return os;
}

enum class ntype_t : uint8_t {
  leaf,
  inner,
};

constexpr ntype_t flip_ntype(ntype_t ntype) noexcept
{
  if (ntype == ntype_t::leaf) {
    return ntype_t::inner;
  } else {
    return ntype_t::leaf;
  }
}

template<int N, ntype_t NodeType>
struct FixedKey {};

template<ntype_t NodeType>
struct FixedKey<0, NodeType>
{
  static constexpr int level = 0;
  int8_t shard = -1;
  int64_t pool = -1;
  uint32_t hash = 0;
  uint16_t offset = 0;

  FixedKey() = default;
  FixedKey(const ghobject_t& oid, uint16_t offset)
    : shard{oid.shard_id},
      pool{oid.hobj.pool},
      hash{oid.hobj.hash},
      offset{offset}
  {}

  void set(const ghobject_t& oid, uint16_t new_offset) {
    shard = oid.shard_id;
    pool = oid.hobj.pool;
    hash = oid.hobj.get_hash();
    offset = new_offset;
  }

  void set(const FixedKey& k, uint16_t new_offset) {
    shard = k.shard;
    pool = k.pool;
    hash = k.hash;
    offset = new_offset;
  }

  void update(const ghobject_t& oid) {
    shard = oid.shard_id;
    pool = oid.hobj.pool;
    hash = oid.hobj.get_hash();
  }

  void update_oid(ghobject_t* oid) const {
    oid->set_shard(shard_id_t{shard});
    oid->hobj.pool = pool;
    oid->hobj.set_hash(hash);
  }

  ordering_t compare(const ghobject_t& oid) const {
    // so std::tie() can bind them  by reference
    int8_t rhs_shard = oid.shard_id;
    uint32_t rhs_hash = oid.hobj.get_hash();
    return cmp(std::tie(shard, pool, hash),
               std::tie(rhs_shard, oid.hobj.pool, rhs_hash));
  }
  // @return true if i likes @c k, we will can be pushed down to next level
  //              in the same node
  likes_t likes(const FixedKey& k) const {
    if (shard == k.shard && pool == k.pool) {
      return likes_t::yes;
    } else {
      return likes_t::no;
    }
  }
};

template<ntype_t NodeType>
std::ostream& operator<<(std::ostream& os, const FixedKey<0, NodeType>& k) {
  if (k.shard != shard_id_t::NO_SHARD) {
    os << "s" << k.shard;
  }
  return os << "p=" << k.pool << ","
            << "h=" << std::hex << k.hash << std::dec << ","
            << ">" << k.offset;
}

// all elements in this node share the same <shard, pool>
template<ntype_t NodeType>
struct FixedKey<1, NodeType> {
  static constexpr int level = 1;
  uint32_t hash = 0;
  uint16_t offset = 0;

  FixedKey() = default;
  FixedKey(uint32_t hash, uint16_t offset)
    : hash{hash},
      offset{offset}
  {}
  FixedKey(const ghobject_t& oid, uint16_t offset)
    : FixedKey(oid.hobj.hash, offset)
  {}
  void set(const ghobject_t& oid, uint16_t new_offset) {
    hash = oid.hobj.get_hash();
    offset = new_offset;
  }
  template<int N>
  void set(const FixedKey<N, NodeType>& k, uint16_t new_offset) {
    hash = k.hash;
    offset = new_offset;
  }
  void update_oid(ghobject_t* oid) const {
    oid->hobj.set_hash(hash);
  }
  void update(const ghobject_t& oid) {
    hash = oid.hobj.get_hash();
  }
  ordering_t compare(const ghobject_t& oid) const {
    return compare_element(hash, oid.hobj.get_hash());
  }
  likes_t likes(const FixedKey& k) const {
    return hash == k.hash ? likes_t::yes : likes_t::no;
  }
};

template<ntype_t NodeType>
std::ostream& operator<<(std::ostream& os, const FixedKey<1, NodeType>& k) {
  return os << "0x" << std::hex << k.hash << std::dec << ","
            << ">" << k.offset;
}

// all elements in this node must share the same <shard, pool, hash>
template<ntype_t NodeType>
struct FixedKey<2, NodeType> {
  static constexpr int level = 2;
  uint16_t offset = 0;

  FixedKey() = default;

  static constexpr ordering_t compare(const ghobject_t& oid) {
    // need to compare the cell
    return ordering_t::equivalent;
  }
  // always defer to my cell for likeness
  constexpr likes_t likes(const FixedKey&) const {
    return likes_t::maybe;
  }
  void set(const ghobject_t&, uint16_t new_offset) {
    offset = new_offset;
  }
  template<int N>
  void set(const FixedKey<N, NodeType>&, uint16_t new_offset) {
    offset = new_offset;
  }
  void update(const ghobject_t&) {}
  void update_oid(ghobject_t*) const {}
};

template<ntype_t NodeType>
std::ostream& operator<<(std::ostream& os, const FixedKey<2, NodeType>& k) {
  return os << ">" << k.offset;
}

struct fixed_key_3 {
  uint64_t snap = 0;
  uint64_t gen = 0;

  fixed_key_3() = default;
  fixed_key_3(const ghobject_t& oid)
  : snap{oid.hobj.snap}, gen{oid.generation}
  {}
  ordering_t compare(const ghobject_t& oid) const {
    return cmp(std::tie(snap, gen),
               std::tie(oid.hobj.snap.val, oid.generation));
  }
  // no object likes each other at this level
  constexpr likes_t likes(const fixed_key_3&) const {
    return likes_t::no;
  }
  void update_with_oid(const ghobject_t& oid) {
    snap = oid.hobj.snap;
    gen = oid.generation;
  }
  void update_oid(ghobject_t* oid) const {
    oid->hobj.snap = snap;
    oid->generation = gen;
  }
};

static inline std::ostream& operator<<(std::ostream& os, const fixed_key_3& k) {
  if (k.snap != CEPH_NOSNAP) {
    os << "s" << k.snap << ",";
  }
  if (k.gen != ghobject_t::NO_GEN) {
    os << "g" << k.gen << ",";
  }
  return os;
}

// all elements in this node must share the same <shard, pool, hash, namespace, oid>
// but the unlike other FixedKey<>, a node with FixedKey<3> does not have
// variable_sized_key, so if it is an inner node, we can just embed the child
// addr right in the key.
template<>
struct FixedKey<3, ntype_t::inner> : public fixed_key_3 {
  static constexpr int level = 3;
  // the item is embedded in the key
  laddr_t child_addr = 0;

  FixedKey() = default;
  void set(const ghobject_t& oid, laddr_t new_child_addr) {
    update_with_oid(oid);
    child_addr = new_child_addr;
  }
  // unlikely get called, though..
  void update(const ghobject_t& oid) {}
  template<int N>
  std::enable_if_t<N < 3> set(const FixedKey<N, ntype_t::inner>&,
                              laddr_t new_child_addr) {
    child_addr = new_child_addr;
  }
  void set(const FixedKey& k, laddr_t new_child_addr) {
    snap = k.snap;
    gen = k.gen;
    child_addr = new_child_addr;
  }
  void set(const variable_sized_key_t& k, laddr_t new_child_addr) {
    snap = k.snap;
    gen = k.gen;
    child_addr = new_child_addr;
  }
};

template<>
struct FixedKey<3, ntype_t::leaf> : public fixed_key_3 {
  static constexpr int level = 3;
  uint16_t offset = 0;

  FixedKey() = default;
  void set(const ghobject_t& oid, uint16_t new_offset) {
    update_with_oid(oid);
    offset = new_offset;
  }
  void set(const FixedKey& k, uint16_t new_offset) {
    snap = k.snap;
    gen = k.gen;
    offset = new_offset;
  }
  template<int N>
  std::enable_if_t<N < 3> set(const FixedKey<N, ntype_t::leaf>&,
                              uint16_t new_offset) {
    offset = new_offset;
  }
};

// prefix-compress the key
struct prefix_compressed_key_t {
  struct index_t {
    enum {
      shared_key_len,
      unshared_key_len,
      onode_len,
      key_data,
      onode_data,
    };
  };
  using layout_type = asci::Layout<uint8_t,
                                   uint8_t,
                                   uint16_t,
                                   char,
                                   char>;
  constexpr static layout_type cell_layout(uint8_t unshared_len,
                                           uint16_t onode_len) {
    return layout_type{1, 1, 1, unshared_len, onode_len};
  }
};

struct tag_t {
  template<int N, ntype_t node_type>
  static constexpr tag_t create() {
    static_assert(std::clamp(N, 0, 4) == N);
    return tag_t{static_cast<uint8_t>(N) << 4 | static_cast<uint8_t>(node_type)};
  }
  bool is_leaf() const {
    return ntype_t{static_cast<uint8_t>(v & 0b0000'1111U)} == ntype_t::leaf;
  }
  int layout() const {
    return static_cast<int>(v >> 4);
  }
  uint8_t v;
};

// for calculating size of variable-sized item/key
template<class T>
size_t size_of(const T& t) {
  using decayed_t = std::decay_t<T>;
  if constexpr (std::is_scalar_v<decayed_t>) {
    return sizeof(decayed_t);
  } else {
    return t.size();
  }
}

enum class size_state_t {
  okay,
  underflow,
  overflow,
};

// layout of a node
template<size_t BlockSize,
         int N,
         ntype_t NodeType>
struct node_t {
  static_assert(N == std::clamp(N, 0, 4));
  constexpr static ntype_t node_type = NodeType;
  constexpr static int node_n = N;

  using key_t = FixedKey<N, NodeType>;
  using item_t = std::conditional_t<NodeType == ntype_t::leaf,
                                    onode_t,
                                    child_addr_t>;
  using const_item_t = std::conditional_t<NodeType == ntype_t::leaf,
                                          const onode_t&,
                                          child_addr_t>;
  // XXX: magic numbers here
  static constexpr bool item_in_key = (N == 3 &&
                                       NodeType == ntype_t::inner);
  using partial_key_t = std::conditional_t<N < 3,
                                           variable_sized_key_t,
                                           empty_key_t>;

  std::pair<const key_t&, const partial_key_t&>
  key_at(unsigned slot) const {
    auto& key = keys[slot];
    if constexpr (item_in_key) {
      return {key, partial_key_t{}};
    } else {
      auto p = from_end(key.offset);
      return {key, *reinterpret_cast<const partial_key_t*>(p)};
    }
  }

  // update an existing oid with the specified item
  ghobject_t update_oid_with_slot(unsigned slot, const ghobject_t& oid) const {
    auto [key1, key2] = key_at(slot);
    ghobject_t updated = oid;
    key1.update_oid(&updated);
    key2.update_oid(&updated);
    return updated;
  }

  const_item_t item_at(const key_t& key) const {
    if constexpr (item_in_key) {
      return key.child_addr;
    } else {
      assert(key.offset < BlockSize);
      auto p = from_end(key.offset);
      auto partial_key = reinterpret_cast<const partial_key_t*>(p);
      p += size_of(*partial_key);
      return *reinterpret_cast<const item_t*>(p);
    }
  }

  void dump(std::ostream& os) const {
    for (uint16_t i = 0; i < count; i++) {
      const auto& [key1, key2] = key_at(i);
      os << " [" << i << '/' << count - 1 << "]\n"
         << "  key1 = (" << key1 << ")\n"
         << "  key2 = (" << key2 << ")\n";
      const auto& item = item_at(key1);
      if (_is_leaf()) {
        os << " item = " << item << "\n";
      } else {
        os << " child = " << std::hex << item << std::dec << "\n";
      }
    }
  }

  // for debugging only.
  static constexpr bool is_leaf() {
    return node_type == ntype_t::leaf;
  }

  bool _is_leaf() const {
    return tag.is_leaf();
  }

  char* from_end(uint16_t offset) {
    auto end = reinterpret_cast<char*>(this) + BlockSize;
    return end - static_cast<int>(offset);
  }

  const char* from_end(uint16_t offset) const {
    auto end = reinterpret_cast<const char*>(this) + BlockSize;
    return end - static_cast<int>(offset);
  }

  uint16_t used_space() const {
    if constexpr (item_in_key) {
      return count * sizeof(key_t);
    } else {
      if (count) {
        return keys[count - 1].offset + count * sizeof(key_t);
      } else {
        return 0;
      }
    }
  }

  uint16_t free_space() const {
    return capacity() - used_space();
  }

  static constexpr uint16_t capacity() {
    auto p = reinterpret_cast<node_t*>(0);
    return BlockSize - (reinterpret_cast<char*>(p->keys) -
                         reinterpret_cast<char*>(p));
  }

  // TODO: if it's allowed to update 2 siblings at the same time, we can have
  //       B* tree
  static constexpr uint16_t min_size() {
    return capacity() / 2;
  }

  // @return <minimum bytes to grab, maximum bytes to grab>
  static constexpr std::pair<int16_t, int16_t> bytes_to_grab(uint16_t size) {
    assert(size < min_size());
    return {min_size() - size, capacity() - size};
  }

  // @return <minimum bytes to grab, maximum bytes to grab>
  static constexpr std::pair<int16_t, int16_t> bytes_to_push(uint16_t size) {
    assert(size > capacity());
    return {size - capacity(), size - min_size()};
  }

  size_state_t size_state(uint16_t size) const {
    if (size > capacity()) {
      return size_state_t::overflow;
    } else if (size < capacity() / 2) {
      return size_state_t::underflow;
    } else {
      return size_state_t::okay;
    }
  }

  bool is_underflow(uint16_t size) const {
    switch (size_state(size)) {
    case size_state_t::underflow:
      return true;
    case size_state_t::okay:
      return false;
    default:
      assert(0);
      return false;
    }
  }

  int16_t size_with_key(unsigned slot, const ghobject_t& oid) const {
    if constexpr (item_in_key) {
      return capacity();
    } else {
      // the size of fixed key does not change
      [[maybe_unused]] const auto& [key1, key2] = key_at(slot);
      return capacity() + partial_key_t::size_from(oid) - key2.size();
    }
  }

  ordering_t compare_with_slot(unsigned slot, const ghobject_t& oid) const {
    const auto& [key, partial_key] = key_at(slot);
    if (auto result = key.compare(oid); result != ordering_t::equivalent) {
      return result;
    } else {
      return partial_key.compare(oid);
    }
  }

  /// return the slot number of the first slot that is greater or equal to
  /// key
  std::pair<unsigned, bool> lower_bound(const ghobject_t& oid) const {
    unsigned s = 0, e = count;
    while (s != e) {
      unsigned mid = (s + e) / 2;
      switch (compare_with_slot(mid, oid)) {
      case ordering_t::less:
        s = ++mid;
        break;
      case ordering_t::greater:
        e = mid;
        break;
      case ordering_t::equivalent:
        assert(mid == 0 || mid < count);
        return {mid, true};
      }
    }
    return {s, false};
  }

  static uint16_t size_of_item(const ghobject_t& oid, const item_t& item) {
    if constexpr (item_in_key) {
      return sizeof(key_t);
    } else {
      return (sizeof(key_t) +
              partial_key_t::size_from(oid) + size_of(item));
    }
  }

  bool is_overflow(const ghobject_t& oid, const item_t& item) const {
    return free_space() < size_of_item(oid, item);
  }

  // inserts an item into the given slot, pushing all subsequent keys forward
  // @note if the item is not embedded in key, shift the right half as well
  void insert_at(unsigned slot,
                 const ghobject_t& oid,
                 const item_t& item) {
    assert(!is_overflow(oid, item));
    assert(slot <= count);
    if constexpr (item_in_key) {
      // shift the keys right
      key_t* key = keys + slot;
      key_t* last_key = keys + count;
      std::copy_backward(key, last_key, last_key + 1);
      key->set(oid, item);
    } else {
      const uint16_t size = partial_key_t::size_from(oid) + size_of(item);
      uint16_t offset = size;
      if (slot > 0) {
        offset += keys[slot - 1].offset;
      }
      if (slot < count) {
        //                                 V
        // |         |... //    ...|//////||    |
        // |         |... // ...|//////|   |    |
        // shift the partial keys and items left
        auto first = keys[slot - 1].offset;
        auto last = keys[count - 1].offset;
        std::memmove(from_end(last + size), from_end(last), last - first);
        // shift the keys right and update the pointers
        for (key_t* dst = keys + count; dst > keys + slot; dst--) {
          key_t* src = dst - 1;
          *dst = *src;
          dst->offset += size;
        }
      }
      keys[slot].set(oid, offset);
      auto p = from_end(offset);
      auto partial_key = reinterpret_cast<partial_key_t*>(p);
      partial_key->set(oid);
      p += size_of(*partial_key);
      auto item_ptr = reinterpret_cast<item_t*>(p);
      *item_ptr = item;
    }
    count++;
    assert(free_space() < BlockSize);
  }

  // used by InnerNode for updating the keys indexing its children when their lower boundaries
  // is updated
  void update_key_at(unsigned slot, const ghobject_t& oid) {
    if constexpr (is_leaf()) {
      assert(0);
    } else if constexpr (item_in_key) {
      keys[slot].update(oid);
    } else {
      const auto& [key1, key2] = key_at(slot);
      int16_t delta = partial_key_t::size_from(oid) - key2.size();
      if (delta > 0) {
        // shift the cells sitting at its left side
        auto first = keys[slot].offset;
        auto last = keys[count - 1].offset;
        std::memmove(from_end(last + delta), from_end(last), last - first);
        // update the pointers
        for (key_t* key = keys + slot; key < keys + count; key++) {
          key->offset += delta;
        }
      }
      keys[slot].update(oid);
      auto p = from_end(keys[slot].offset);
      auto partial_key = reinterpret_cast<partial_key_t*>(p);
      partial_key->set(oid);
      // we don't update item here
    }
  }

  // @return the number of element to grab
  std::pair<unsigned, uint16_t> calc_grab_front(uint16_t min_grab, uint16_t max_grab) const {
    // TODO: split by likeness
    uint16_t grabbed = 0;
    uint16_t used = used_space();
    int n = 0;
    for (; n < count; n++) {
      const auto& [key1, key2] = key_at(n);
      uint16_t to_grab = sizeof(key1) + size_of(key2);
      if constexpr (!item_in_key) {
        const auto& item = item_at(key1);
        to_grab += size_of(item);
      }
      if (grabbed + to_grab > max_grab) {
        break;
      }
      grabbed += to_grab;
    }
    if (grabbed >= min_grab) {
      if (n == count) {
        return {n, grabbed};
      } else if (!is_underflow(used - grabbed)) {
        return {n, grabbed};
      }
    }
    return {0, 0};
  }

  std::pair<unsigned, uint16_t> calc_grab_back(uint16_t min_grab, uint16_t max_grab) const {
    // TODO: split by likeness
    uint16_t grabbed = 0;
    uint16_t used = used_space();
    for (int i = count - 1; i >= 0; i--) {
      const auto& [key1, key2] = key_at(i);
      uint16_t to_grab = sizeof(key1) + size_of(key2);
      if constexpr (!item_in_key) {
        const auto& item = item_at(key1);
        to_grab += size_of(item);
      }
      grabbed += to_grab;
      if (is_underflow(used - grabbed)) {
        return {0, 0};
      } else if (grabbed > max_grab) {
        return {0, 0};
      } else if (grabbed >= min_grab) {
        return {i + 1, grabbed};
      }
    }
    return {0, 0};
  }

  template<int LeftN, class Mover>
  void grab_from_left(
    node_t<BlockSize, LeftN, NodeType>& left,
    unsigned n, uint16_t bytes,
    Mover& mover) {
    // TODO: rebuild keys if moving across different layouts
    //       group by likeness
    shift_right(n, bytes);
    mover.move_from(left.count - n, 0, n);
  }
  // have to define a set of trap functions
  template<int LeftN, class Mover, ntype_t O = flip_ntype(NodeType)>
  void grab_from_left(node_t<BlockSize, LeftN, O>&,
                      unsigned, uint16_t,
                      Mover&) {
    assert(0);
  }

  template<int RightN, class Mover>
  void acquire_right(node_t<BlockSize, RightN, NodeType>& right,
                     unsigned whoami, Mover& mover) {
    mover.move_from(0, count, right.count);
  }

  template<int RightN, class Mover, ntype_t O = flip_ntype(NodeType)>
  void acquire_right(node_t<BlockSize, RightN, O>&,
                     unsigned, Mover&) {
    assert(0);
  }
  // transfer n elements at the front of given node to me
  template<int RightN, class Mover, ntype_t O = flip_ntype(NodeType)>
  void grab_from_right(node_t<BlockSize, RightN, O>&,
                       unsigned, uint16_t,
                       Mover&) {
    assert(0);
  }
  template<int RightN, class Mover>
  void grab_from_right(node_t<BlockSize, RightN, NodeType>& right,
                       unsigned n, uint16_t bytes,
                       Mover& mover) {
    mover.move_from(0, count, n);
    right.shift_left(n, 0);
  }

  template<int LeftN, class Mover, ntype_t O = flip_ntype(NodeType)>
  void push_to_left(node_t<BlockSize, LeftN, O>&,
                       unsigned, uint16_t,
                       Mover&) {
    assert(0);
  }
  template<int LeftN, class Mover>
  void push_to_left(node_t<BlockSize, LeftN, NodeType>& left,
                    unsigned n, uint16_t bytes,
                    Mover& mover) {
    left.grab_from_right(*this, n, bytes, mover);
  }

  template<int RightN, class Mover, ntype_t O = flip_ntype(NodeType)>
  void push_to_right(node_t<BlockSize, RightN, O>&,
                     unsigned, uint16_t,
                     Mover&) {
    assert(0);
  }
  template<int RightN, class Mover>
  void push_to_right(node_t<BlockSize, RightN, NodeType>& right,
                     unsigned n, uint16_t bytes,
                     Mover& mover) {
    right.grab_from_left(*this, n, bytes, mover);
  }
  // [to, from) are removed, so we need to shift left
  // actually there are only two use cases:
  // - to = 0: for giving elements in bulk
  // - to = from - 1: for removing a single element
  // old: |////|.....|   |.....|/|........|
  // new: |.....|        |.....||........|
  void shift_left(unsigned from, unsigned to) {
    assert(from < count);
    assert(to < from);
    if constexpr (item_in_key) {
      std::copy(keys + from, keys + count, keys + to);
    } else {
      const uint16_t cell_hi = keys[count - 1].offset;
      const uint16_t cell_lo = keys[from - 1].offset;
      const uint16_t offset_delta = keys[from].offset - keys[to].offset;
      for (auto src_key = keys + from, dst_key = keys + to;
           src_key != keys + count;
           ++src_key, ++dst_key) {
        // shift the keys left
        *dst_key = *src_key;
        // update the pointers
        dst_key->offset -= offset_delta;
      }
      // and cells
      auto dst = from_end(cell_hi);
      std::memmove(dst + offset_delta, dst, cell_hi - cell_lo);
    }
    count -= (from - to);
  }

  void insert_front(ceph::bufferptr&& keys_buf, ceph::bufferptr&& cells_buf) {
    unsigned n = keys_buf.length() / sizeof(key_t);
    shift_right(n, cells_buf.length());
    keys_buf.copy_out(0, keys_buf.length(), keys);
    cells_buf.copy_out(0, cells_buf.length(), from_end(keys[n - 1].offset));
  }


  void insert_back(ceph::bufferptr&& keys_buf, ceph::bufferptr&& cells_buf) {
    keys_buf.copy_out(0, keys_buf.length(), reinterpret_cast<char*>(keys + count));
    count += keys_buf.length() / sizeof(key_t);
    cells_buf.copy_out(0, cells_buf.length(), from_end(keys[count - 1].offset));
  }

  // one or more elements are inserted, so we need to shift the elements right
  // actually there are only two use cases:
  // - bytes != 0: for inserting bytes before from
  // - bytes = 0: for inserting a single element before from
  // old: ||.....|
  // new: |/////|.....|
  void shift_right(unsigned n, unsigned bytes) {
    assert(bytes + used_space() < capacity());
    // shift the keys left
    std::copy_backward(keys, keys + count, keys + count + n);
    count += n;
    if constexpr (!item_in_key) {
      uint16_t cells = keys[count - 1].offset;
      // copy the partial keys and items
      std::memmove(from_end(cells + bytes), from_end(cells), cells);
      // update the pointers
      for (auto key = keys + n; key < keys + count; ++key) {
        key->offset += bytes;
      }
    }
  }

  // shift all keys after slot is removed.
  // @note if the item is not embdedded in key, all items sitting at the left
  //       side of it will be shifted right
  void remove_from(unsigned slot) {
    assert(slot < count);
    if (unsigned next = slot + 1; next < count) {
      shift_left(next, slot);
    } else {
      // slot is the last one
      count--;
    }
  }

  //         /-------------------------------|
  //         |                               V
  // |header|k0|k1|k2|...  | / / |k2'v2|k1'v1|k0'.v0| v_m |
  //        |<-- count  -->|
  tag_t tag = tag_t::create<N, NodeType>();
  // the count of values in the node
  uint16_t count = 0;
  key_t keys[];
};

template<class parent_t,
         class from_t,
         class to_t,
         typename=void>
class EntryMover {
public:
  // a "trap" mover
  EntryMover(const parent_t&, from_t&, to_t& dst, unsigned) {
    assert(0);
  }
  void move_from(unsigned, unsigned, unsigned) {
    assert(0);
  }
  delta_t get_delta() {
    return delta_t::nop();
  }
};

// lower the layout, for instance, from L0 to L1, no reference oid is used
template<class parent_t,
         class from_t,
         class to_t>
class EntryMover<parent_t,
                 from_t,
                 to_t,
                 std::enable_if_t<from_t::node_n < to_t::node_n>>
{
public:
  EntryMover(const parent_t&, from_t& src, to_t& dst, unsigned)
    : src{src}, dst{dst}
  {}

  void move_from(unsigned src_first, unsigned dst_first, unsigned n)
  {
    ceph::bufferptr keys_buf{n * sizeof(to_t::key_t)};
    ceph::bufferptr cells_buf;
    auto dst_keys = reinterpret_cast<typename to_t::key_t*>(keys_buf.c_str());
    if constexpr (to_t::item_in_key) {
      for (unsigned i = 0; i < n; i++) {
        const auto& [k1, k2] = src.key_at(src_first + i);
        dst_keys[i].set(k2, src.item_at(k1));
      }
    } else {
      // copy keys
      uint16_t src_offset = src_first > 0 ? src.keys[src_first - 1].offset : 0;
      uint16_t dst_offset = dst_first > 0 ? dst.keys[dst_first - 1].offset : 0;
      for (unsigned i = 0; i < n; i++) {
        auto& src_key = src.keys[src_first + i];
        uint16_t offset = src_key.offset - src_offset + dst_offset;
        dst_keys[i].set(src_key, offset);
      }
      // copy cells in bulk, yay!
      auto src_end = src.keys[src_first + n - 1].offset;
      uint16_t total_cell_size = src_end - src_offset;
      cells_buf = ceph::bufferptr{total_cell_size};
      cells_buf.copy_in(0, total_cell_size, src.from_end(src_end));
    }
    if (dst_first == dst.count) {
      dst_delta = delta_t::insert_back(keys_buf, cells_buf);
    } else {
      dst_delta = delta_t::insert_front(keys_buf, cells_buf);
    }
    if (src_first > 0 && src_first + n == src.count) {
      src_delta = delta_t::truncate(n);
    } else if (src_first == 0 && n < src.count) {
      src_delta = delta_t::shift_left(n);
    } else if (src_first == 0 && n == src.count) {
      // the caller will retire the src extent
    } else {
      // grab in the middle?
      assert(0);
    }
  }

  delta_t from_delta() {
    return std::move(src_delta);
  }
  delta_t to_delta() {
    return std::move(dst_delta);
  }
private:
  const from_t& src;
  const to_t& dst;
  delta_t dst_delta;
  delta_t src_delta;
};

// lift the layout, for instance, from L2 to L0, need a reference oid
template<class parent_t,
         class from_t,
         class to_t>
class EntryMover<parent_t, from_t, to_t,
                 std::enable_if_t<(from_t::node_n > to_t::node_n)>>
{
public:
  EntryMover(const parent_t& parent, from_t& src, to_t& dst, unsigned from_slot)
    : src{src}, dst{dst}, ref_oid{parent->update_oid_with_slot(from_slot, {})}
  {}
  void move_from(unsigned src_first, unsigned dst_first, unsigned n)
  {
    ceph::bufferptr keys_buf{n * sizeof(to_t::key_t)};
    ceph::bufferptr cells_buf;
    auto dst_keys = reinterpret_cast<typename to_t::key_t*>(keys_buf.c_str());
    uint16_t in_node_offset = dst_first > 0 ? dst.keys[dst_first - 1].offset : 0;
    static_assert(!std::is_same_v<typename to_t::partial_key_t, empty_key_t>);
    // copy keys
    uint16_t buf_offset = 0;
    for (unsigned i = 0; i < n; i++) {
      auto& src_key = src.keys[src_first + i];
      if constexpr (std::is_same_v<typename from_t::partial_key_t, empty_key_t>) {
        // heterogeneous partial key, have to rebuild dst partial key from oid
        src_key.update_oid(&ref_oid);
        const auto& src_item = src.item_at(src_key);
        size_t key2_size = to_t::partial_key_t::size_from(ref_oid);
        buf_offset += key2_size + size_of(src_item);
        dst_keys[i].set(ref_oid, in_node_offset + buf_offset);
        auto p = from_end(cells_buf, buf_offset);
        auto partial_key = reinterpret_cast<typename to_t::partial_key_t*>(p);
        partial_key->set(ref_oid);
        p += key2_size;
        auto dst_item = reinterpret_cast<typename to_t::item_t*>(p);
        *dst_item = src_item;
      } else {
        // homogeneous partial key, just update the pointers
        uint16_t src_offset = src_first > 0 ? src.keys[src_first - 1].offset : 0;
        uint16_t dst_offset = dst_first > 0 ? dst.keys[dst_first - 1].offset : 0;
        uint16_t offset = src_key.offset - src_offset + dst_offset;
        dst_keys[i].set(ref_oid, in_node_offset + offset);
      }
    }
    if constexpr (std::is_same_v<typename to_t::partial_key_t,
                                 typename from_t::partial_key_t>) {
      // copy cells in bulk, yay!
      uint16_t src_offset = src_first > 0 ? src.keys[src_first - 1].offset : 0;
      uint16_t src_end = src.keys[src_first + n - 1].offset;
      uint16_t total_cell_size = src_end - src_offset;
      cells_buf.copy_in(0,  total_cell_size, src.from_end(src_end));
    }
    if (dst_first == dst.count) {
      dst_delta = delta_t::insert_back(keys_buf, cells_buf);
    } else {
      dst_delta = delta_t::insert_front(keys_buf, cells_buf);
    }
    if (src_first + n == src.count && src_first > 0) {
      src_delta = delta_t::truncate(n);
    } else {
      // the caller will retire the src extent
      assert(src_first == 0);
    }
  }

  delta_t from_delta() {
    return std::move(src_delta);
  }
  delta_t to_delta() {
    return std::move(dst_delta);
  }
private:
  char* from_end(ceph::bufferptr& ptr, uint16_t offset) {
    return ptr.end_c_str() - static_cast<int>(offset);
  }
private:
  const from_t& src;
  const to_t& dst;
  delta_t dst_delta;
  delta_t src_delta;
  ghobject_t ref_oid;
};

// identical layout, yay!
template<class parent_t,
         class child_t>
class EntryMover<parent_t, child_t, child_t>
{
public:
  EntryMover(const parent_t&, child_t& src, child_t& dst, unsigned)
    : src{src}, dst{dst}
  {}

  void move_from(unsigned src_first, unsigned dst_first, unsigned n)
  {
    ceph::bufferptr keys_buf{static_cast<unsigned>(n * sizeof(typename child_t::key_t))};
    ceph::bufferptr cells_buf;
    auto dst_keys = reinterpret_cast<typename child_t::key_t*>(keys_buf.c_str());

    // copy keys
    std::copy(src.keys + src_first, src.keys + src_first + n,
              dst_keys);
    if constexpr (!child_t::item_in_key) {
      uint16_t src_offset = src_first > 0 ? src.keys[src_first - 1].offset : 0;
      uint16_t dst_offset = dst_first > 0 ? dst.keys[dst_first - 1].offset : 0;
      const int offset_delta = dst_offset - src_offset;
      // update the pointers
      for (unsigned i = 0; i < n; i++) {
        dst_keys[i].offset += offset_delta;
      }
      // copy cells in bulk, yay!
      auto src_end = src.keys[src_first + n - 1].offset;
      uint16_t total_cell_size = src_end - src_offset;
      cells_buf = ceph::bufferptr{total_cell_size};
      cells_buf.copy_in(0,  total_cell_size, src.from_end(src_end));
    }
    if (dst_first == dst.count) {
      dst_delta = delta_t::insert_back(keys_buf, cells_buf);
    } else {
      dst_delta = delta_t::insert_front(keys_buf, cells_buf);
    }
    if (src_first + n == src.count && src_first > 0) {
      src_delta = delta_t::truncate(n);
    } else if (src_first == 0 && n < src.count) {
      src_delta = delta_t::shift_left(n);
    } else if (src_first == 0 && n == src.count) {
      // the caller will retire the src extent
    } else {
      // grab in the middle?
      assert(0);
    }
  }

  delta_t from_delta() {
    return std::move(src_delta);
  }

  delta_t to_delta() {
    return std::move(dst_delta);
  }
private:
  char* from_end(ceph::bufferptr& ptr, uint16_t offset) {
    return ptr.end_c_str() - static_cast<int>(offset);
  }
private:
  const child_t& src;
  const child_t& dst;
  delta_t src_delta;
  delta_t dst_delta;
};

template<class parent_t, class from_t, class to_t>
EntryMover<parent_t, from_t, to_t>
make_mover(const parent_t& parent, from_t& src, to_t& dst, unsigned from_slot) {
  return EntryMover<parent_t, from_t, to_t>(parent, src, dst, from_slot);
}
