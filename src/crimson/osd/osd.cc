#include "osd.h"

seastar::future<> OSD::start()
{
  return seastar::now();
}

seastar::future<> OSD::stop()
{
  return gate.close();
}
