// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab

#include <list>
#include <map>
#include <type_traits>
#include <unordered_map>

template <class Key, class Value, bool Ordered>
class LRUCache {
  static_assert(std::is_default_constructible_v<Value>);
  using list_type = std::list<Key>;

  template<class K, class V>
  using map_t = std::conditional_t<Ordered,
				   std::map<K, V>,
				   std::unordered_map<K, V>>;
  using map_type = map_t<Key, std::pair<Value, typename list_type::iterator>>;

  list_type lru;
  map_type cache;
  const size_t max_size;

public:
  LRUCache(size_t max_size = 20)
    : max_size(max_size)
  {}
  size_t size() const {
    return cache.size();
  }
  size_t capacity() const {
    return max_size;
  }
  using insert_return_type = std::pair<Value, bool>;
  insert_return_type insert(const Key& key, Value&& value) {
    if constexpr(Ordered) {
      auto found = cache.lower_bound(key);
      if (found != cache.end() && found->first == key) {
	// already exists
	return {found->second.first, true};
      } else {
	if (size() > capacity()) {
	  _evict();
	}
	lru.push_front(key);
	// use lower_bound as hint to save the lookup
	auto inserted =
	  cache.emplace_hint(found, key, std::make_pair(std::move(value),
							lru.begin()));
	return {inserted->second->first, false};
      }
    } else {
      // cache is not ordered
      auto found = cache.find(key);
      if (found != cache.end()) {
	// already exists
	return {found->second.first, true};
      } else {
	if (size() > capacity()) {
	  _evict();
	}
	lru.push_front(key);
	auto inserted =
	  cache.emplace(key, std::make_pair(std::move(value), lru.begin()));
	return {inserted->second->first, false};
      }
    }
  }
  Value get(const Key& key) {
    auto found = cache.find(key);
    if (found == cache.end()){
      return {};
    } else {
      return _lru_add(found);
    }
  }
  std::enable_if<Ordered, Value> lower_bound(const Key& key) {
    auto found = cache.lower_bound(key);
    if (found == cache.end()) {
      return {};
    } else {
      return _lru_add(found);
    }
  }
private:
  Value _lru_add(typename map_type::iterator&& found) {
    auto& [value, in_lru] = found->second;
    if (in_lru != lru.begin()){
      // move item to the front
      lru.splice(lru.begin(), lru, in_lru->second);
      return value;
    } else {
      // the item is already at the front
      return value;
    }
  }
  void _evict() {
    // evict the last element of most recently used list
    auto last = --lru.end();
    cache.erase(*last);
    lru.erase(last);
  }
};
