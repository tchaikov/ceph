// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 

#include "ConfigProxy.h"

seastar::future<> ConfigProxy::stop()
{
  return seastar::make_ready_future<>();
}

seastar::future<> ConfigProxy::local_set_val(const std::string& key,
					     const char* val)
{
  lw_shared_ptr<ConfigValues> local_new_values =
    get_local_proxy().config_values.set_val(key, val);
  auto foreign_new_values =
    seastar::make_foreign_ptr(local_new_values);
  invoke_on_all([new_values=foreign_new_values](config&)
      config.values = new_values.copy();
  });
}

seastar::sharded<ConfigProxy> ConfigProxy::_the_proxies;
