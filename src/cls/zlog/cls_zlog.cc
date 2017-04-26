#include <errno.h>
#include <iostream>
#include <boost/lexical_cast.hpp>
#include "include/rados/objclass.h"
#include "zlog.pb.h"

namespace zlog {

  enum {
    CLS_ZLOG_OK            = 0x00,
    CLS_ZLOG_STALE_EPOCH   = 0x01,
    CLS_ZLOG_READ_ONLY     = 0x02,
    CLS_ZLOG_NOT_WRITTEN   = 0x03,
    CLS_ZLOG_INVALIDATED   = 0x04,
    CLS_ZLOG_INVALID_EPOCH = 0x05,
  };

}

/**
 * Helper function to serialize a protobuf object into a bufferlist.
 */
void encode(ceph::buffer::list& bl, google::protobuf::Message& msg) {
  assert(msg.IsInitialized());
  char buf[msg.ByteSize()];
  assert(msg.SerializeToArray(buf, sizeof(buf)));
  bl.append(buf, sizeof(buf));
}

/**
 * Helper function to deserialize a bufferlist in to a protobuf object.
 */
bool decode(ceph::bufferlist& bl, google::protobuf::Message* msg) {
  if (bl.length() == 0) {
    return false;
  }
  if (!msg->ParseFromString(bl.to_str())) {
    std::cerr << "decode: unable to decode message" << std::endl;
    return false;
  }
  if (!msg->IsInitialized()) {
    std::cerr << "decode: message is uninitialized" << std::endl;
    return false;
  }
  return true;
}

CLS_VER(1,0)
CLS_NAME(zlog)

cls_handle_t h_class;

// version 1
cls_method_handle_t h_seal;
cls_method_handle_t h_fill;
cls_method_handle_t h_write;
cls_method_handle_t h_read;
cls_method_handle_t h_trim;
cls_method_handle_t h_max_position;

// version 2
cls_method_handle_t h_seal_v2;
cls_method_handle_t h_write_v2;
cls_method_handle_t h_read_v2;
cls_method_handle_t h_fill_v2;
cls_method_handle_t h_trim_v2;
cls_method_handle_t h_max_position_v2;

// projection management
cls_method_handle_t h_set_projection;
cls_method_handle_t h_get_projection;

/*
 * ZLog Version 1
 *
 *  - Log entries are stored in the omap index
 *  - Each entry contains metadata and entry blob
 */
#define ZLOG_EPOCH_KEY   "____zlog.epoch"
#define ZLOG_MAX_POS_KEY "____zlog.max_position"
#define ZLOG_POS_PREFIX  "____zlog.pos."

using namespace ceph;
using namespace zlog;
using namespace std;

/*
 * ZLog Version 2
 *
 *  - Entry metadata is stored in the omap index
 *  - Entry blob data is stored in bytestream interface
 *  - Epoch and max position are stored in xattr
 *
 * TODO:
 *  - trim can garbage collect data, but it isn't clear how to do that with
 *    object data yet. for now we don't free the space.
 *  - add cls_cxx_append function upstream
 *
 * Large Object Handling
 *
 * When an object becomes too large a new stripe needs to be created.  Each
 * object has a configurable maximum size imposed by Ceph (by default it is
 * 100 GB). When a maximum is hit -EFBIG is returned. We also want to be able
 * to enforce our own size limits, and currently a signed integer is used to
 * encode offsets so there are various limits. In each of these cases we will
 * return -EFBIG and select a reasonable maximum object size that won't reach
 * the offset limits of a 31 bits.
 */
#define MAX_OBJECT_SIZE 1073741824 // 1 GB

/*
 * Project management
 */
// per-epoch key prefix
#define ZLOG_PROJECTION_PREFIX  "____zlog.projection."
// maximum epoch tracked
#define ZLOG_LATEST_PROJECTION_KEY "____zlog.latest_projection"

/*
 * Convert value into zero-padded string for omap comparisons.
 */
static inline std::string u64tostr(uint64_t value)
{
  std::stringstream ss;
  ss << ZLOG_POS_PREFIX << std::setw(20) << std::setfill('0') << value;
  return ss.str();
}
 
/*
 * Convert string into numeric value.
 */
static inline int strtou64(const std::string value, uint64_t *out)
{
  uint64_t v;

  //assert expected prefix

  try {
    std::string value2 = value.substr(strlen(ZLOG_POS_PREFIX));
    v = boost::lexical_cast<uint64_t>(value2);
  } catch (boost::bad_lexical_cast &) {
    CLS_ERR("converting key into numeric value %s", value.c_str());
    return -EIO;
  }
 
  *out = v;
  return 0;
}

static int get_epoch(cls_method_context_t hctx, uint64_t *pepoch)
{
  bufferlist bl;
  int ret = cls_cxx_map_get_val(hctx, ZLOG_EPOCH_KEY, &bl);
  if (ret < 0)
    return ret;

  zlog_proto::GetEpochOp op;
  if (!decode(bl, &op)) {
    CLS_LOG(0, "ERROR: check_epoch(): failed to decode epoch entry");
    return -EIO;
  }

  *pepoch = op.epoch();
  return 0;
}

