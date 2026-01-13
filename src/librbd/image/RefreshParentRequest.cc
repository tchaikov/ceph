// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/image/RefreshParentRequest.h"
#include "include/rados/librados.hpp"
#include "common/dout.h"
#include "common/errno.h"
#include "common/WorkQueue.h"
#include "librbd/ImageCtx.h"
#include "librbd/Utils.h"
#include "librbd/RemoteClusterUtils.h"
#include "librbd/internal.h"
#include "librbd/image/CloseRequest.h"
#include "librbd/image/OpenRequest.h"
#include "librbd/io/ObjectDispatcher.h"

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::image::RefreshParentRequest: "

namespace librbd {
namespace image {

using util::create_async_context_callback;
using util::create_context_callback;

template <typename I>
RefreshParentRequest<I>::RefreshParentRequest(
    I &child_image_ctx, const ParentImageInfo &parent_md,
    const MigrationInfo &migration_info, Context *on_finish)
  : m_child_image_ctx(child_image_ctx), m_parent_md(parent_md),
    m_migration_info(migration_info), m_on_finish(on_finish),
    m_parent_image_ctx(nullptr), m_parent_snap_id(CEPH_NOSNAP),
    m_error_result(0) {
}

template <typename I>
bool RefreshParentRequest<I>::is_refresh_required(
    I &child_image_ctx, const ParentImageInfo &parent_md,
    const MigrationInfo &migration_info) {
  ceph_assert(child_image_ctx.snap_lock.is_locked());
  ceph_assert(child_image_ctx.parent_lock.is_locked());
  return (is_open_required(child_image_ctx, parent_md, migration_info) ||
          is_close_required(child_image_ctx, parent_md, migration_info));
}

template <typename I>
bool RefreshParentRequest<I>::is_close_required(
    I &child_image_ctx, const ParentImageInfo &parent_md,
    const MigrationInfo &migration_info) {
  return (child_image_ctx.parent != nullptr &&
          !does_parent_exist(child_image_ctx, parent_md, migration_info));
}

template <typename I>
bool RefreshParentRequest<I>::is_open_required(
    I &child_image_ctx, const ParentImageInfo &parent_md,
    const MigrationInfo &migration_info) {
  return (does_parent_exist(child_image_ctx, parent_md, migration_info) &&
          (child_image_ctx.parent == nullptr ||
           child_image_ctx.parent->md_ctx.get_id() != parent_md.spec.pool_id ||
           child_image_ctx.parent->md_ctx.get_namespace() !=
             parent_md.spec.pool_namespace ||
           child_image_ctx.parent->id != parent_md.spec.image_id ||
           child_image_ctx.parent->snap_id != parent_md.spec.snap_id));
}

template <typename I>
bool RefreshParentRequest<I>::does_parent_exist(
    I &child_image_ctx, const ParentImageInfo &parent_md,
    const MigrationInfo &migration_info) {
  if (child_image_ctx.child != nullptr &&
      child_image_ctx.child->migration_info.empty() && parent_md.overlap == 0) {
    // intermediate, non-migrating images should only open their parent if they
    // overlap
    return false;
  }

  return ((parent_md.spec.pool_id > -1 || !parent_md.spec.pool_name.empty()) &&
          parent_md.overlap > 0) ||
          !migration_info.empty();
}

template <typename I>
void RefreshParentRequest<I>::send() {
  if (is_open_required(m_child_image_ctx, m_parent_md, m_migration_info)) {
    send_open_parent();
  } else {
    // parent will be closed (if necessary) during finalize
    send_complete(0);
  }
}

template <typename I>
void RefreshParentRequest<I>::apply() {
  ceph_assert(m_child_image_ctx.snap_lock.is_wlocked());
  ceph_assert(m_child_image_ctx.parent_lock.is_wlocked());
  std::swap(m_child_image_ctx.parent, m_parent_image_ctx);
}

template <typename I>
void RefreshParentRequest<I>::finalize(Context *on_finish) {
  CephContext *cct = m_child_image_ctx.cct;
  ldout(cct, 10) << this << " " << __func__ << dendl;

  m_on_finish = on_finish;
  if (m_parent_image_ctx != nullptr) {
    send_close_parent();
  } else {
    send_complete(0);
  }
}

template <typename I>
void RefreshParentRequest<I>::send_open_parent() {
  // For remote parents loaded from metadata, pool_id may be -1 (pool_name is used instead)
  // Only assert for local parents
  if (m_parent_md.parent_type != PARENT_TYPE_REMOTE_STANDALONE) {
    ceph_assert(m_parent_md.spec.pool_id >= 0);
  }

  CephContext *cct = m_child_image_ctx.cct;
  ldout(cct, 10) << this << " " << __func__ << dendl;

  librados::IoCtx parent_io_ctx;
  int r;

  // Check if parent is in remote cluster
  if (m_parent_md.parent_type == PARENT_TYPE_REMOTE_STANDALONE) {
    ldout(cct, 10) << "opening remote parent in cluster: "
                   << m_parent_md.remote_cluster_name << dendl;

    // Establish remote cluster connection proactively (not lazily)
    // This avoids slow-start on first I/O that needs the parent
    m_child_image_ctx.remote_parent_cluster.reset(new librados::Rados());

    r = util::connect_to_remote_cluster(
      cct,
      m_parent_md.remote_cluster_name,
      m_parent_md.remote_mon_hosts,
      m_parent_md.remote_keyring,
      "client.admin",
      *m_child_image_ctx.remote_parent_cluster);

    if (r < 0) {
      lderr(cct) << "failed to connect to remote cluster: "
                 << cpp_strerror(r) << dendl;
      m_child_image_ctx.remote_parent_cluster.reset();
      send_complete(r);
      return;
    }

    ldout(cct, 10) << "successfully connected to remote cluster" << dendl;

    // Create IoCtx from remote cluster
    // Use pool name instead of pool ID for remote parents (pool IDs are cluster-specific)
    if (!m_parent_md.spec.pool_name.empty()) {
      r = m_child_image_ctx.remote_parent_cluster->ioctx_create(
        m_parent_md.spec.pool_name.c_str(), parent_io_ctx);
      if (r < 0) {
        lderr(cct) << "failed to create ioctx for remote parent pool '"
                   << m_parent_md.spec.pool_name << "': "
                   << cpp_strerror(r) << dendl;
        // Clean up remote cluster connection to prevent resource leak
        m_child_image_ctx.remote_parent_cluster.reset();
        send_complete(r);
        return;
      }
    } else {
      // Fallback to pool_id if pool_name not available (backward compatibility)
      r = m_child_image_ctx.remote_parent_cluster->ioctx_create2(
        m_parent_md.spec.pool_id, parent_io_ctx);
      if (r < 0) {
        lderr(cct) << "failed to create ioctx for remote parent pool: "
                   << cpp_strerror(r) << dendl;
        // Clean up remote cluster connection to prevent resource leak
        m_child_image_ctx.remote_parent_cluster.reset();
        send_complete(r);
        return;
      }
    }

    if (!m_parent_md.spec.pool_namespace.empty()) {
      parent_io_ctx.set_namespace(m_parent_md.spec.pool_namespace);
    }
  } else {
    // Local parent - use existing code path
    r = util::create_ioctx(m_child_image_ctx.md_ctx, "parent image",
                           m_parent_md.spec.pool_id,
                           m_parent_md.spec.pool_namespace, &parent_io_ctx);
    if (r < 0) {
      send_complete(r);
      return;
    }
  }

  std::string image_name;
  uint64_t flags = 0;
  if (!m_migration_info.empty() && !m_migration_info.image_name.empty()) {
    image_name = m_migration_info.image_name;
    flags |= OPEN_FLAG_OLD_FORMAT;
  }

  m_parent_image_ctx = new I(image_name, m_parent_md.spec.image_id,
                             m_parent_md.spec.snap_id, parent_io_ctx, true);
  m_parent_image_ctx->child = &m_child_image_ctx;

  // set rados flags for reading the parent image
  if (m_child_image_ctx.config.template get_val<bool>("rbd_balance_parent_reads")) {
    m_parent_image_ctx->set_read_flag(librados::OPERATION_BALANCE_READS);
  } else if (m_child_image_ctx.config.template get_val<bool>("rbd_localize_parent_reads")) {
    m_parent_image_ctx->set_read_flag(librados::OPERATION_LOCALIZE_READS);
  }

  using klass = RefreshParentRequest<I>;
  Context *ctx = create_async_context_callback(
    m_child_image_ctx, create_context_callback<
      klass, &klass::handle_open_parent, false>(this));
  OpenRequest<I> *req = OpenRequest<I>::create(m_parent_image_ctx, flags, ctx);
  req->send();
}

template <typename I>
Context *RefreshParentRequest<I>::handle_open_parent(int *result) {
  CephContext *cct = m_child_image_ctx.cct;
  ldout(cct, 10) << this << " " << __func__ << " r=" << *result << dendl;

  save_result(result);
  if (*result < 0) {
    lderr(cct) << "failed to open parent image: " << cpp_strerror(*result)
               << dendl;

    // image already closed by open state machine
    delete m_parent_image_ctx;
    m_parent_image_ctx = nullptr;
  } else {
    // Parent opened successfully - load S3 configuration if available
    load_parent_s3_config();
  }

  return m_on_finish;
}

template <typename I>
void RefreshParentRequest<I>::load_parent_s3_config() {
  ceph_assert(m_parent_image_ctx != nullptr);

  CephContext *cct = m_child_image_ctx.cct;
  ldout(cct, 10) << this << " " << __func__ << dendl;

  // Load S3 configuration from parent image metadata
  // Metadata keys: s3.enabled, s3.bucket, s3.endpoint, s3.region,
  //                s3.access_key, s3.secret_key, s3.prefix,
  //                s3.timeout_ms, s3.max_retries, s3.image_name, s3.image_format

  S3Config& s3_config = m_parent_image_ctx->s3_config;

  // Helper lambda to get metadata value
  auto get_metadata = [this, cct](const std::string& key, std::string& value) -> bool {
    int r = librbd::metadata_get(m_parent_image_ctx, key, &value);
    if (r < 0 && r != -ENOENT) {
      ldout(cct, 5) << "warning: failed to read " << key << ": "
                    << cpp_strerror(r) << dendl;
      return false;
    }
    return (r == 0);
  };

  // Read s3.enabled
  std::string enabled_str;
  if (!get_metadata("s3.enabled", enabled_str)) {
    ldout(cct, 15) << "S3 not configured for parent image" << dendl;
    return;
  }

  s3_config.enabled = (enabled_str == "true" || enabled_str == "1");
  if (!s3_config.enabled) {
    ldout(cct, 15) << "S3 disabled for parent image" << dendl;
    return;
  }

  // Read required fields
  get_metadata("s3.bucket", s3_config.bucket);
  get_metadata("s3.endpoint", s3_config.endpoint);

  // Read optional fields
  get_metadata("s3.region", s3_config.region);
  get_metadata("s3.access_key", s3_config.access_key);

  // Secret key is stored base64-encoded for security
  std::string encoded_secret_key;
  if (get_metadata("s3.secret_key", encoded_secret_key)) {
    // Decode base64-encoded secret key
    bufferlist encoded_bl;
    encoded_bl.append(encoded_secret_key);
    bufferlist decoded_bl;
    try {
      decoded_bl.decode_base64(encoded_bl);
      s3_config.secret_key = decoded_bl.to_str();
    } catch (buffer::error& err) {
      // If we have an access key but failed to decode the secret key,
      // this is a critical error - disable S3 configuration
      if (!s3_config.access_key.empty()) {
        lderr(cct) << "ERROR: failed to decode s3.secret_key for authenticated access - "
                   << "disabling S3 configuration" << dendl;
        s3_config.enabled = false;
        return;
      }
      // Otherwise it's anonymous access - continue without credentials
      ldout(cct, 10) << "note: no valid s3.secret_key, using anonymous access" << dendl;
      s3_config.secret_key = "";
    }
  }

  get_metadata("s3.prefix", s3_config.prefix);
  get_metadata("s3.image_name", s3_config.image_name);
  get_metadata("s3.image_format", s3_config.image_format);

  std::string timeout_str, retries_str;
  if (get_metadata("s3.timeout_ms", timeout_str)) {
    try {
      s3_config.timeout_ms = std::stoul(timeout_str);
    } catch (...) {
      ldout(cct, 5) << "warning: invalid s3.timeout_ms value: " << timeout_str << dendl;
    }
  }

  if (get_metadata("s3.max_retries", retries_str)) {
    try {
      s3_config.max_retries = std::stoul(retries_str);
    } catch (...) {
      ldout(cct, 5) << "warning: invalid s3.max_retries value: " << retries_str << dendl;
    }
  }

  // Validate configuration
  if (s3_config.is_valid()) {
    ldout(cct, 10) << "loaded S3 configuration for parent: "
                   << "bucket=" << s3_config.bucket
                   << ", endpoint=" << s3_config.endpoint
                   << ", prefix=" << s3_config.prefix
                   << ", region=" << s3_config.region
                   << ", access_key=" << s3_config.access_key
                   << ", anonymous=" << s3_config.is_anonymous()
                   << dendl;
  } else {
    ldout(cct, 5) << "warning: incomplete S3 configuration for parent image" << dendl;
    s3_config.enabled = false;
  }
}

template <typename I>
void RefreshParentRequest<I>::send_close_parent() {
  ceph_assert(m_parent_image_ctx != nullptr);

  CephContext *cct = m_child_image_ctx.cct;
  ldout(cct, 10) << this << " " << __func__ << dendl;

  using klass = RefreshParentRequest<I>;
  Context *ctx = create_async_context_callback(
    m_child_image_ctx, create_context_callback<
      klass, &klass::handle_close_parent, false>(this));
  CloseRequest<I> *req = CloseRequest<I>::create(m_parent_image_ctx, ctx);
  req->send();
}

template <typename I>
Context *RefreshParentRequest<I>::handle_close_parent(int *result) {
  CephContext *cct = m_child_image_ctx.cct;
  ldout(cct, 10) << this << " " << __func__ << " r=" << *result << dendl;

  delete m_parent_image_ctx;
  m_parent_image_ctx = nullptr;

  if (*result < 0) {
    lderr(cct) << "failed to close parent image: " << cpp_strerror(*result)
               << dendl;
  }

  send_reset_existence_cache();
  return nullptr;
}

template <typename I>
void RefreshParentRequest<I>::send_reset_existence_cache() {
  CephContext *cct = m_child_image_ctx.cct;
  ldout(cct, 10) << this << " " << __func__ << dendl;

  Context *ctx = create_async_context_callback(
    m_child_image_ctx, create_context_callback<
      RefreshParentRequest<I>,
      &RefreshParentRequest<I>::handle_reset_existence_cache, false>(this));
  m_child_image_ctx.io_object_dispatcher->reset_existence_cache(ctx);
}

template <typename I>
Context *RefreshParentRequest<I>::handle_reset_existence_cache(int *result) {
  CephContext *cct = m_child_image_ctx.cct;
  ldout(cct, 10) << this << " " << __func__ << " r=" << *result << dendl;

  if (*result < 0) {
    lderr(cct) << "failed to reset object existence cache: "
               << cpp_strerror(*result) << dendl;
  }

  if (m_error_result < 0) {
    // propagate errors from opening the image
    *result = m_error_result;
  } else {
    *result = 0;
  }
  return m_on_finish;
}

template <typename I>
void RefreshParentRequest<I>::send_complete(int r) {
  CephContext *cct = m_child_image_ctx.cct;
  ldout(cct, 10) << this << " " << __func__ << dendl;

  m_on_finish->complete(r);
}

} // namespace image
} // namespace librbd

template class librbd::image::RefreshParentRequest<librbd::ImageCtx>;
