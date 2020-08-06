// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "compression_onwire.h"
#include "common/dout.h"

#define dout_subsys ceph_subsys_ms

ceph::compression::onwire::rxtx_t ceph::compression::onwire::rxtx_t::create_handler_pair(
    CephContext* ctx, 
    const CompConnectionMeta& comp_meta,
    std::uint64_t compress_min_size) {
    if (comp_meta.is_compress()) {
        CompressorRef compressor = Compressor::create(ctx, comp_meta.get_method());

        if (compressor != nullptr) {
            return {std::make_unique<RxHandler>(ctx, compressor), 
                    std::make_unique<TxHandler>(ctx, compressor, comp_meta.get_mode(), compress_min_size)};
        }
    } 
    
    return {nullptr, nullptr};
}

bool ceph::compression::onwire::TxHandler::compress(const ceph::bufferlist &input, ceph::bufferlist &out) {
    boost::optional<int32_t> compressor_message;
    if (m_init_onwire_size < m_min_size) {
        return false;
    }

    m_compress_potential -= input.length();

    if (input.length() == 0) {
        ldout(m_cct, 15) << __func__ 
            << " discovered an empty segment, skipping compression without aborting"
            << dendl;
        out.clear();
    } else if (0 != m_compressor->compress(input, out, compressor_message)) {
        return false;
    }

    ldout(m_cct, 15) << __func__
                << " uncompressed.length()=" << input.length()
                << " compressed.length()=" << out.length()
                << dendl;

    m_onwire_size += out.length();
    return true;
}

bool ceph::compression::onwire::RxHandler::decompress(const ceph::bufferlist &input, ceph::bufferlist &out) {
    boost::optional<int32_t> compressor_message;
    if (input.length() == 0) {
        ldout(m_cct, 20) << __func__ 
            << " discovered an empty segment, skipping decompression without aborting"
            << dendl;
        out.clear();
        return true;
    }

    if (0 != m_compressor->decompress(input, out, compressor_message)) {
        return false;
    }

    ldout(m_cct, 20) << __func__
        << " compressed.length()=" << input.length()
        << " uncompressed.length()=" << out.length()
        << dendl;

    return true;
}

void ceph::compression::onwire::TxHandler::final() {
    ldout(m_cct, 25) << __func__ << " comprestion ratio=" << get_ratio() << dendl;
}