static int get_epoch_v2(cls_method_context_t hctx, uint64_t *pepoch)
{
  bufferlist bl;
  int ret = cls_cxx_getxattr(hctx, ZLOG_EPOCH_KEY, &bl);
  if (ret < 0) {
    if (ret == -ENODATA)
      ret = -ENOENT;
    return ret;
  }

  zlog_proto::GetEpochOp op;
  if (!decode(bl, &op)) {
    CLS_LOG(0, "ERROR: check_epoch_v2(): failed to decode epoch entry");
    return -EIO;
  }

  *pepoch = op.epoch();
  return 0;
}

static int check_epoch(cls_method_context_t hctx, uint64_t epoch)
{
  uint64_t cur_epoch;
  int ret = get_epoch(hctx, &cur_epoch);
  if (ret < 0 && ret != -ENOENT) {
    CLS_LOG(10, "ERROR: check_epoch(): failed to read epoch (%d)", ret);
    return ret;
  }

  /*
   * Handle initialization case. Effectively this simulates an initial sealed
   * epoch of -1, which is always less than the epoch of an operation which is
   * unsigned. This allows us to avoid the ugly case where initially we create
   * epoch 0 and epoch 1, then immediately seal epoch 0.
   *
   * Note that this implies that even though the target object may not exist,
   * and ZLOG_EPOCH_KEY may not be set, all unit tests must pass as if the
   * real sealed epoch is -1 and that should be taken into account in the unit
   * tests.
   */
  if (ret == -ENOENT) {
    CLS_LOG(0, "NOTICE: treating non-init object as cur_epoch = -1");
    return 0;
  }

  if (epoch <= cur_epoch) {
    CLS_LOG(0, "NOTICE: check_update(): old epoch proposed");
    return CLS_ZLOG_STALE_EPOCH;
  }

  return 0;
}

static int check_epoch_v2(cls_method_context_t hctx, uint64_t epoch)
{
  uint64_t cur_epoch;
  int ret = get_epoch_v2(hctx, &cur_epoch);
  if (ret < 0 && ret != -ENOENT) {
    CLS_LOG(10, "ERROR: check_epoch_v2(): failed to read epoch (%d)", ret);
    return ret;
  }

  /*
   * Handle initialization case. Effectively this simulates an initial sealed
   * epoch of -1, which is always less than the epoch of an operation which is
   * unsigned. This allows us to avoid the ugly case where initially we create
   * epoch 0 and epoch 1, then immediately seal epoch 0.
   *
   * Note that this implies that even though the target object may not exist,
   * and ZLOG_EPOCH_KEY may not be set, all unit tests must pass as if the
   * real sealed epoch is -1 and that should be taken into account in the unit
   * tests.
   */
  if (ret == -ENOENT) {
    CLS_LOG(0, "NOTICE check_epoch_v2(): treating non-init object as cur_epoch = -1");
    return 0;
  }

  if (epoch <= cur_epoch) {
    CLS_LOG(0, "NOTICE: check_update_v2(): old epoch proposed");
    return CLS_ZLOG_STALE_EPOCH;
  }

  return 0;
}

static int __max_position(cls_method_context_t hctx, uint64_t *pposition)
{
  // read max_position from omap
  bufferlist bl;
  int ret = cls_cxx_map_get_val(hctx, ZLOG_MAX_POS_KEY, &bl);
  if (ret < 0) {
    CLS_LOG(10, "NOTICE: __max_position(): failed to read max_position (%d)", ret);
    return ret;
  }

  // decode
  
  // Using MaxPositionRet message here to avoid creating another message
  zlog_proto::MaxPositionRet reply;
  if (!decode(bl, &reply)) {
    CLS_LOG(0, "ERROR: __max_position(): failed to decode max_position entry");
    return -EIO;
  }

  *pposition = reply.pos();
  return 0;
}

static int __max_position_v2(cls_method_context_t hctx, uint64_t *pposition)
{
  // read max_position from xattr
  bufferlist bl;
  int ret = cls_cxx_getxattr(hctx, ZLOG_MAX_POS_KEY, &bl);
  if (ret < 0) {
    CLS_LOG(10, "NOTICE: __max_position_v2(): failed to read max_position (%d)", ret);
    if (ret == -ENODATA)
      ret = -ENOENT;
    return ret;
  }

  // decode
  
  // Using MaxPositionRet message here to avoid creating another message
  zlog_proto::MaxPositionRet content;
  if (!decode(bl, &content)) {
    CLS_LOG(0, "ERROR: __max_position_v2(): failed to decode max_position entry");
    return -EIO;
  }

  *pposition = content.pos();
  return 0;
}

