bool ConfigValues::set_value(const std::string& name,
                             Option::value_t&& new_value,
                             int level)
{  
  if (auto p = values.find(opt.name); p != values.end()) {
    auto q = p->second.find(level);
    if (q != p->second.end()) {
      if (new_value == q->second) {
        // no change!
        return false;
      }
      q->second = std::move(new_value);
    } else {
      p->second[level] = std::move(new_value);
    }
    values_bl.clear();
    if (p->second.rbegin()->first > level) {
      // there was a higher priority value; no effect
      return false;
    } else {
      return true;
    }
  } else {
    values_bl.clear();
    values[opt.name][level] = std::move(new_value);
    return true;
  }
}

void ConfigValues::_refresh()
{
  
}

bool LockConfigValues<LockPolicy::MUTEX>::set_value(
    const std::string& name,
    Option::value_t&& value,
    int level)
{
  if (values.set_value(name, std::move(value), level)) {
    
  }
}

seastar::future<> LockConfigValues<LockPolicy::SINGLE>::set_value(
    const std::string& key,
    const char* val)
{
  return ConfigProxy::get_proxy().invoke_on_all([](ConfigProxy& proxy) {
    proxy.set_val(key, val);
  });
}
