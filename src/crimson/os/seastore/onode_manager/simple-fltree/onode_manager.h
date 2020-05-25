// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <sys/mman.h>
#include <string.h>

#include <memory>
#include <string.h>

#include "crimson/common/log.h"
#include "crimson/os/seastore/onode_manager.h"
#include "crimson/os/seastore/onode_manager/simple-fltree/onode_tree.h"

namespace crimson::os::seastore::onode_manager::fltree {

class OnodeManager final : public crimson::os::seastore::OnodeManager {
public:
  open_ertr::future<OnodeRef>
  get_or_create_onode(Transaction &trans,
		      const ghobject_t &hoid) final;
  open_ertr::future<std::vector<OnodeRef>>
  get_or_create_onodes(Transaction &trans,
		       const std::vector<ghobject_t> &hoids) final;
  write_ertr::future<> write_dirty(Transaction &trans,
				   const std::vector<OnodeRef> &onodes) final;

  OnodeManager(TransactionManager& tm);
  ~OnodeManager() final;

private:
  Btree tree;
};

}