static int read(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  // decode input operation
  zlog_proto::ReadOp op;
  if (!decode(*in, &op)) {
    CLS_LOG(0, "ERROR: read(): failed to decode input");
    return -EINVAL;
  }

  int ret = check_epoch(hctx, op.epoch());
  if (ret) {
    CLS_LOG(10, "NOTICE: read(): stale epoch value");
    return ret;
  }

  // lookup position in omap index
  bufferlist bl;
  std::string key = u64tostr(op.pos());
  ret = cls_cxx_map_get_val(hctx, key, &bl);
  if (ret < 0 && ret != -ENOENT) {
    CLS_LOG(0, "ERROR: read(): failed to read from index");
    return ret;
  }
  // if not found, return NOT_WRITTEN status
  if (ret == -ENOENT)
    return CLS_ZLOG_NOT_WRITTEN;

  // otherwise try to decode the entry
  zlog_proto::LogEntry entry;
  if (!decode(bl, &entry)) {
    CLS_LOG(0, "ERROR: read(): failed to decode index entry");
    return -EIO;
  }

  if (entry.flags() & zlog_proto::LogEntry::INVALIDATED ||
        entry.flags() & zlog_proto::LogEntry::TRIMMED)
      return CLS_ZLOG_INVALIDATED;
  out->append(entry.data());
  return CLS_ZLOG_OK;
}

static int write(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  // decode input operation
  zlog_proto::WriteOp op;
  if (!decode(*in, &op)) {
    CLS_LOG(0, "ERROR: write(): failed to decode input");
    return -EINVAL;
  }
  int ret = check_epoch(hctx, op.epoch());
  if (ret) {
    CLS_LOG(10, "NOTICE: write(): stale epoch value");
    return ret;
  }

  // lookup position in index
  bufferlist bl;
  std::string key = u64tostr(op.pos());
  ret = cls_cxx_map_get_val(hctx, key, &bl);
  if (ret < 0 && ret != -ENOENT) {
    CLS_LOG(0, "ERROR: write(): failed to read index");
    return ret;
  }

  // if position hasn't been written, we'll take it!
  if (ret == -ENOENT) {
    zlog_proto::LogEntry entry;
    entry.set_data(op.data());
    bufferlist entrybl;
    encode(entrybl, entry);
    ret = cls_cxx_map_set_val(hctx, key, &entrybl);
    if (ret < 0) {
      CLS_LOG(0, "ERROR: write(): failed to write index");
      return ret;
    }

    uint64_t cur_max_position = 0;
    ret = __max_position(hctx, &cur_max_position);
    if (ret < 0 && ret != -ENOENT)
      return ret;

    if (ret != -ENOENT)
      assert(op.pos() != cur_max_position);
    // update the max_position? the first test will always update the max_position to the
    // maximum written position. the second test will be true if the max_position
    // hasn't yet been set. note that if the first write is to position 0,
    // then the second condition lets the max_position initialization occur.

    if (op.pos() > cur_max_position || ret == -ENOENT) {
      bufferlist max_positionbl;
      // Using MaxPositionRet message here to avoid creating another message
      zlog_proto::MaxPositionRet reply;
      reply.set_pos(op.pos());
      encode(max_positionbl, reply);

      ret = cls_cxx_map_set_val(hctx, ZLOG_MAX_POS_KEY, &max_positionbl);
      if (ret < 0) {
        CLS_LOG(0, "ERROR: write(): failed to update max_position");
        return ret;
      }
    }

    return CLS_ZLOG_OK;
  }

  /*
   * currently both junk and trimmed entries are kept in the index so this
   * works. with optimization in which ranges of trimmed entries are replaced
   * by a small piece of metadata will require changes here because read-only
   * is returned even though the entry won't be in the index, and hence ret ==
   * -ENOENT as above.
   */
  return CLS_ZLOG_READ_ONLY;
}

static int fill(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  // decode input operation
  zlog_proto::FillOp op;
  if (!decode(*in, &op)) {
    CLS_LOG(0, "ERROR: fill(): failed to decode input");
    return -EINVAL;
  }
  
  int ret = check_epoch(hctx, op.epoch());
  if (ret) {
    CLS_LOG(10, "NOTICE: fill(): stale epoch value");
    return ret;
  }
  // lookup position in the omap index
  bufferlist bl;
  std::string key = u64tostr(op.pos());
  ret = cls_cxx_map_get_val(hctx, key, &bl);
  if (ret < 0 && ret != -ENOENT) {
    CLS_LOG(0, "ERROR: fill(): failed to read index");
    return ret;
  }
  
  zlog_proto::LogEntry entry;
  // if position hasn't been written, invalidate it
  if (ret == -ENOENT) {
    entry.set_flags(static_cast<zlog_proto::LogEntry_Flags>(zlog_proto::LogEntry::DEFAULT | zlog_proto::LogEntry::INVALIDATED));
    entry.set_flags(static_cast<zlog_proto::LogEntry_Flags>(zlog_proto::LogEntry::DEFAULT | zlog_proto::LogEntry::TRIMMED));
    bufferlist entrybl;
    encode(entrybl, entry);
    ret = cls_cxx_map_set_val(hctx, key, &entrybl);
    if (ret < 0) {
      CLS_LOG(0, "ERROR: fill(): failed to write index");
      return ret;
    }
    return CLS_ZLOG_OK;
  }

  // decode the entry from the index
  if (!decode(bl, &entry)) {
    CLS_LOG(0, "ERROR: fill(): failed to decode log entry");
    return -EIO;
  }

  // if it is already invalidated or filled, then report success
  if (entry.flags() & zlog_proto::LogEntry::INVALIDATED ||
        entry.flags() & zlog_proto::LogEntry::TRIMMED)
    return CLS_ZLOG_OK;
  
  return CLS_ZLOG_READ_ONLY;
}

