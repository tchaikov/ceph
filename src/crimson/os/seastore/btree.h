// -*- mode: c++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

using logical_block_id_t uint32_t;
using physical_block_id_t uint32_t;

class Block {
};

enum class NodeOwner : uint8_t {
  ONODE,
  LBA,
};

template<class Key, size_t TargetNodeSize, size_t ValueSize>
class BaseNode {
public:
  bool is_leaf() const {
    return level == 0;
  }
  // Returns the position of the first value whose key is not less than k using
  // binary search performed using compare-to.
  std::pair<uint16_t, bool> lower_bound(const Key& key) const {
    uint_fast16_t low = 0;
    uint_fast16_t high = count;
    while (low < high) {
      uint_fast16_t mid = (low + high) / 2;
      const auto ret = cmp(keys[mid], key);
      if (ret < 0) {
	low = mid + 1;
      } else if (ret > 0) {
	high = mid;
      } else {
	return {mid, true};
      }
    }
    return {low, false};
  }

public:
  // the block in which this node is located
  BlockRef block_ptr;
  NodeOwner owner;
  uint16_t count;
  uint8_t level;
  constexpr static size_type SizeWithNValues(size_type n) {
    return sizeof(BaseBlock) + n * ValueSize;
  }
  constexpr static size_t NrValuesFromSize(const int begin, const int end) {
    return (begin == end ?
	    begin :
	    (SizeWithNValues((begin + end) / 2 + 1) > TargetNodeSize ?
	     NrValuesFromSize(begin, (begin + end) / 2) :
	     NrValuesFromSize((begin + end) / 2 + 1, end)));
  }
  constexpr static size_t NrNodeValues = NrValuesFromSize(0, TargetNodeSize);
  std::array<Key, NrNodeValue> keys;
};

class BlockRef {
public:
  logical_block_id_t get_id() const {
    return id;
  }
  template<class Node>
  const Node& get_node() const {
    return static_cast<const Node&>(*node);
  }
  template<class Node>
  Node& get_node() {
    return static_cast<Node&>(*node);
  }
private:
  seastar::future<BaseNode*> load() {
    return BlockCache::local_cache().load(id);
  }
  logical_block_id_t id;
};

template<class Key, class Value, size_t TargetNodeSize>
class InternalNode : public BaseNode<TargetNodeSize,
				     sizeof(Key) + sizeof(Value)> {
  std::array<BlockRef, NrNodeValues> children;
  const BlockRef get_block(uint16_t i) const {
    return children[i];
  }
};

template<class Key, class Value, size_t TargetNodeSize>
class LeafNode : public BaseNode<TargetNodeSize,
				 sizeof(Key) + sizeof(Value)> {
public:
  const Value& value(uint16_t i) const {
    return values[i];
  }
  // The data is separate from the items to get the keys closer together
  // during searches.
  std::array<Value, NrNodeValues> values;
};

class BTree {
public:
  struct cursor_t {
    LeafNode* node;
    uint16_t index;
  };
  future<BlockRef> insert(const Key& key, Value&& value);
  future<cursor_t> find(const Key& key) {
    return root().then([this](InternalNode* r) {
      return seastar::do_with(BaseNode*{r}, [this] (BaseNode*& node) {
        return seastar::repeat_until_value([this, &node] {
          [[maybe_unused]] auto [where, found] = node->find(key);
          if (node->is_leaf()) {
            auto leaf = static_cast<LeafNode*>(node);
            return seastar::make_ready_future<std::optional<cursor_t>>(leaf,
                                                                       where);
          } else {
            auto inter = static_cast<InternalNode*>(node);
            node = inter->get_block(where).load();
            return seastar::make_ready_future<std::optional<cursor_t>>();
          }
        });
      });
    });
  }
  future<> erase(BlockRef&& block);
private:
};
