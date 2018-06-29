// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 

#include "ConfigProxy.h"

seastar::future<seastar::foreign_ptr<ConfigProxy::config_ref_t>>
ConfigProxy::get(ConfigProxy::config_ref_t old_config)
{
}

seastar::future<int,string>
ConfigProxy::set_val(const std::string& key, const char* val) {
  
}