static int trim(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  // decode input operation
  zlog_proto::TrimOp op;
  if (!decode(*in, &op)) {
    CLS_LOG(0, "ERROR: trim(): failed to decode input");
    return -EINVAL;
  }

  int ret = check_epoch(hctx, op.epoch());
  if (ret) {
    CLS_LOG(10, "NOTICE: trim(): stale epoch value");
    return ret;
  }

  // lookup position in the omap index
  bufferlist bl;
  std::string key = u64tostr(op.pos());
  ret = cls_cxx_map_get_val(hctx, key, &bl);
  if (ret < 0 && ret != -ENOENT) {
    CLS_LOG(0, "ERROR: trim(): failed to read index");
    return ret;
  }

  zlog_proto::LogEntry entry;
  // if position hasn't been written, trim it
  if (ret == -ENOENT) {
    entry.set_flags(static_cast<zlog_proto::LogEntry_Flags>(zlog_proto::LogEntry::DEFAULT | zlog_proto::LogEntry::TRIMMED));
    bufferlist entrybl;
    encode(entrybl, entry);
    ret = cls_cxx_map_set_val(hctx, key, &entrybl);
    if (ret < 0) {
      CLS_LOG(0, "ERROR: trim(): failed to write index");
      return ret;
    }
    return CLS_ZLOG_OK;
  }

  // decode the entry from the index
  if (!decode(bl, &entry)) {
    CLS_LOG(0, "ERROR: trim(): failed to decode log entry");
    return -EIO;
  }

  if (entry.flags() & zlog_proto::LogEntry::TRIMMED)
    return CLS_ZLOG_OK;

  // if it exists then set the trim flag and delete the payload.
  entry.clear_data();
  entry.set_flags(static_cast<zlog_proto::LogEntry_Flags>(zlog_proto::LogEntry::DEFAULT | zlog_proto::LogEntry::TRIMMED));  

  bufferlist entrybl;
  encode(entrybl, entry);
  ret = cls_cxx_map_set_val(hctx, key, &entrybl);
  if (ret < 0) {
    CLS_LOG(0, "ERROR: trim(): failed to update index");
    return ret;
  }

  return CLS_ZLOG_OK;
}

static int seal(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  // decode input operation
  zlog_proto::SealOp op;
  if (!decode(*in, &op)) {
    CLS_LOG(0, "ERROR: seal(): failed to decode input");
    return -EINVAL;
  }
  // read the current epoch value (may not yet be set)
  bufferlist bl;
  int ret = cls_cxx_map_get_val(hctx, ZLOG_EPOCH_KEY, &bl);
  if (ret < 0 && ret != -ENOENT) {
    CLS_LOG(10, "NOTICE: seal(): failed to read max_position (%d)", ret);
    return ret;
  }

  // if an epoch exists, verify that the new epoch is larger
  if (ret != -ENOENT) {
    // Using GetEpochOp message
    zlog_proto::GetEpochOp reply;
    if (!decode(bl, &reply)) {
      CLS_LOG(0, "ERROR: seal(): failed to decode epoch entry");
      return -EIO;
    }

    if (op.epoch() <= reply.epoch()) {
      CLS_LOG(0, "ERROR: seal(): epochs move strictly forward");
      return CLS_ZLOG_INVALID_EPOCH;
    }
  }
  
  // Using GetEpochOp message to set new epoch value, to avoid creating another message
  // set new epoch value in omap
  zlog_proto::GetEpochOp content;
  bufferlist epochbl;
  content.set_epoch(op.epoch());
  encode(epochbl, content);
  ret = cls_cxx_map_set_val(hctx, ZLOG_EPOCH_KEY, &epochbl);
  if (ret < 0) {
    CLS_LOG(0, "ERROR: seal(): failed to update epoch");
    return ret;
  }

  return CLS_ZLOG_OK;
}

static int seal_v2(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  // decode input operation
  zlog_proto::SealOp op;
  if(!decode(*in, &op)) {
    CLS_LOG(0, "ERROR: seal_v2(): failed to decode input");
    return -EINVAL;
  }

  // read the current epoch value (may not yet be set)
  bufferlist bl;
  int ret = cls_cxx_getxattr(hctx, ZLOG_EPOCH_KEY, &bl);
  if (ret < 0 && (ret != -ENODATA && ret != -ENOENT)) {
    CLS_LOG(0, "ERROR: seal_v2(): failed to read max_position (%d)", ret);
    return ret;
  }

  if (ret == 0) {
    CLS_ERR("ERROR: seal_v2(): read a zero length epoch value");
    return -EIO;
  }

  // if an epoch exists, verify that the new epoch is larger
  if (ret > 0) {
    // Using GetEpochOp
    zlog_proto::GetEpochOp reply;
    if (!decode(bl, &reply)) {
      CLS_LOG(0, "ERROR: seal_v2(): failed to decode epoch entry");
      return -EIO;
    }

    if (op.epoch() <= reply.epoch()) {
      CLS_LOG(0, "ERROR: seal_v2(): epochs move strictly forward");
      return CLS_ZLOG_INVALID_EPOCH;
    }
  }

  // set new epoch value in omap
  bufferlist epochbl;
  // Using GetEpochOp to set new epoch, to avoid creating a new message
  zlog_proto::GetEpochOp content;
  content.set_epoch(op.epoch());
  encode(epochbl, content);
  ret = cls_cxx_setxattr(hctx, ZLOG_EPOCH_KEY, &epochbl);
  if (ret < 0) {
    CLS_LOG(0, "ERROR: seal_v2(): failed to update epoch: %d", ret);
    return ret;
  }

  return CLS_ZLOG_OK;
}

