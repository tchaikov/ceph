// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

namespace crimson::os {

// each delta describes a logical mutation of one or more blocks
struct delta_t {
  uint8_t op;
};

class Record {
  struct header_t {
    uint64_t len;
    // shall we support different checksum algorithms?
    uint32_t csum;
  }
  std::vector<delta_t> ops;
  std::vector<block_t> blocks;
};

}

  
