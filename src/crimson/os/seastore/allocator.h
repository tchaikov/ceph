// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

namespace crimson::os {

struct block_ref_t {
  segment_id_t id;
  segment_offset_t offset;
};

struct block_owner_t {
  
};
 
class Allocator
{
  btree::btree_map<block_ref_t, block_owner_t> backref_tree;
};

}