static int read_v2(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  // decode input operation
  zlog_proto::ReadOp op;
  if (!decode(*in, &op)) {
    CLS_LOG(0, "ERROR: read_v2(): failed to decode input");
    return -EINVAL;
  }

  int ret = check_epoch_v2(hctx, op.epoch());
  if (ret) {
    CLS_LOG(10, "NOTICE: read_v2(): stale epoch value");
    return ret;
  }

  // search omap index for this position
  bufferlist entrybl;
  std::string key = u64tostr(op.pos());
  ret = cls_cxx_map_get_val(hctx, key, &entrybl);
  if (ret < 0 && ret != -ENOENT) {
    CLS_LOG(0, "ERROR: read_v2(): failed to read from index: %d", ret);
    return ret;
  }

  // if position is not found, return NOT_WRITTEN status
  if (ret == -ENOENT)
    return CLS_ZLOG_NOT_WRITTEN;

  // if position is found decode the entry
  zlog_proto::LogEntryV2 entry;
  if (!decode(entrybl, &entry)) {
    CLS_LOG(0, "ERROR: read_v2(): failed to decode index entry");
    return -EIO;
  }

  // check if entry has been filled or invalidated
  
  if (entry.flags() & zlog_proto::LogEntryV2::INVALIDATED ||
      entry.flags() & zlog_proto::LogEntryV2::TRIMMED)
    return CLS_ZLOG_INVALIDATED;

  // read the entry data from object
  ret = cls_cxx_read(hctx, entry.offset(), entry.length(), out);
  if (ret < 0) {
    CLS_ERR("ERROR: read_v2(): failed to read from object: %d", ret);
    return ret;
  }

  /*
   * zlog client expects the whole log entry to be returned. ceph should
   * return the entire entry, but we just add a safety check here. if this
   * becomes an issue we can fix this to be more robust.
   */
  if ((uint64_t)ret != entry.length()) {
    CLS_ERR("ERROR: read_v2(): unexpected read length: %d vs %llu",
        ret, (unsigned long long)entry.length());
    return -EIO;
  }

  return CLS_ZLOG_OK;
}

static int write_v2(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  // decode input operation
  zlog_proto::WriteOp op;
  if (!decode(*in, &op)) {
    CLS_LOG(0, "ERROR: write_v2(): failed to decode input");
    return -EINVAL;
  }

  int ret = check_epoch_v2(hctx, op.epoch());

  if (ret) {
    CLS_LOG(10, "NOTICE: write_v2(): stale epoch value");
    return ret;
  }

  // search omap index for this position
  bufferlist bl;
  std::string key = u64tostr(op.pos());
  ret = cls_cxx_map_get_val(hctx, key, &bl);
  if (ret < 0 && ret != -ENOENT) {
    CLS_LOG(0, "ERROR: write_v2(): failed to read index");
    return ret;
  }

  // if position hasn't been written, we'll take it!
  if (ret == -ENOENT) {
    // offset for write to append to object
    uint64_t object_size;
    ret = cls_cxx_stat(hctx, &object_size, NULL);
    if (ret < 0 && ret != -ENOENT) {
      CLS_ERR("ERROR: write_v2(): stat error: %d", ret);
      return ret;
    }

    if (ret == -ENOENT)
      object_size = 0;

    bufferlist bl_data;
    bl_data.append(op.data());
    if ((object_size + bl_data.length()) > MAX_OBJECT_SIZE) {
      CLS_LOG(10, "NOTICE: write_v2(): maximum object size reached");
      return -EFBIG;
    }

    // append entry data to object

    ret = cls_cxx_write(hctx, object_size, bl_data.length(), &bl_data);
    if (ret) {
      CLS_ERR("ERROR: write_v2(): write error: %d", ret);
      return ret;
    }

    // write position metadata into omap index
    zlog_proto::LogEntryV2 entry;
    entry.set_offset(object_size);
    entry.set_length(bl_data.length());
    
    bufferlist entrybl;
    encode(entrybl, entry);
    ret = cls_cxx_map_set_val(hctx, key, &entrybl);
    if (ret < 0) {
      CLS_LOG(0, "ERROR: write_v2(): failed to write index: %d", ret);
      return ret;
    }

    uint64_t cur_max_position = 0;
    ret = __max_position_v2(hctx, &cur_max_position);
    if (ret < 0 && ret != -ENOENT)
      return ret;

    if (ret != -ENOENT)
      assert(op.pos() != cur_max_position);

    // update the max_position? the first test will always update the max_position to the
    // maximum written position. the second test will be true if the max_position
    // hasn't yet been set. note that if the first write is to position 0,
    // then the second condition lets the max_position initialization occur.
    
    if (op.pos() > cur_max_position || ret == -ENOENT) {
      bufferlist max_positionbl;
      // Using MaxPositionRet to set position 
      zlog_proto::MaxPositionRet content;
      content.set_pos(op.pos());
      encode(max_positionbl, content);
      ret = cls_cxx_setxattr(hctx, ZLOG_MAX_POS_KEY, &max_positionbl);
      if (ret < 0) {
        CLS_LOG(0, "ERROR: write_v2(): failed to update max_position: %d", ret);
        return ret;
      }
    }

    return CLS_ZLOG_OK;
  }

  /*
   * currently both junk and trimmed entries are kept in the index so this
   * works. with optimization in which ranges of trimmed entries are replaced
   * by a small piece of metadata will require changes here because read-only
   * is returned even though the entry won't be in the index, and hence ret ==
   * -ENOENT as above.
   */
  return CLS_ZLOG_READ_ONLY;
}

