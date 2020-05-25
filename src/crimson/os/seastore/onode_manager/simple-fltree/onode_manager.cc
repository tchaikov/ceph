// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "onode_manager.h"

namespace crimson::os::seastore::onode_manager::fltree {

auto OnodeManager::get_or_create_onode(Transaction &trans,
				       const ghobject_t &hoid)
  -> open_ertr::future<OnodeRef>
{
  return tree.find(hoid, trans).handle_error(
    open_ertr::pass_further{},
    crimson::ct_error::assert_all{"TODO"});
}

auto OnodeManager::get_or_create_onodes(Transaction &trans,
					const std::vector<ghobject_t> &hoids)
  -> open_ertr::future<std::vector<OnodeRef>>
{
  return open_ertr::make_ready_future<std::vector<OnodeRef>>();
}

auto OnodeManager::write_dirty(Transaction &trans,
			       const std::vector<OnodeRef> &onodes)
  -> write_ertr::future<>
{
  return write_ertr::now();
}

OnodeManager::OnodeManager(TransactionManager& tm)
  : tree{tm}
{}

OnodeManager::~OnodeManager()
{
}

}
