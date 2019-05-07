// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <string>
#include <unordered_map>
#include <map>
#include <typeinfo>
#include <vector>

#include <seastar/core/future.hh>

#include "osd/osd_types.h"
#include "include/uuid.h"

namespace ceph::os {

class Collection;
class Transaction;

// a just-enough store for reading/writing the superblock
class CyanStore {
  using CollectionRef = boost::intrusive_ptr<Collection>;
  const std::string path;
  std::unordered_map<coll_t, CollectionRef> coll_map;
  std::map<coll_t,CollectionRef> new_coll_map;
  uint64_t used_bytes = 0;
  uuid_d osd_fsid;

public:
  template <class ConcreteExceptionT>
  class Exception : public std::logic_error {
  public:
    using std::logic_error::logic_error;

    // Throwing an exception isn't the sole way to signalize an error
    // with it. This approach nicely fits cold, infrequent issues but
    // when applied to a hot one (like ENOENT on write path), it will
    // likely hurt performance.
    // Alternative approach for hot errors is to create exception_ptr
    // on our own and place it in the future via make_exception_future.
    // When ::handle_exception is called, handler would inspect stored
    // exception whether it's hot-or-cold before rethrowing it.
    // The main advantage is both types flow through very similar path
    // based on future::handle_exception.
    static bool is_class_of(const std::exception_ptr& ep) {
      // Seastar offers hacks for making throwing lock-less but stack
      // unwinding still can be a problem so painful to justify going
      // with non-standard, obscure things like this one.
      return *ep.__cxa_exception_type() == typeid(ConcreteExceptionT);
    }
  };

  struct EnoentException : public Exception<EnoentException> {
    using Exception<EnoentException>::Exception;
  };

  CyanStore(const std::string& path);
  ~CyanStore();

  seastar::future<> mount();
  seastar::future<> umount();

  seastar::future<> mkfs();
  seastar::future<bufferlist> read(CollectionRef c,
				   const ghobject_t& oid,
				   uint64_t offset,
				   size_t len,
				   uint32_t op_flags = 0);
  seastar::future<ceph::bufferptr> get_attr(CollectionRef c,
					    const ghobject_t& oid,
					    std::string_view name);
  using attrs_t = std::map<std::string, ceph::bufferptr, std::less<>>;
  seastar::future<attrs_t> get_attrs(CollectionRef c, const ghobject_t& oid);
  using omap_values_t = std::map<std::string,bufferlist, std::less<>>;
  seastar::future<omap_values_t> omap_get_values(
    CollectionRef c,
    const ghobject_t& oid,
    std::vector<std::string>&& keys);
  CollectionRef create_new_collection(const coll_t& cid);
  CollectionRef open_collection(const coll_t& cid);
  std::vector<coll_t> list_collections();

  seastar::future<> do_transaction(CollectionRef ch,
				   Transaction&& txn);

  void write_meta(const std::string& key,
		  const std::string& value);
  int read_meta(const std::string& key, std::string* value);
  uuid_d get_fsid() const;

private:
  int _touch(const coll_t& cid, const ghobject_t& oid);
  int _write(const coll_t& cid, const ghobject_t& oid,
	     uint64_t offset, size_t len, const bufferlist& bl,
	     uint32_t fadvise_flags);
  int _truncate(const coll_t& cid, const ghobject_t& oid, uint64_t size);
  int _setattrs(const coll_t& cid, const ghobject_t& oid,
                map<string,bufferptr>& aset);
  int _create_collection(const coll_t& cid, int bits);
};

}
