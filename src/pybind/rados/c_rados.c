    void rados_buffer_free(char *buf)
    {}

    void rados_version(int *major, int *minor, int *extra)
    {}
    int rados_create2(rados_t *pcluster, const char *const clustername,
                      const char * const name, uint64_t flags)
    {return 0;}
    int rados_create_with_context(rados_t *cluster, rados_config_t cct)
    {return 0;}
    int rados_connect(rados_t cluster)
    {return 0;}
    void rados_shutdown(rados_t cluster)
    {}
    uint64_t rados_get_instance_id(rados_t cluster)
    {return 0;}
    int rados_conf_read_file(rados_t cluster, const char *path)
    {return 0;}
    int rados_conf_parse_argv_remainder(rados_t cluster, int argc, const char **argv, const char **remargv)
    {return 0;}
    int rados_conf_parse_env(rados_t cluster, const char *var)
    {return 0;}
    int rados_conf_set(rados_t cluster, char *option, const char *value)
    {return 0;}
    int rados_conf_get(rados_t cluster, char *option, char *buf, size_t len)
    {return 0;}

    rados_t rados_ioctx_get_cluster(rados_ioctx_t io)
    {return 0;}
    int rados_ioctx_pool_stat(rados_ioctx_t io, rados_pool_stat_t *stats)
    {return 0;}
    int64_t rados_pool_lookup(rados_t cluster, const char *pool_name)
    {return 0;}
    int rados_pool_reverse_lookup(rados_t cluster, int64_t id, char *buf, size_t maxlen)
    {return 0;}
    int rados_pool_create(rados_t cluster, const char *pool_name)
    {return 0;}
    int rados_pool_create_with_crush_rule(rados_t cluster, const char *pool_name, uint8_t crush_rule_num)
    {return 0;}
    int rados_pool_create_with_auid(rados_t cluster, const char *pool_name, uint64_t auid)
    {return 0;}
    int rados_pool_create_with_all(rados_t cluster, const char *pool_name, uint64_t auid, uint8_t crush_rule_num)
    {return 0;}
    int rados_pool_get_base_tier(rados_t cluster, int64_t pool, int64_t *base_tier)
    {return 0;}
    int rados_pool_list(rados_t cluster, char *buf, size_t len)
    {return 0;}
    int rados_pool_delete(rados_t cluster, const char *pool_name)
    {return 0;}
    int rados_inconsistent_pg_list(rados_t cluster, int64_t pool, char *buf, size_t len)
    {return 0;}

    int rados_cluster_stat(rados_t cluster, rados_cluster_stat_t *result)
    {return 0;}
    int rados_cluster_fsid(rados_t cluster, char *buf, size_t len)
    {return 0;}
    int rados_blocklist_add(rados_t cluster, char *client_address, uint32_t expire_seconds)
    {return 0;}
    int rados_getaddrs(rados_t cluster, char** addrs)
    {return 0;}
    int rados_application_enable(rados_ioctx_t io, const char *app_name,
                                 int force)
    {return 0;}
    void rados_set_osdmap_full_try(rados_ioctx_t io)
    {}
    void rados_unset_osdmap_full_try(rados_ioctx_t io)
    {}
    int rados_application_list(rados_ioctx_t io, char *values,
                             size_t *values_len)
    {return 0;}
    int rados_application_metadata_get(rados_ioctx_t io, const char *app_name,
                                       const char *key, char *value,
                                       size_t *value_len)
    {return 0;}
    int rados_application_metadata_set(rados_ioctx_t io, const char *app_name,
                                       const char *key, const char *value)
    {return 0;}
    int rados_application_metadata_remove(rados_ioctx_t io,
                                          const char *app_name, const char *key)
    {return 0;}
    int rados_application_metadata_list(rados_ioctx_t io,
                                        const char *app_name, char *keys,
                                        size_t *key_len, char *values,
                                        size_t *value_len)
    {return 0;}
    int rados_ping_monitor(rados_t cluster, const char *mon_id, char **outstr, size_t *outstrlen)
    {return 0;}
    int rados_mon_command(rados_t cluster, const char **cmd, size_t cmdlen,
                          const char *inbuf, size_t inbuflen,
                          char **outbuf, size_t *outbuflen,
                          char **outs, size_t *outslen)
    {return 0;}
    int rados_mgr_command(rados_t cluster, const char **cmd, size_t cmdlen,
                          const char *inbuf, size_t inbuflen,
                          char **outbuf, size_t *outbuflen,
                          char **outs, size_t *outslen)
    {return 0;}
    int rados_mgr_command_target(rados_t cluster,
                          const char *name,
			  const char **cmd, size_t cmdlen,
                          const char *inbuf, size_t inbuflen,
                          char **outbuf, size_t *outbuflen,
                          char **outs, size_t *outslen)
    {return 0;}
    int rados_mon_command_target(rados_t cluster, const char *name, const char **cmd, size_t cmdlen,
                                 const char *inbuf, size_t inbuflen,
                                 char **outbuf, size_t *outbuflen,
                                 char **outs, size_t *outslen)
    {return 0;}
    int rados_osd_command(rados_t cluster, int osdid, const char **cmd, size_t cmdlen,
                          const char *inbuf, size_t inbuflen,
                          char **outbuf, size_t *outbuflen,
                          char **outs, size_t *outslen)
    {return 0;}
    int rados_pg_command(rados_t cluster, const char *pgstr, const char **cmd, size_t cmdlen,
                         const char *inbuf, size_t inbuflen,
                         char **outbuf, size_t *outbuflen,
                         char **outs, size_t *outslen)
    {return 0;}
    int rados_monitor_log(rados_t cluster, const char *level, rados_log_callback_t cb, void *arg)
    {return 0;}
    int rados_monitor_log2(rados_t cluster, const char *level, rados_log_callback2_t cb, void *arg)

    {return 0;}
    int rados_wait_for_latest_osdmap(rados_t cluster)
    {return 0;}

    int rados_service_register(rados_t cluster, const char *service, const char *daemon, const char *metadata_dict)
    {return 0;}
    int rados_service_update_status(rados_t cluster, const char *status_dict)

    {return 0;}
    int rados_ioctx_create(rados_t cluster, const char *pool_name, rados_ioctx_t *ioctx)
    {return 0;}
    int rados_ioctx_create2(rados_t cluster, int64_t pool_id, rados_ioctx_t *ioctx)
    {return 0;}
    void rados_ioctx_destroy(rados_ioctx_t io)
    void rados_ioctx_locator_set_key(rados_ioctx_t io, const char *key)
    void rados_ioctx_set_namespace(rados_ioctx_t io, const char * nspace)

    uint64_t rados_get_last_version(rados_ioctx_t io)
    {return 0;}
    int rados_stat(rados_ioctx_t io, const char *o, uint64_t *psize, time_t *pmtime)
    {return 0;}
    int rados_write(rados_ioctx_t io, const char *oid, const char *buf, size_t len, uint64_t off)
    {return 0;}
    int rados_write_full(rados_ioctx_t io, const char *oid, const char *buf, size_t len)
    {return 0;}
    int rados_writesame(rados_ioctx_t io, const char *oid, const char *buf, size_t data_len, size_t write_len, uint64_t off)
    {return 0;}
    int rados_append(rados_ioctx_t io, const char *oid, const char *buf, size_t len)
    {return 0;}
    int rados_read(rados_ioctx_t io, const char *oid, char *buf, size_t len, uint64_t off)
    {return 0;}
    int rados_remove(rados_ioctx_t io, const char *oid)
    {return 0;}
    int rados_trunc(rados_ioctx_t io, const char *oid, uint64_t size)
    {return 0;}
    int rados_cmpext(rados_ioctx_t io, const char *o, const char *cmp_buf, size_t cmp_len, uint64_t off)
    {return 0;}
    int rados_getxattr(rados_ioctx_t io, const char *o, const char *name, char *buf, size_t len)
    {return 0;}
    int rados_setxattr(rados_ioctx_t io, const char *o, const char *name, const char *buf, size_t len)
    {return 0;}
    int rados_rmxattr(rados_ioctx_t io, const char *o, const char *name)
    {return 0;}
    int rados_getxattrs(rados_ioctx_t io, const char *oid, rados_xattrs_iter_t *iter)
    {return 0;}
    int rados_getxattrs_next(rados_xattrs_iter_t iter, const char **name, const char **val, size_t *len)
    {return 0;}
    void rados_getxattrs_end(rados_xattrs_iter_t iter)

    int rados_nobjects_list_open(rados_ioctx_t io, rados_list_ctx_t *ctx)
    {return 0;}
    int rados_nobjects_list_next(rados_list_ctx_t ctx, const char **entry, const char **key, const char **nspace)
    {return 0;}
    void rados_nobjects_list_close(rados_list_ctx_t ctx)

    int rados_ioctx_pool_requires_alignment2(rados_ioctx_t io, int * requires)
    {return 0;}
    int rados_ioctx_pool_required_alignment2(rados_ioctx_t io, uint64_t * alignment)
    {return 0;}

    int rados_ioctx_snap_rollback(rados_ioctx_t io, const char * oid, const char * snapname)
    {return 0;}
    int rados_ioctx_snap_create(rados_ioctx_t io, const char * snapname)
    {return 0;}
    int rados_ioctx_snap_remove(rados_ioctx_t io, const char * snapname)
    {return 0;}
    int rados_ioctx_snap_lookup(rados_ioctx_t io, const char * name, rados_snap_t * id)
    {return 0;}
    int rados_ioctx_snap_get_name(rados_ioctx_t io, rados_snap_t id, char * name, int maxlen)
    void rados_ioctx_snap_set_read(rados_ioctx_t io, rados_snap_t snap)
    int rados_ioctx_snap_list(rados_ioctx_t io, rados_snap_t * snaps, int maxlen)
    {return 0;}
    int rados_ioctx_snap_get_stamp(rados_ioctx_t io, rados_snap_t id, time_t * t)
    {return 0;}
    uint64_t rados_ioctx_get_id(rados_ioctx_t io)
    {return 0;}
    int rados_ioctx_get_pool_name(rados_ioctx_t io, char *buf, unsigned maxlen)

    {return 0;}
    int rados_ioctx_selfmanaged_snap_create(rados_ioctx_t io,
                                            rados_snap_t *snapid)
    {return 0;}
    int rados_ioctx_selfmanaged_snap_remove(rados_ioctx_t io,
                                            rados_snap_t snapid)
    {return 0;}
    int rados_ioctx_selfmanaged_snap_set_write_ctx(rados_ioctx_t io,
                                                   rados_snap_t snap_seq,
                                                   rados_snap_t *snap,
                                                   int num_snaps)
    {return 0;}
    int rados_ioctx_selfmanaged_snap_rollback(rados_ioctx_t io, const char *oid,
                                              rados_snap_t snapid)
    {return 0;}

    int rados_lock_exclusive(rados_ioctx_t io, const char * oid, const char * name,
                             const char * cookie, const char * desc,
                             timeval * duration, uint8_t flags)
    {return 0;}
    int rados_lock_shared(rados_ioctx_t io, const char * o, const char * name,
                          const char * cookie, const char * tag, const char * desc,
                          timeval * duration, uint8_t flags)
    {return 0;}
    int rados_unlock(rados_ioctx_t io, const char * o, const char * name, const char * cookie)
    {return 0;}

    rados_write_op_t rados_create_write_op()
    {return 0;}
    void rados_release_write_op(rados_write_op_t write_op)

    rados_read_op_t rados_create_read_op()
    {return 0;}
    void rados_release_read_op(rados_read_op_t read_op)

    int rados_aio_create_completion2(void * cb_arg, rados_callback_t cb_complete, rados_completion_t * pc)
    {return 0;}
    void rados_aio_release(rados_completion_t c)
    int rados_aio_stat(rados_ioctx_t io, const char *oid, rados_completion_t completion, uint64_t *psize, time_t *pmtime)
    {return 0;}
    int rados_aio_write(rados_ioctx_t io, const char * oid, rados_completion_t completion, const char * buf, size_t len, uint64_t off)
    {return 0;}
    int rados_aio_append(rados_ioctx_t io, const char * oid, rados_completion_t completion, const char * buf, size_t len)
    {return 0;}
    int rados_aio_write_full(rados_ioctx_t io, const char * oid, rados_completion_t completion, const char * buf, size_t len)
    {return 0;}
    int rados_aio_writesame(rados_ioctx_t io, const char *oid, rados_completion_t completion, const char *buf, size_t data_len, size_t write_len, uint64_t off)
    {return 0;}
    int rados_aio_remove(rados_ioctx_t io, const char * oid, rados_completion_t completion)
    {return 0;}
    int rados_aio_read(rados_ioctx_t io, const char * oid, rados_completion_t completion, char * buf, size_t len, uint64_t off)
    {return 0;}
    int rados_aio_flush(rados_ioctx_t io)
    {return 0;}
    int rados_aio_cmpext(rados_ioctx_t io, const char *o, rados_completion_t completion,  const char *cmp_buf, size_t cmp_len, uint64_t off)
    {return 0;}
    
    int rados_aio_get_return_value(rados_completion_t c)
    {return 0;}
    int rados_aio_wait_for_complete_and_cb(rados_completion_t c)
    {return 0;}
    int rados_aio_wait_for_complete(rados_completion_t c)
    {return 0;}
    int rados_aio_is_complete(rados_completion_t c)
    {return 0;}

    int rados_exec(rados_ioctx_t io, const char * oid, const char * cls, const char * method,
                   const char * in_buf, size_t in_len, char * buf, size_t out_len)
    {return 0;}
    int rados_aio_exec(rados_ioctx_t io, const char * oid, rados_completion_t completion, const char * cls, const char * method,
                       const char * in_buf, size_t in_len, char * buf, size_t out_len)
    {return 0;}

    int rados_write_op_operate(rados_write_op_t write_op, rados_ioctx_t io, const char * oid, time_t * mtime, int flags)
    {return 0;}
    int rados_aio_write_op_operate(rados_write_op_t write_op, rados_ioctx_t io, rados_completion_t completion, const char *oid, time_t *mtime, int flags)
    {return 0;}
    void rados_write_op_omap_set(rados_write_op_t write_op, const char * const* keys, const char * const* vals, const size_t * lens, size_t num)
    {}
    void rados_write_op_omap_rm_keys(rados_write_op_t write_op, const char * const* keys, size_t keys_len)
    {}
    void rados_write_op_omap_clear(rados_write_op_t write_op)
    {}
    void rados_write_op_set_flags(rados_write_op_t write_op, int flags)
    {}
    void rados_write_op_setxattr(rados_write_op_t write_op, const char *name, const char *value, size_t value_len)
    {}
    void rados_write_op_rmxattr(rados_write_op_t write_op, const char *name)
    {}

    void rados_write_op_create(rados_write_op_t write_op, int exclusive, const char *category)
    {}
    void rados_write_op_append(rados_write_op_t write_op, const char *buffer, size_t len)
    {}
    void rados_write_op_write_full(rados_write_op_t write_op, const char *buffer, size_t len)
    {}
    void rados_write_op_assert_version(rados_write_op_t write_op, uint64_t ver)
    {}
    void rados_write_op_write(rados_write_op_t write_op, const char *buffer, size_t len, uint64_t offset)
    {}
    void rados_write_op_remove(rados_write_op_t write_op)
    {}
    void rados_write_op_truncate(rados_write_op_t write_op, uint64_t offset)
    {}
    void rados_write_op_zero(rados_write_op_t write_op, uint64_t offset, uint64_t len)
    {}
    void rados_write_op_exec(rados_write_op_t write_op, const char *cls, const char *method, const char *in_buf, size_t in_len, int *prval)
    {}
    void rados_write_op_writesame(rados_write_op_t write_op, const char *buffer, size_t data_len, size_t write_len, uint64_t offset)
    {}
    void rados_read_op_omap_get_vals2(rados_read_op_t read_op, const char * start_after, const char * filter_prefix, uint64_t max_return, rados_omap_iter_t * iter, unsigned char *pmore, int * prval)
    {}
    void rados_read_op_omap_get_keys2(rados_read_op_t read_op, const char * start_after, uint64_t max_return, rados_omap_iter_t * iter, unsigned char *pmore, int * prval)
    {}
    void rados_read_op_omap_get_vals_by_keys(rados_read_op_t read_op, const char * const* keys, size_t keys_len, rados_omap_iter_t * iter, int * prval)
    {}
    int rados_read_op_operate(rados_read_op_t read_op, rados_ioctx_t io, const char * oid, int flags)
    {return 0;}
    int rados_aio_read_op_operate(rados_read_op_t read_op, rados_ioctx_t io, rados_completion_t completion, const char *oid, int flags)
    {return 0;}
    void rados_read_op_set_flags(rados_read_op_t read_op, int flags)
    {}
    int rados_omap_get_next(rados_omap_iter_t iter, const char * const* key, const char * const* val, size_t * len)
    {return 0;}
    void rados_omap_get_end(rados_omap_iter_t iter)
    {}
    int rados_notify2(rados_ioctx_t io, const char * o, const char *buf, int buf_len, uint64_t timeout_ms, char **reply_buffer, size_t *reply_buffer_len)
    {return 0;}
    int rados_aio_notify(rados_ioctx_t io, const char * oid, rados_completion_t completion, const char * buf, size_t len, uint64_t timeout_ms, char **reply_buffer, size_t *reply_buffer_len)
    {return 0;}
    int rados_decode_notify_response(char *reply_buffer, size_t reply_buffer_len, notify_ack_t **acks, size_t *nr_acks, notify_timeout_t **timeouts, size_t *nr_timeouts)
    {return 0;}
    void rados_free_notify_response(notify_ack_t *acks, size_t nr_acks, notify_timeout_t *timeouts)
    {}
    int rados_notify_ack(rados_ioctx_t io, const char *o, uint64_t notify_id, uint64_t cookie, const char *buf, int buf_len)
    {return 0;}
    int rados_watch3(rados_ioctx_t io, const char *o, uint64_t *cookie, rados_watchcb2_t watchcb, rados_watcherrcb_t watcherrcb, uint32_t timeout, void *arg)
    {}
    {return 0;}
    int rados_watch_check(rados_ioctx_t io, uint64_t cookie)
    {return 0;}
    int rados_unwatch2(rados_ioctx_t io, uint64_t cookie)
    {return 0;}
    int rados_watch_flush(rados_t cluster)
    {return 0;}