static int fill_v2(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  // decode input operation
  zlog_proto::FillOp op;
  if (!decode(*in, &op)) {
    CLS_LOG(0, "ERROR: fill_v2(): failed to decode input");
    return -EINVAL;
  }
  int ret = check_epoch_v2(hctx, op.epoch());
  if (ret) {
    CLS_LOG(10, "NOTICE: fill_v2(): stale epoch value: %d", ret);
    return ret;
  }

  // search for position in the omap index
  bufferlist bl;
  std::string key = u64tostr(op.pos());
  ret = cls_cxx_map_get_val(hctx, key, &bl);
  if (ret < 0 && ret != -ENOENT) {
    CLS_LOG(0, "ERROR: fill_v2(): failed to read index");
    return ret;
  }

  zlog_proto::LogEntryV2 entry;

  // if position hasn't been written, invalidate it
  if (ret == -ENOENT) {
    entry.set_flags(static_cast<zlog_proto::LogEntryV2_Flags>(zlog_proto::LogEntryV2::DEFAULT| zlog_proto::LogEntryV2::INVALIDATED));
    entry.set_flags(static_cast<zlog_proto::LogEntryV2_Flags>(zlog_proto::LogEntryV2::DEFAULT| zlog_proto::LogEntryV2::TRIMMED));
    bufferlist entrybl;
    encode(entrybl, entry);
    ret = cls_cxx_map_set_val(hctx, key, &entrybl);
    if (ret < 0) {
      CLS_LOG(0, "ERROR: fill_v2(): failed to write index: %d", ret);
      return ret;
    }
    return CLS_ZLOG_OK;
  }

  // decode the entry from the index
  if (!decode(bl, &entry)) {
    CLS_LOG(0, "ERROR: fill_v2(): failed to decode log entry");
    return -EIO;
  }
  // if it is already invalidated or filled, then report success
  if (entry.flags() & zlog_proto::LogEntryV2::INVALIDATED ||
        entry.flags() & zlog_proto::LogEntryV2::TRIMMED)  
      return CLS_ZLOG_OK;

  return CLS_ZLOG_READ_ONLY;
}

static int trim_v2(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  // decode input operation
  zlog_proto::TrimOp op;
  if (!decode(*in, &op)) {
    CLS_LOG(0, "ERROR: trim_v2(): failed to decode input");
    return -EINVAL;
  }

  int ret = check_epoch_v2(hctx, op.epoch());
  if (ret) {
    CLS_LOG(10, "NOTICE: trim_v2(): stale epoch value: %d", ret);
    return ret;
  }

  // search for position in the omap index
  bufferlist bl;
  std::string key = u64tostr(op.pos());
  ret = cls_cxx_map_get_val(hctx, key, &bl);
  if (ret < 0 && ret != -ENOENT) {
    CLS_LOG(0, "ERROR: trim_v2(): failed to read index: %d", ret);
    return ret;
  }

  zlog_proto::LogEntryV2 entry;

  // if position hasn't been written, trim it
  if (ret == -ENOENT) {
    entry.set_flags(static_cast<zlog_proto::LogEntryV2_Flags>(zlog_proto::LogEntryV2::DEFAULT | zlog_proto::LogEntryV2::TRIMMED));
    bufferlist entrybl;
    encode(entrybl, entry);
    ret = cls_cxx_map_set_val(hctx, key, &entrybl);
    if (ret < 0) {
      CLS_LOG(0, "ERROR: trim_v2(): failed to write index: %d", ret);
      return ret;
    }
    return CLS_ZLOG_OK;
  }

  // decode the entry from the index
  if (!decode(bl, &entry)) {
    CLS_LOG(0, "ERROR: trim_v2(): failed to decode log entry");
    return -EIO;
  }

  if (entry.flags() & zlog_proto::LogEntryV2::TRIMMED)
    return CLS_ZLOG_OK;

  // if it exists then set the trim flag
  entry.set_flags(static_cast<zlog_proto::LogEntryV2_Flags>(zlog_proto::LogEntryV2::DEFAULT | zlog_proto::LogEntryV2::TRIMMED));
  // TODO: entry space would be reclaimed now, or periodically
  // reclaimed by looking for entries with the trim flag.

  bufferlist entrybl;
  encode(entrybl, entry);
  ret = cls_cxx_map_set_val(hctx, key, &entrybl);
  if (ret < 0) {
    CLS_LOG(0, "ERROR: trim_v2(): failed to update index: %d", ret);
    return ret;
  }

  return CLS_ZLOG_OK;
}

/*
 * Ideally seal() would return the maximum position written to, but rados
 * object classes currently prevent this because data can't be returned after
 * an object update. Instead we enforce epoch equality, and require clients
 * that seal() to make another pass using the same epoch.
 *
 * The exact semantics are that this method returns the next position in the
 * log assuming a single object is being striped across. This is useful
 * because 0 indicates no writes and we don't need an extra flag to describe
 * the state of the object being empty.
 */
static int max_position_v2(cls_method_context_t hctx, bufferlist *in,
    bufferlist *out)
{
  zlog_proto::MaxPositionOp op;
  if (!decode(*in, &op)) {
    CLS_LOG(0, "ERROR: max_position_v2(): failed to decode input");
    return -EINVAL;
  }

  uint64_t cur_epoch;
  int ret = get_epoch_v2(hctx, &cur_epoch);
  if (ret < 0) {
    CLS_LOG(0, "ERROR: max_position_v2(): could not retrieve epoch: %d", ret);
    return ret;
  }

  if (op.epoch() != cur_epoch) {
    CLS_LOG(0, "ERROR: max_position_v2(): invalid epoch");
    return -EINVAL;
  }

  uint64_t position;
  ret = __max_position_v2(hctx, &position);
  if (ret < 0 && ret != -ENOENT)
    return ret;
  
  zlog_proto::MaxPositionRet reply;
  if (ret == -ENOENT)
    reply.set_pos(0);
  else
    reply.set_pos(position + 1);
  
  encode(*out, reply);

  return CLS_ZLOG_OK;
}

/*
 * Ideally seal() would return the maximum position written to, but rados
 * object classes currently prevent this because data can't be returned after
 * an object update. Instead we enforce epoch equality, and require clients
 * that seal() to make another pass using the same epoch.
 *
 * The exact semantics are that this method returns the next position in the
 * log assuming a single object is being striped across. This is useful
 * because 0 indicates no writes and we don't need an extra flag to describe
 * the state of the object being empty.
 */
static int max_position(cls_method_context_t hctx, bufferlist *in,
    bufferlist *out)
{
  zlog_proto::MaxPositionOp op;
  if (!decode(*in, &op)) {
    CLS_LOG(0, "ERROR: max_position(): failed to decode input");
    return -EINVAL;
  }

  uint64_t cur_epoch;
  int ret = get_epoch(hctx, &cur_epoch);
  if (ret < 0) {
    CLS_LOG(0, "ERROR: max_position(): could not retrieve epoch");
    return ret;
  }

  if (op.epoch() != cur_epoch) {
    CLS_LOG(0, "ERROR: max_position(): invalid epoch");
    return -EINVAL;
  }

  uint64_t position;
  ret = __max_position(hctx, &position);
  if (ret < 0 && ret != -ENOENT)
    return ret;

  zlog_proto::MaxPositionRet reply;
  if (ret == -ENOENT)
    reply.set_pos(0);
  else
    reply.set_pos(position + 1);

  encode(*out, reply);

  return CLS_ZLOG_OK;
}

static int __set_latest_projection(cls_method_context_t hctx,
    uint64_t epoch)
{
  bufferlist bl;
  zlog_proto::SetLatestProjectionOp op;
  op.set_epoch(epoch);
  encode(bl, op);

  int ret = cls_cxx_map_set_val(hctx, ZLOG_LATEST_PROJECTION_KEY, &bl);
  if (ret < 0) {
    CLS_LOG(0, "ERROR: could not set latest projection %llu",
        (unsigned long long)epoch);
    return ret;
  }

  return 0;
}

static int __get_latest_projection(cls_method_context_t hctx,
    uint64_t *pepoch)
{
  bufferlist bl;
  int ret = cls_cxx_map_get_val(hctx, ZLOG_LATEST_PROJECTION_KEY, &bl);
  if (ret < 0) {
    CLS_LOG(0, "__get_latest_projection: failed to get map val");
    return ret;
  }
  
  zlog_proto::GetLatestProjectionOp op;
  if (!decode(bl, &op)) {
    CLS_LOG(0, "ERROR: __get_latest_projection: cannot decode");
    return -EIO;
  }

  *pepoch = op.epoch();
  return 0;
}

static int __get_projection(cls_method_context_t hctx, bufferlist *out,
    uint64_t epoch)
{
  // build projection key: prefix.epoch
  std::stringstream key;
  key << ZLOG_PROJECTION_PREFIX << epoch;

  // read key from omap
  bufferlist bl;
  int ret = cls_cxx_map_get_val(hctx, key.str(), &bl);
  if (ret < 0)
    return ret;

  if (out)
    *out = bl;

  return 0;
}

static int __set_projection(cls_method_context_t hctx, uint64_t epoch, bufferlist *bl)
{
  // projections are write-once. we return if a projection already exists at
  // this epoch, or there is an error other than -ENOENT.
  int ret = __get_projection(hctx, NULL, epoch);
  if (ret == 0) {
    CLS_LOG(0, "ERROR: __set_projection: projection already set %llu",
        (unsigned long long)epoch);
    return -EINVAL;
  } else if (ret != -ENOENT) {
    CLS_LOG(0, "ERROR: __set_projection: could not get projection %llu",
        (unsigned long long)epoch);
    return ret;
  }
  assert(ret == -ENOENT);

  // build projection key: prefix.epoch
  stringstream key;
  key << ZLOG_PROJECTION_PREFIX << epoch;

  // set the projection
  ret = cls_cxx_map_set_val(hctx, key.str(), bl);
  if (ret < 0) {
    CLS_LOG(0, "ERROR: __set_projction(): could not set projection");
    return ret;
  }

  return 0;
}

static int set_projection(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  zlog_proto::SetProjectionOp op;
  if(!decode(*in, &op)) {
    CLS_LOG(0, "ERROR: set_projection: failed to decode input");
    return -EINVAL;
  }

  // get the latest projection, if one exists
  uint64_t epoch;
  int ret = __get_latest_projection(hctx, &epoch);
  if (ret && ret != -ENOENT) {
    CLS_LOG(0, "ERROR: set_projection: error finding latest epoch %d", ret);
    return ret;
  }
  // if not projection exists, then the first should be zero
  if (ret == -ENOENT && op.epoch() != 0) {
    CLS_LOG(0, "ERROR: set_projection: first epoch must be zero %llu given",
        (unsigned long long)epoch);
    return -EINVAL;
  }
  // if a projection does exist, then we should be setting curr_proj + 1
  if (ret != -ENOENT && op.epoch() != (epoch + 1)) {
    CLS_LOG(0, "ERROR: set_projection: new epoch must be curr+1 curr " \
        "%llu given %llu", (unsigned long long)op.epoch(),
        (unsigned long long)epoch);
    return -EINVAL;
  }
  
  bufferlist bl;
  bl.append(op.data());
  ret = __set_projection(hctx, op.epoch(), &bl);
  if (ret < 0) {
    CLS_LOG(0, "ERROR: set_projection(): failed to set projection");
    return ret;
  }
  ret = __set_latest_projection(hctx, op.epoch());
  if (ret < 0) {
    CLS_LOG(0, "ERROR: set_projection(): failed to set latest projection");
    return ret;
  }
  return 0;
}

static int get_projection(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  zlog_proto::GetProjectionOp op;
  if (!decode(*in, &op)) {
    CLS_LOG(0, "ERROR: get_projection: failed to decode input");
    return -EINVAL;
  }

  // use specified epoch, unless latest is requested
  uint64_t epoch = op.epoch();
  if (op.latest()) {
    int ret = __get_latest_projection(hctx, &epoch);
    if (ret) {
      CLS_LOG(0, "ERROR: get_projection: error finding latest epoch %d", ret);
      return ret;
    }
  }

  // read the projection blob at the given epoch
  bufferlist bl;
  int ret = __get_projection(hctx, &bl, epoch);
  if (ret < 0) {
    CLS_LOG(0, "ERROR: get_projection: could not find epoch %llu ret %d",
        (unsigned long long)epoch, ret);
    return ret;
  }

  zlog_proto::GetProjectionRet reply;
  reply.set_epoch(epoch);
  reply.set_out(bl.to_str());
  encode(*out, reply);

  return 0;
}

void __cls_init()
{
  CLS_LOG(0, "loading cls_zlog");

  cls_register("zlog", &h_class);

  // version 1

  cls_register_cxx_method(h_class, "seal",
      CLS_METHOD_RD | CLS_METHOD_WR,
      seal, &h_seal);

  cls_register_cxx_method(h_class, "fill",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  fill, &h_fill);

  cls_register_cxx_method(h_class, "write",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  write, &h_write);

  cls_register_cxx_method(h_class, "read",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  read, &h_read);

  cls_register_cxx_method(h_class, "trim",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  trim, &h_trim);

  cls_register_cxx_method(h_class, "max_position",
			  CLS_METHOD_RD,
			  max_position, &h_max_position);

  // version 2

  cls_register_cxx_method(h_class, "seal_v2",
      CLS_METHOD_RD | CLS_METHOD_WR,
      seal_v2, &h_seal_v2);

  cls_register_cxx_method(h_class, "write_v2",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  write_v2, &h_write_v2);

  cls_register_cxx_method(h_class, "read_v2",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  read_v2, &h_read_v2);

  cls_register_cxx_method(h_class, "fill_v2",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  fill_v2, &h_fill_v2);

  cls_register_cxx_method(h_class, "trim_v2",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  trim_v2, &h_trim_v2);

  cls_register_cxx_method(h_class, "max_position_v2",
			  CLS_METHOD_RD,
			  max_position_v2, &h_max_position_v2);

  // projection management

  cls_register_cxx_method(h_class, "get_projection",
			  CLS_METHOD_RD,
			  get_projection, &h_get_projection);

  cls_register_cxx_method(h_class, "set_projection",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  set_projection, &h_set_projection);
}
