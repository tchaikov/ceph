# -*- coding: utf-8 -*-
"""Gateway commands.

Each function is a "ceph nvmeof ..." CLI command and doubles as the
implementation behind the dashboard's REST endpoints, which reach it
through NVMeoF.api_call(). The first argument is the nvmeof module
instance, which supplies the registry and mTLS lookups for the grpc
client.
"""
import threading
from functools import partial
from typing import Dict, List, Optional, Tuple

from . import model
from .cli import NvmeofCLICommand
from .client import NVMeoFClient, handle_nvmeof_error
from .converters import convert_to_model, namedtuple_to_dict
from .errors import NvmeofInvalidInputError
from .utils import convert_to_bytes, escape_address_if_ipv6, \
    format_host_updates, resolve_nvmeof_server_address, str_to_bool


# Side channel for REST-only semantics that must not leak HTTP concepts
# into this module: namespace_update flags a partial failure here, and
# NVMeoF.api_call() forwards it to the dashboard, which turns it into a
# 202 response. The flag lives in a thread local because api_call and
# the handler run on the same thread.
_thread_ctx = threading.local()


def reset_partial_failure() -> None:
    _thread_ctx.partial_failure = False


def partial_failure_flagged() -> bool:
    return getattr(_thread_ctx, 'partial_failure', False)


def _flag_partial_failure() -> None:
    _thread_ctx.partial_failure = True


def _normalize_enum_key(val):
    return val.replace("_", " ").title()


def _update_connections(connection_list_resp):
    conns = connection_list_resp.get('connections')
    if not conns:
        conns = []
    for con in conns:
        orig = con.get("dhchap_controller_origin")
        if orig:
            con["dhchap_controller_origin"] = _normalize_enum_key(orig)
    return connection_list_resp


@NvmeofCLICommand("nvmeof connection list", model.ConnectionList)
@convert_to_model(model.ConnectionList, finalize=_update_connections)
@handle_nvmeof_error
def connection_list(mgr, nqn: Optional[str] = None,
                    gw_group: Optional[str] = None,
                    server_address: Optional[str] = None,
                    traddr: Optional[str] = None):
    """List all NVMeoF Subsystem Connections"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    if not nqn:
        nqn = '*'
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.list_connections(
        NVMeoFClient.pb2.list_connections_req(subsystem=nqn)
    )


@NvmeofCLICommand(
    "nvmeof connection get_io_statistics",
    model.ConnectionIOStatistics,
    success_message_template="Please use JSON format to see the statistics"
)
@convert_to_model(model.ConnectionIOStatistics)
@handle_nvmeof_error
def connection_get_io_statistics(mgr, nqn: str, host_nqn: str,
                                 gw_group: Optional[str] = None,
                                 server_address: Optional[str] = None,
                                 traddr: Optional[str] = None):
    """Get the IO statistics for a connection"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.get_connection_io_statistics(
        NVMeoFClient.pb2.get_connection_io_statistics_req(subsystem_nqn=nqn,
                                                          host_nqn=host_nqn,
                                                          reset=False)
    )


@NvmeofCLICommand(
    "nvmeof connection reset_io_statistics",
    model.ConnectionIOStatistics,
    success_message_template=(
        "Resetting host's {host_nqn} in {nqn} IO statistics: Successful"
    )
)
@convert_to_model(model.ConnectionIOStatistics)
@handle_nvmeof_error
def connection_reset_io_statistics(mgr, nqn: str, host_nqn: str,
                                   gw_group: Optional[str] = None,
                                   server_address: Optional[str] = None,
                                   traddr: Optional[str] = None):
    """Reset the IO statistics for a connection"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.get_connection_io_statistics(
        NVMeoFClient.pb2.get_connection_io_statistics_req(subsystem_nqn=nqn,
                                                          host_nqn=host_nqn,
                                                          reset=True)
    )


def _update_hosts(hosts_info_resp):
    if hosts_info_resp.get('allow_any_host'):
        hosts_info_resp['hosts'].insert(0, {"nqn": "*"})
    hosts = hosts_info_resp.get('hosts')
    if not hosts:
        hosts = []
    for h in hosts:
        orig = h.get("dhchap_controller_origin")
        if orig:
            h["dhchap_controller_origin"] = _normalize_enum_key(orig)
    return hosts_info_resp


@NvmeofCLICommand("nvmeof host list", model.HostsInfo)
@convert_to_model(model.HostsInfo, finalize=_update_hosts)
@handle_nvmeof_error
def host_list(mgr, nqn: str, gw_group: Optional[str] = None,
              server_address: Optional[str] = None,
              traddr: Optional[str] = None):
    """List all allowed hosts for an NVMeoF subsystem"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.list_hosts(
        NVMeoFClient.pb2.list_hosts_req(subsystem=nqn, clear_alerts=False)
    )


@NvmeofCLICommand(
    "nvmeof host add",
    model.RequestStatus,
    success_message_fn=partial(
        format_host_updates,
        template_wildcard="Allowing open host access to {nqn}: Successful",
        template_item="Adding host {host_nqn} to {nqn}: Successful",
    ),
)
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def host_add(mgr, nqn: str, host_nqn: str, dhchap_key: Optional[str] = None,
             dhchap_controller_key: Optional[str] = None,
             psk: Optional[str] = None, gw_group: Optional[str] = None,
             server_address: Optional[str] = None,
             traddr: Optional[str] = None):
    """Allow hosts to access an NVMeoF subsystem"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.add_host(
        NVMeoFClient.pb2.add_host_req(
            subsystem_nqn=nqn, host_nqn=host_nqn,
            dhchap_key=dhchap_key,
            dhchap_ctrlr_key=dhchap_controller_key,
            psk=psk)
    )


@NvmeofCLICommand(
    "nvmeof host del",
    model.RequestStatus,
    success_message_fn=partial(
        format_host_updates,
        template_wildcard="Disabling open host access to {nqn}: Successful",
        template_item="Removing host {host_nqn} access from {nqn}: Successful",
    ),
)
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def host_del(mgr, nqn: str, host_nqn: str, force: Optional[bool] = False,
             gw_group: Optional[str] = None,
             server_address: Optional[str] = None,
             traddr: Optional[str] = None,
             keep_connections: Optional[bool] = False):
    """Disallow hosts from accessing an NVMeoF subsystem"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.remove_host(
        NVMeoFClient.pb2.remove_host_req(subsystem_nqn=nqn, host_nqn=host_nqn,
                                         force=force,
                                         keep_connections=keep_connections)
    )


def _host_change_key(mgr, nqn, host_nqn, dhchap_key, dhchap_ctrlr_key,
                     gw_group, server_address, traddr):
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.change_host_key(
        NVMeoFClient.pb2.change_host_key_req(subsystem_nqn=nqn,
                                             host_nqn=host_nqn,
                                             dhchap_key=dhchap_key,
                                             dhchap_ctrlr_key=dhchap_ctrlr_key)
    )


@NvmeofCLICommand(
    "nvmeof host change_key",
    model.RequestStatus,
    success_message_template=("Changing key for host {host_nqn} "
                              "on subsystem {nqn}: Successful")
)
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def host_change_key(mgr, nqn: str, host_nqn: str, dhchap_key: str,
                    gw_group: Optional[str] = None,
                    server_address: Optional[str] = None,
                    traddr: Optional[str] = None):
    """Change host DH-HMAC-CHAP key"""
    return _host_change_key(mgr, nqn, host_nqn, dhchap_key, "-",
                            gw_group, server_address, traddr)


@NvmeofCLICommand(
    "nvmeof host change_controller_key",
    model.RequestStatus,
    success_message_template=("Changing controller key for host {host_nqn} "
                              "on subsystem {nqn}: Successful")
)
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def host_change_controller_key(mgr, nqn: str, host_nqn: str,
                               dhchap_controller_key: str,
                               gw_group: Optional[str] = None,
                               server_address: Optional[str] = None,
                               traddr: Optional[str] = None):
    """Change host DH-HMAC-CHAP controller key"""
    return _host_change_key(mgr, nqn, host_nqn, "-", dhchap_controller_key,
                            gw_group, server_address, traddr)


@NvmeofCLICommand(
    "nvmeof host del_key",
    model.RequestStatus,
    success_message_template=("Deleting key for host {host_nqn} "
                              "on subsystem {nqn}: Successful")
)
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def host_del_key(mgr, nqn: str, host_nqn: str,
                 gw_group: Optional[str] = None,
                 server_address: Optional[str] = None,
                 traddr: Optional[str] = None):
    """Delete host DH-HMAC-CHAP key"""
    return _host_change_key(mgr, nqn, host_nqn, None, "-",
                            gw_group, server_address, traddr)


@NvmeofCLICommand(
    "nvmeof host del_controller_key",
    model.RequestStatus,
    success_message_template=("Deleting controller key for host {host_nqn} "
                              "on subsystem {nqn}: Successful")
)
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def host_del_controller_key(mgr, nqn: str, host_nqn: str,
                            gw_group: Optional[str] = None,
                            server_address: Optional[str] = None,
                            traddr: Optional[str] = None):
    """Delete host DH-HMAC-CHAP controller key"""
    return _host_change_key(mgr, nqn, host_nqn, "-", None,
                            gw_group, server_address, traddr)


@NvmeofCLICommand("nvmeof listener list", model.ListenerList)
@convert_to_model(model.ListenerList)
@handle_nvmeof_error
def listener_list(mgr, nqn: str, gw_group: Optional[str] = None,
                  server_address: Optional[str] = None,
                  traddr: Optional[str] = None):
    """List all NVMeoF listeners"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.list_listeners(
        NVMeoFClient.pb2.list_listeners_req(subsystem=nqn)
    )


@NvmeofCLICommand(
    "nvmeof listener add",
    model.RequestStatus,
    success_message_template="Adding {nqn} listener at {traddr}:{trsvcid}: Successful"
)
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def listener_add(mgr, nqn: str, host_name: str, traddr: str,
                 trsvcid: Optional[int] = None,
                 adrfam: int = 0,  # IPv4,
                 gw_group: Optional[str] = None,
                 server_address: Optional[str] = None,
                 secure: Optional[bool] = False,
                 force: Optional[bool] = False,
                 verify_host_name: Optional[bool] = False):
    """Create a new NVMeoF listener"""
    client = NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    )
    return client.stub.create_listener(
        NVMeoFClient.pb2.create_listener_req(
            nqn=nqn,
            host_name=host_name,
            traddr=traddr,
            trsvcid=int(trsvcid) if trsvcid is not None else None,
            adrfam=int(adrfam),
            secure=str_to_bool(secure),
            force=str_to_bool(force),
            verify_host_name=str_to_bool(verify_host_name),
        )
    )


@NvmeofCLICommand(
    "nvmeof listener del",
    model.RequestStatus,
    success_message_template=(
        "Deleting listener {traddr}:{trsvcid} from {nqn} {host_msg}: Successful"
    ),
    success_message_map={
        "traddr": lambda v, _f: escape_address_if_ipv6(v) if v is not None else "",
        "host_msg": lambda _v, f: (
            "for all hosts" if f.get("host_name") == "*"
            else f"for host {f.get('host_name')}"
        ),
    }
)
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def listener_del(mgr, nqn: str, host_name: str, traddr: str, trsvcid: int,
                 adrfam: int = 0,  # IPv4
                 force: bool = False,
                 gw_group: Optional[str] = None,
                 server_address: Optional[str] = None):
    """Delete an existing NVMeoF listener"""
    client = NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    )
    return client.stub.delete_listener(
        NVMeoFClient.pb2.delete_listener_req(
            nqn=nqn,
            host_name=host_name,
            traddr=traddr,
            trsvcid=int(trsvcid),
            adrfam=int(adrfam),
            force=str_to_bool(force),
        )
    )


@NvmeofCLICommand(
    "nvmeof gateway info", model.GatewayInfo, alias="nvmeof gw info"
)
@convert_to_model(model.GatewayInfo)
@handle_nvmeof_error
def gateway_info(mgr, gw_group: Optional[str] = None,
                 server_address: Optional[str] = None,
                 traddr: Optional[str] = None):
    """Get information about the NVMeoF gateway"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.get_gateway_info(
        NVMeoFClient.pb2.get_gateway_info_req()
    )


@NvmeofCLICommand(
    "nvmeof gateway version", model.GatewayVersion, alias="nvmeof gw version"
)
@convert_to_model(model.GatewayVersion)
@handle_nvmeof_error
def gateway_version(mgr, gw_group: Optional[str] = None,
                    server_address: Optional[str] = None,
                    traddr: Optional[str] = None):
    """Get the version of the NVMeoF gateway"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    gw_info = NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.get_gateway_info(
        NVMeoFClient.pb2.get_gateway_info_req()
    )
    return NVMeoFClient.pb2.gw_version(status=gw_info.status,
                                       error_message=gw_info.error_message,
                                       version=gw_info.version)


@NvmeofCLICommand(
    "nvmeof gateway get_log_level", model.GatewayLogLevelInfo,
    alias="nvmeof gw get_log_level"
)
@convert_to_model(model.GatewayLogLevelInfo)
@handle_nvmeof_error
def gateway_get_log_level(mgr, gw_group: Optional[str] = None,
                          server_address: Optional[str] = None,
                          traddr: Optional[str] = None):
    """Get NVMeoF gateway log level information"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.get_gateway_log_level(
        NVMeoFClient.pb2.get_gateway_log_level_req()
    )


@NvmeofCLICommand(
    "nvmeof gateway set_log_level", model.RequestStatus,
    alias="nvmeof gw set_log_level",
    success_message_template="Set gateway log level to {log_level}: Successful")
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def gateway_set_log_level(mgr, log_level: str, gw_group: Optional[str] = None,
                          server_address: Optional[str] = None,
                          traddr: Optional[str] = None):
    """Set NVMeoF gateway log levels"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    log_level = log_level.strip().lower()
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.set_gateway_log_level(
        NVMeoFClient.pb2.set_gateway_log_level_req(log_level=log_level)
    )


@NvmeofCLICommand(
    "nvmeof gateway get_stats", model.GatewayStatsInfo,
    alias="nvmeof gw get_stats")
@convert_to_model(model.GatewayStatsInfo)
@handle_nvmeof_error
def gateway_get_stats(mgr, gw_group: Optional[str] = None,
                      server_address: Optional[str] = None,
                      traddr: Optional[str] = None):
    """Get NVMeoF statistics for the gateway"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.get_gateway_stats(
        NVMeoFClient.pb2.get_gateway_stats_req()
    )


@NvmeofCLICommand(
    "nvmeof gateway listener_info", model.GatewayListenersInfo,
    alias="nvmeof gw listener_info")
@convert_to_model(model.GatewayListenersInfo)
@handle_nvmeof_error
def gateway_listener_info(mgr, nqn: str, gw_group: Optional[str] = None,
                          server_address: Optional[str] = None,
                          traddr: Optional[str] = None):
    """Get NVMeoF gateway's listeners info"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.show_gateway_listeners_info(
        NVMeoFClient.pb2.show_gateway_listeners_info_req(subsystem_nqn=nqn)
    )


@NvmeofCLICommand(
    "nvmeof gateway set_io_stats_mode", model.RequestStatus,
    alias="nvmeof gw set_io_stats_mode",
    success_message_template="Set gateway IO statistics mode to {enabled}: Successful")
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def gateway_set_io_stats_mode(mgr, enabled: bool,
                              gw_group: Optional[str] = None,
                              server_address: Optional[str] = None,
                              traddr: Optional[str] = None):
    """Enable or disable IO statistics collection"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.set_gateway_io_stats_mode(
        NVMeoFClient.pb2.set_gateway_io_stats_mode_req(enabled=enabled)
    )


@NvmeofCLICommand(
    "nvmeof gateway get_thread_stats", model.ThreadStatsInfo,
    alias="nvmeof gw get_thread_stats")
@convert_to_model(model.ThreadStatsInfo)
@handle_nvmeof_error
def gateway_get_thread_stats(mgr, gw_group: Optional[str] = None,
                             server_address: Optional[str] = None,
                             traddr: Optional[str] = None):
    """Get NVMeoF thread statistics for the gateway"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.get_thread_stats(
        NVMeoFClient.pb2.get_thread_stats_req()
    )


@NvmeofCLICommand(
    "nvmeof gateway refresh_network", model.GwRefreshNetworkStatus,
    alias="nvmeof gw refresh_network",
    success_message_template=("Refreshed configured network masks for subsystem "
                              "{nqn} on gateway {server_address}: "
                              "Successful{added}{removed}"),
    success_message_map={
        "server_address": lambda v, f: v or f.get("traddr"),
        "added": lambda v, _f: f"\nAdded: {', '.join(v)}" if v else "",
        "removed": lambda v, _f: f"\nRemoved: {', '.join(v)}" if v else "",
    }
)
@convert_to_model(model.GwRefreshNetworkStatus)
@handle_nvmeof_error
def gateway_refresh_network(mgr, nqn: str = "", gw_group: Optional[str] = None,
                            server_address: Optional[str] = None,
                            traddr: Optional[str] = None):
    """Re-evaluate subsystem network masks and update auto-listeners for this gateway"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr,
        require=True
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.gw_refresh_network(
        NVMeoFClient.pb2.gw_refresh_network_req(subsystem_nqn=nqn)
    )


@NvmeofCLICommand("nvmeof spdk_log_level get",
                  model.SpdkNvmfLogFlagsAndLevelInfo)
@convert_to_model(model.SpdkNvmfLogFlagsAndLevelInfo)
@handle_nvmeof_error
def spdk_log_level_get(mgr, all_log_flags: Optional[bool] = None,
                       gw_group: Optional[str] = None,
                       server_address: Optional[str] = None,
                       traddr: Optional[str] = None):
    """Get NVMeoF gateway spdk log levels"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.get_spdk_nvmf_log_flags_and_level(
        NVMeoFClient.pb2.get_spdk_nvmf_log_flags_and_level_req(all_log_flags=all_log_flags)
    )


@NvmeofCLICommand(
    "nvmeof spdk_log_level set",
    model.RequestStatus,
    success_message_template="Set SPDK log levels and nvmf log flags: Successful"
)
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def spdk_log_level_set(mgr, log_level: Optional[str] = None,
                       print_level: Optional[str] = None,
                       extra_log_flags: Optional[List[str]] = None,
                       gw_group: Optional[str] = None,
                       server_address: Optional[str] = None,
                       traddr: Optional[str] = None):
    """Set NVMeoF gateway spdk log levels"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    log_level = log_level.strip().upper() if log_level else None
    print_level = print_level.strip().upper() if print_level else None
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.set_spdk_nvmf_logs(
        NVMeoFClient.pb2.set_spdk_nvmf_logs_req(log_level=log_level,
                                                print_level=print_level,
                                                extra_log_flags=extra_log_flags)
    )


@NvmeofCLICommand("nvmeof spdk_log_level disable", model.RequestStatus,
                  success_message_template="Disable SPDK log flags: Successful")
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def spdk_log_level_disable(mgr, extra_log_flags: Optional[List[str]] = None,
                           gw_group: Optional[str] = None,
                           server_address: Optional[str] = None,
                           traddr: Optional[str] = None):
    """Disable NVMeoF gateway spdk log"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.disable_spdk_nvmf_logs(
        NVMeoFClient.pb2.disable_spdk_nvmf_logs_req(extra_log_flags=extra_log_flags)
    )


@NvmeofCLICommand("nvmeof subsystem list", model.SubsystemList)
@convert_to_model(model.SubsystemList)
@handle_nvmeof_error
def subsystem_list(mgr, nqn: Optional[str] = None, serial_number: Optional[str] = None,
                   gw_group: Optional[str] = None, server_address: Optional[str] = None,
                   traddr: Optional[str] = None):
    """List all NVMeoF subsystems"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.list_subsystems(
        NVMeoFClient.pb2.list_subsystems_req(subsystem_nqn=nqn,
                                             serial_number=serial_number)
    )


@NvmeofCLICommand("nvmeof subsystem get", model.SubsystemList)
@convert_to_model(model.SubsystemList)
@handle_nvmeof_error
def subsystem_get(mgr, nqn: str, gw_group: Optional[str] = None,
                  server_address: Optional[str] = None,
                  traddr: Optional[str] = None):
    """Get information from a specific NVMeoF subsystem"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.list_subsystems(
        NVMeoFClient.pb2.list_subsystems_req(subsystem_nqn=nqn)
    )


@NvmeofCLICommand("nvmeof subsystem add", model.SubsystemStatus,
                  success_message_template="Adding subsystem {nqn}: Successful")
@convert_to_model(model.SubsystemStatus)
@handle_nvmeof_error
def subsystem_add(mgr, nqn: str,
                  max_namespaces: Optional[int] = None, no_group_append: Optional[bool] = False,
                  serial_number: Optional[str] = None, dhchap_key: Optional[str] = None,
                  gw_group: Optional[str] = None, server_address: Optional[str] = None,
                  network_mask: Optional[List[str]] = None,
                  port: Optional[int] = None, secure_listeners: Optional[bool] = False,
                  traddr: Optional[str] = None, model_name: Optional[str] = None):
    """Create a new NVMeoF subsystem"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.create_subsystem(
        NVMeoFClient.pb2.create_subsystem_req(
            subsystem_nqn=nqn, serial_number=serial_number,
            max_namespaces=max_namespaces, enable_ha=True,
            no_group_append=no_group_append,
            dhchap_key=dhchap_key, network_mask=network_mask,
            port=port, secure_listeners=secure_listeners,
            model_name=model_name
        )
    )


@NvmeofCLICommand("nvmeof subsystem del", model.RequestStatus,
                  success_message_template="Deleting subsystem {nqn}: Successful")
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def subsystem_del(mgr, nqn: str, force: Optional[str] = "false", gw_group: Optional[str] = None,
                  server_address: Optional[str] = None,
                  traddr: Optional[str] = None):
    """Delete an existing NVMeoF subsystem"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.delete_subsystem(
        NVMeoFClient.pb2.delete_subsystem_req(
            subsystem_nqn=nqn, force=str_to_bool(force)
        )
    )


@NvmeofCLICommand("nvmeof subsystem change_key", model.RequestStatus,
                  success_message_template="Changing key for subsystem {nqn}: Successful")
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def subsystem_change_key(mgr, nqn: str, dhchap_key: str, gw_group: Optional[str] = None,
                         server_address: Optional[str] = None,
                         traddr: Optional[str] = None):
    """Change subsystem inband authentication key"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.change_subsystem_key(
        NVMeoFClient.pb2.change_subsystem_key_req(
            subsystem_nqn=nqn,
            dhchap_key=dhchap_key
        )
    )


@NvmeofCLICommand("nvmeof subsystem del_key", model.RequestStatus,
                  success_message_template="Deleting key for subsystem {nqn}: Successful")
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def subsystem_del_key(mgr, nqn: str, gw_group: Optional[str] = None,
                      server_address: Optional[str] = None,
                      traddr: Optional[str] = None):
    """Delete subsystem inband authentication key"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.change_subsystem_key(
        NVMeoFClient.pb2.change_subsystem_key_req(
            subsystem_nqn=nqn, dhchap_key=None
        )
    )


@NvmeofCLICommand(
    "nvmeof subsystem add_network", model.RequestStatus,
    success_message_template=("Adding network mask {network_mask} for subsystem "
                              "{nqn}: Successful")
)
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def subsystem_add_network(mgr, nqn: str, network_mask: str, gw_group: Optional[str] = None,
                          server_address: Optional[str] = None,
                          traddr: Optional[str] = None):
    """Add subsystem network mask"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.add_subsystem_network(
        NVMeoFClient.pb2.add_subsystem_network_req(
            subsystem_nqn=nqn, network_mask=network_mask
        )
    )


@NvmeofCLICommand(
    "nvmeof subsystem del_network", model.RequestStatus,
    success_message_template=("Deleting network mask {network_mask} for subsystem "
                              "{nqn}: Successful")
)
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def subsystem_del_network(mgr, nqn: str, network_mask: str, gw_group: Optional[str] = None,
                          server_address: Optional[str] = None,
                          traddr: Optional[str] = None):
    """Delete subsystem network mask"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.del_subsystem_network(
        NVMeoFClient.pb2.del_subsystem_network_req(
            subsystem_nqn=nqn, network_mask=network_mask
        )
    )


@NvmeofCLICommand(
    "nvmeof subsystem add_kmip_server_endpoint", model.RequestStatus,
    success_message_template=("Adding an endpoint, with address {address}:{port}, "
                              "to KMIP server {server_name} on subsystem {nqn}: "
                              "Successful")
)
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def subsystem_add_kmip_server_endpoint(mgr, nqn: str, server_name: str,
                                       address: Optional[str] = None,
                                       port: Optional[int] = 5696, gw_group: Optional[str] = None,
                                       server_address: Optional[str] = None,
                                       traddr: Optional[str] = None):
    """Add a KMIP server endpoint to the subsystem"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    ep = NVMeoFClient.pb2.kmip_server_endpoint(address=address, port=port)
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.add_kmip_server_endpoints(
        NVMeoFClient.pb2.add_kmip_server_endpoints_req(
            subsystem_nqn=nqn, server_name=server_name,
            endpoints=[ep]
        )
    )


@NvmeofCLICommand(
    "nvmeof subsystem del_kmip_server_endpoint", model.RequestStatus,
    success_message_template=("Deleting endpoint, with address {address}:{port}, from "
                              "KMIP server {server_name} on subsystem {nqn}: Successful")
)
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def subsystem_del_kmip_server_endpoint(mgr, nqn: str, server_name: str,
                                       address: Optional[str] = None,
                                       port: Optional[int] = 5696, gw_group: Optional[str] = None,
                                       server_address: Optional[str] = None,
                                       traddr: Optional[str] = None):
    """Delete a KMIP server endpoint from the subsystem"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    ep = NVMeoFClient.pb2.kmip_server_endpoint(address=address, port=port)
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.del_kmip_server_endpoints(
        NVMeoFClient.pb2.del_kmip_server_endpoints_req(
            subsystem_nqn=nqn, server_name=server_name,
            endpoints=[ep]
        )
    )


@NvmeofCLICommand("nvmeof subsystem list_kmip_server_endpoints",
                  model.SubsystemListKMIPEndpoints)
@convert_to_model(model.SubsystemListKMIPEndpoints)
@handle_nvmeof_error
def subsystem_list_kmip_server_endpoints(mgr, nqn: Optional[str] = None, server_name: Optional[str] = None,
                                         gw_group: Optional[str] = None, server_address: Optional[str] = None,
                                         traddr: Optional[str] = None):
    """List KMIP server endpoints for a subsystem or all subsystems"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.list_kmip_server_endpoints(
        NVMeoFClient.pb2.list_kmip_server_endpoints_req(subsystem_nqn=nqn,
                                                        server_name=server_name)
    )


@NvmeofCLICommand("nvmeof get_subsystems", model.GetSubsystems)
@convert_to_model(model.GetSubsystems)
@handle_nvmeof_error
def get_subsystems(mgr,
                   gw_group: Optional[str] = None,
                   server_address: Optional[str] = None,
                   traddr: Optional[str] = None
                   ):
    """Get NVMeoF subsystems"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.get_subsystems(
        NVMeoFClient.pb2.get_subsystems_req()
    )


@NvmeofCLICommand(
    "nvmeof namespace list", model.NamespaceList, alias="nvmeof ns list"
)
@convert_to_model(model.NamespaceList)
@handle_nvmeof_error
def namespace_list(mgr, nqn: Optional[str] = None, nsid: Optional[str] = None,
                   uuid: Optional[str] = None,
                   gw_group: Optional[str] = None, server_address: Optional[str] = None,
                   traddr: Optional[str] = None):
    """List all NVMeoF namespaces in a subsystem"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.list_namespaces(
        NVMeoFClient.pb2.list_namespaces_req(subsystem=nqn,
                                             nsid=int(nsid) if nsid else None,
                                             uuid=uuid)
    )


@NvmeofCLICommand(
    "nvmeof namespace get", model.NamespaceList, alias="nvmeof ns get")
@convert_to_model(model.NamespaceList)
@handle_nvmeof_error
def namespace_get(mgr, nqn: str, nsid: str, gw_group: Optional[str] = None,
                  server_address: Optional[str] = None,
                  traddr: Optional[str] = None):
    """Get info from specified NVMeoF namespace"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.list_namespaces(
        NVMeoFClient.pb2.list_namespaces_req(subsystem=nqn, nsid=int(nsid))
    )


@NvmeofCLICommand(
    "nvmeof namespace get_io_stats", model.NamespaceIOStats,
    alias="nvmeof ns get_io_stats"
)
@convert_to_model(model.NamespaceIOStats)
@handle_nvmeof_error
def namespace_get_io_stats(mgr, nqn: str, nsid: str, gw_group: Optional[str] = None,
                           server_address: Optional[str] = None,
                           traddr: Optional[str] = None):
    """Get IO stats from specified NVMeoF namespace"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.namespace_get_io_stats(
        NVMeoFClient.pb2.namespace_get_io_stats_req(
            subsystem_nqn=nqn, nsid=int(nsid))
    )


@NvmeofCLICommand(
    "nvmeof namespace add",
    model.NamespaceCreation,
    alias="nvmeof ns add",
    success_message_template="Adding namespace {nsid} to {nqn}: Successful",
    deprecated_params={"size": "--size is deprecated, please use --rbd-image-size"}
)
@convert_to_model(model.NamespaceCreation)
@handle_nvmeof_error
def namespace_add(mgr,
                  nqn: str,
                  rbd_image_name: str,
                  rbd_pool: str = "rbd",
                  rbd_data_pool: Optional[str] = None,
                  nsid: Optional[str] = None,
                  uuid: Optional[str] = None,
                  create_image: Optional[bool] = False,
                  size: Optional[str] = None,
                  rbd_image_size: Optional[str] = None,
                  trash_image: Optional[bool] = False,
                  block_size: int = 512,
                  load_balancing_group: Optional[int] = None,
                  force: Optional[bool] = False,
                  no_auto_visible: Optional[bool] = False,
                  disable_auto_resize: Optional[bool] = False,
                  read_only: Optional[bool] = False,
                  location: Optional[str] = None,
                  gw_group: Optional[str] = None,
                  server_address: Optional[str] = None,
                  traddr: Optional[str] = None,
                  rados_namespace: Optional[str] = None,
                  encryption_format: Optional[List[str]] = None,
                  encryption_algorithm: Optional[str] = None,
                  key_id: Optional[List[str]] = None,
                  ):
    """Create a new NVMeoF namespace."""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    if size and rbd_image_size:
        raise NvmeofInvalidInputError(
            "Can use size or rbd_image_size but not both",
            code="can_use_size_or_rbd_image_size_but_not_both",
        )

    size_b = rbd_image_size_b = None
    if size:
        size_b = convert_to_bytes(size, default_unit='MB')
    if rbd_image_size:
        rbd_image_size_b = convert_to_bytes(rbd_image_size, default_unit='MB')
    if not encryption_format:
        encryption_format = []
    if not key_id:
        key_id = []
    if len(encryption_format) != len(key_id):
        raise NvmeofInvalidInputError(
            "The number of key IDs should match the number of encryption formats",
            code="key_ids_encryption_formats_mismatch",
        )
    enc_entries = [
        NVMeoFClient.pb2.encryption_entry(
            format=f.strip().lower(),
            key_id=k.strip()
        ) for f, k in zip(encryption_format, key_id)]
    enc_alg = encryption_algorithm.strip().lower() if encryption_algorithm else None
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.namespace_add(
        NVMeoFClient.pb2.namespace_add_req(
            subsystem_nqn=nqn,
            nsid=int(nsid) if nsid else None,
            uuid=uuid,
            rbd_image_name=rbd_image_name,
            rados_namespace_name=rados_namespace,
            rbd_pool_name=rbd_pool,
            rbd_data_pool_name=rbd_data_pool,
            block_size=block_size,
            create_image=create_image,
            size=rbd_image_size_b or size_b,
            trash_image=trash_image,
            anagrpid=load_balancing_group,
            force=force,
            no_auto_visible=no_auto_visible,
            disable_auto_resize=disable_auto_resize,
            read_only=read_only,
            location=location,
            encryption_entries=enc_entries,
            encryption_algorithm=enc_alg
        )
    )


@NvmeofCLICommand(
    "nvmeof namespace set_qos", model=model.RequestStatus, alias="nvmeof ns set_qos",
    success_message_template="Setting QOS limits of namespace {nsid} in {nqn}: Successful")
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def namespace_set_qos(mgr,
                      nqn: str,
                      nsid: str,
                      rw_ios_per_second: Optional[int] = None,
                      rw_mbytes_per_second: Optional[int] = None,
                      r_mbytes_per_second: Optional[int] = None,
                      w_mbytes_per_second: Optional[int] = None,
                      force: Optional[bool] = False,
                      gw_group: Optional[str] = None,
                      server_address: Optional[str] = None,
                      traddr: Optional[str] = None
                      ):
    """set QOS for specified NVMeoF namespace"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.namespace_set_qos_limits(
        NVMeoFClient.pb2.namespace_set_qos_req(
            subsystem_nqn=nqn,
            nsid=int(nsid),
            rw_ios_per_second=rw_ios_per_second,
            rw_mbytes_per_second=rw_mbytes_per_second,
            r_mbytes_per_second=r_mbytes_per_second,
            w_mbytes_per_second=w_mbytes_per_second,
            force=force,
        )
    )


@NvmeofCLICommand(
    "nvmeof namespace change_load_balancing_group",
    model=model.RequestStatus,
    alias="nvmeof ns change_load_balancing_group",
    success_message_template=("Changing load balancing group of namespace {nsid} "
                              "in {nqn} to {load_balancing_group}: Successful")
)
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def namespace_change_load_balancing_group(mgr,
                                          nqn: str,
                                          nsid: str,
                                          load_balancing_group: Optional[int] = None,
                                          gw_group: Optional[str] = None,
                                          server_address: Optional[str] = None,
                                          traddr: Optional[str] = None
                                          ):
    """set the load balancing group for specified NVMeoF namespace"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.namespace_change_load_balancing_group(
        NVMeoFClient.pb2.namespace_change_load_balancing_group_req(
            subsystem_nqn=nqn, nsid=int(nsid), anagrpid=load_balancing_group
        )
    )


@NvmeofCLICommand(
    "nvmeof namespace resize",
    model=model.RequestStatus,
    alias="nvmeof ns resize",
    success_message_template=("Resizing namespace {nsid} in {nqn} "
                              "to {rbd_image_size}: Successful")
)
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def namespace_resize(mgr,
                     nqn: str,
                     nsid: str,
                     rbd_image_size: str,
                     gw_group: Optional[str] = None,
                     server_address: Optional[str] = None,
                     traddr: Optional[str] = None
                     ):
    """resize the specified NVMeoF namespace"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    if rbd_image_size:
        rbd_image_size_b = convert_to_bytes(rbd_image_size, default_unit='MB')
    mib = 1024 * 1024
    rbd_image_size_mb = rbd_image_size_b // mib

    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.namespace_resize(
        NVMeoFClient.pb2.namespace_resize_req(
            subsystem_nqn=nqn, nsid=int(nsid), new_size=rbd_image_size_mb
        )
    )


@NvmeofCLICommand(
    "nvmeof namespace add_host",
    model=model.RequestStatus,
    alias="nvmeof ns add_host",
    success_message_template=("Adding host {host_nqn} to "
                              "namespace {nsid} on {nqn}: Successful")
)
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def namespace_add_host(mgr,
                       nqn: str,
                       nsid: str,
                       host_nqn: str,
                       force: Optional[bool] = None,
                       gw_group: Optional[str] = None,
                       server_address: Optional[str] = None,
                       traddr: Optional[str] = None
                       ):
    """Adds a host to the specified NVMeoF namespace"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.namespace_add_host(
        NVMeoFClient.pb2.namespace_add_host_req(subsystem_nqn=nqn,
                                                nsid=int(nsid),
                                                host_nqn=host_nqn,
                                                force=str_to_bool(force) if force else None)
    )


@NvmeofCLICommand(
    "nvmeof namespace del_host",
    model=model.RequestStatus,
    alias="nvmeof ns del_host",
    success_message_template=("Deleting host {host_nqn} from "
                              "namespace {nsid} on {nqn}: Successful")
)
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def namespace_del_host(mgr,
                       nqn: str,
                       nsid: str,
                       host_nqn: str,
                       gw_group: Optional[str] = None,
                       server_address: Optional[str] = None,
                       traddr: Optional[str] = None
                       ):
    """Removes a host from the specified NVMeoF namespace"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.namespace_delete_host(
        NVMeoFClient.pb2.namespace_delete_host_req(
            subsystem_nqn=nqn,
            nsid=int(nsid),
            host_nqn=host_nqn,
        )
    )


@NvmeofCLICommand(
    "nvmeof namespace change_visibility",
    model=model.RequestStatus,
    alias="nvmeof ns change_visibility",
    success_message_template=(
        'Changing visibility of namespace {nsid} in {nqn} to "{auto_visible}": Successful'
    ),
    success_message_map={
        "auto_visible": lambda v, _f: (
            "visible to all hosts" if str_to_bool(v)
            else "visible to selected hosts"
        )
    }
)
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def namespace_change_visibility(mgr,
                                nqn: str,
                                nsid: str,
                                auto_visible: str,
                                force: Optional[bool] = False,
                                gw_group: Optional[str] = None,
                                server_address: Optional[str] = None,
                                traddr: Optional[str] = None
                                ):
    """changes the visibility of the specified NVMeoF namespace to all or selected hosts"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.namespace_change_visibility(
        NVMeoFClient.pb2.namespace_change_visibility_req(
            subsystem_nqn=nqn,
            nsid=int(nsid),
            auto_visible=str_to_bool(auto_visible),
            force=str_to_bool(force),
        )
    )


@NvmeofCLICommand(
    "nvmeof namespace change_location",
    model=model.RequestStatus,
    alias="nvmeof ns change_location",
    success_message_template=(
        'Setting location for namespace {nsid} in {nqn} to "{location}": Successful'
    )
)
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def namespace_change_location(mgr,
                              nqn: str,
                              nsid: str,
                              location: str,
                              gw_group: Optional[str] = None,
                              server_address: Optional[str] = None,
                              traddr: Optional[str] = None
                              ):
    """Change the location of the specified NVMeoF namespace"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.namespace_change_location(
        NVMeoFClient.pb2.namespace_change_location_req(
            subsystem_nqn=nqn,
            nsid=int(nsid),
            location=location,
        )
    )


@NvmeofCLICommand(
    "nvmeof namespace list_hosts",
    model=model.NamespaceHostsList,
    alias="nvmeof ns list_hosts"
)
@handle_nvmeof_error
def namespace_list_hosts(
    mgr, nqn: Optional[str] = None, nsid: Optional[str] = None,
    uuid: Optional[str] = None,
    gw_group: Optional[str] = None, server_address: Optional[str] = None,
    traddr: Optional[str] = None
):
    """List all NVMeoF namespaces with their allowed hosts"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    ns_list = NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.list_namespaces(
        NVMeoFClient.pb2.list_namespaces_req(subsystem=nqn,
                                             nsid=int(nsid) if nsid else None,
                                             uuid=uuid)
    )

    # Transform to NamedTuple with only NQN, NSID, and Hosts
    host_infos = []
    for ns in ns_list.namespaces:
        host_infos.append(model.NamespaceHostInfo(
            nqn=ns.ns_subsystem_nqn or nqn or "",
            nsid=ns.nsid,
            hosts=list(ns.hosts)
        ))

    return namedtuple_to_dict(model.NamespaceHostsList(
        status=ns_list.status,
        error_message=ns_list.error_message,
        namespaces=host_infos
    ))


@NvmeofCLICommand(
    "nvmeof namespace list_locations",
    model=model.NamespaceLocationsList,
    alias="nvmeof ns list_locations"
)
@handle_nvmeof_error
def namespace_list_locations(
    mgr, nqn: Optional[str] = None, nsid: Optional[str] = None,
    uuid: Optional[str] = None,
    gw_group: Optional[str] = None, server_address: Optional[str] = None,
    traddr: Optional[str] = None
):
    """List namespace distribution per site locations"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    ns_list = NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.list_namespaces(
        NVMeoFClient.pb2.list_namespaces_req(subsystem=nqn,
                                             nsid=int(nsid) if nsid else None,
                                             uuid=uuid)
    )

    # Aggregate namespaces by subsystem, load balancing group, and location
    location_counts: Dict[Tuple[str, int, str], int] = {}
    for ns in ns_list.namespaces:
        subsystem_nqn = ns.ns_subsystem_nqn or nqn or ""
        lb_group = ns.load_balancing_group if ns.load_balancing_group else 0
        location = ns.location if ns.location else "<default>"

        key = (subsystem_nqn, lb_group, location)
        location_counts[key] = location_counts.get(key, 0) + 1

    # Convert to NamedTuple list
    location_infos = []
    for (subsystem_nqn, lb_group, location), count in sorted(location_counts.items()):
        location_infos.append(model.NamespaceLocationInfo(
            subsystem=subsystem_nqn,
            load_balancing_group=lb_group,
            location=location,
            namespace_count=count
        ))

    return namedtuple_to_dict(model.NamespaceLocationsList(
        status=ns_list.status,
        error_message=ns_list.error_message,
        locations=location_infos
    ))


@NvmeofCLICommand(
    "nvmeof namespace set_auto_resize",
    model=model.RequestStatus,
    alias="nvmeof ns set_auto_resize",
    success_message_template=(
        'Setting auto resize flag for namespace {nsid} '
        'in {nqn} to "{auto_resize_text}": Successful'
    ),
    success_message_map={
        "auto_resize_text": lambda _v, f: (
            "auto resize namespace" if str_to_bool(f.get("auto_resize_enabled"))
            else "do not auto resize namespace"
        )
    }
)
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def namespace_set_auto_resize(mgr,
                              nqn: str,
                              nsid: str,
                              auto_resize_enabled: bool,
                              gw_group: Optional[str] = None,
                              server_address: Optional[str] = None,
                              traddr: Optional[str] = None
                              ):
    """Enable or disable namespace auto resize when RBD image is resized"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.namespace_set_auto_resize(
        NVMeoFClient.pb2.namespace_set_auto_resize_req(
            subsystem_nqn=nqn,
            nsid=int(nsid),
            auto_resize=str_to_bool(auto_resize_enabled),
        )
    )


@NvmeofCLICommand(
    "nvmeof namespace set_rbd_trash_image",
    model=model.RequestStatus,
    alias="nvmeof ns set_rbd_trash_image",
    success_message_template=(
        'Setting RBD trash image flag for namespace {nsid} '
        'in {nqn} to "{trash_text}": Successful'
    ),
    success_message_map={
        "trash_text": lambda _v, f: (
            "trash on namespace deletion" if str_to_bool(f.get("rbd_trash_image_on_delete"))
            else "do not trash on namespace deletion"
        )
    }
)
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def namespace_set_rbd_trash_image(mgr,
                                  nqn: str,
                                  nsid: str,
                                  rbd_trash_image_on_delete: str,
                                  gw_group: Optional[str] = None,
                                  server_address: Optional[str] = None,
                                  traddr: Optional[str] = None
                                  ):
    """changes the trash image on delete of the specified NVMeoF \
                namespace to all or selected hosts"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address,
    ).stub.namespace_set_rbd_trash_image(
        NVMeoFClient.pb2.namespace_set_rbd_trash_image_req(
            subsystem_nqn=nqn,
            nsid=int(nsid),
            trash_image=str_to_bool(rbd_trash_image_on_delete),
        )
    )


@NvmeofCLICommand(
    "nvmeof namespace refresh_size", model=model.RequestStatus,
    alias="nvmeof ns refresh_size",
    success_message_template="Refreshing size for namespace {nsid} in {nqn}: Successful"
)
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def namespace_refresh_size(mgr,
                           nqn: str,
                           nsid: str,
                           gw_group: Optional[str] = None,
                           server_address: Optional[str] = None,
                           traddr: Optional[str] = None
                           ):
    """refresh the specified NVMeoF namespace to current RBD image size"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.namespace_resize(
        NVMeoFClient.pb2.namespace_resize_req(
            subsystem_nqn=nqn,
            nsid=int(nsid),
            new_size=0
        )
    )


@NvmeofCLICommand(
    "nvmeof namespace unpin",
    model=model.RequestStatus,
    alias="nvmeof ns unpin",
    success_message_template=(
        'Unpinning load balancing group for namespace {nsid} '
        'in {nqn}: Successful'
    ),
)
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def namespace_unpin(mgr,
                    nqn: str,
                    nsid: str,
                    gw_group: Optional[str] = None,
                    server_address: Optional[str] = None,
                    traddr: Optional[str] = None
                    ):
    """Unpin namespace load balancing group"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.namespace_unpin(
        NVMeoFClient.pb2.namespace_unpin_req(
            subsystem_nqn=nqn,
            nsid=int(nsid),
        )
    )


@NvmeofCLICommand(
    "nvmeof namespace update", model.NamespaceList, alias="nvmeof ns update"
)
@convert_to_model(model.NamespaceList)
@handle_nvmeof_error
def namespace_update(mgr,
                     nqn: str,
                     nsid: str,
                     rbd_image_size: Optional[int] = None,
                     load_balancing_group: Optional[int] = None,
                     rw_ios_per_second: Optional[int] = None,
                     rw_mbytes_per_second: Optional[int] = None,
                     r_mbytes_per_second: Optional[int] = None,
                     w_mbytes_per_second: Optional[int] = None,
                     trash_image: Optional[bool] = None,
                     location: Optional[str] = None,
                     gw_group: Optional[str] = None,
                     server_address: Optional[str] = None,
                     traddr: Optional[str] = None,
                     ):
    """Update an existing NVMeoF namespace"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    contains_failure = False

    if rbd_image_size:
        mib = 1024 * 1024
        new_size_mib = int((rbd_image_size + mib - 1) / mib)

        resp = NVMeoFClient(
            mgr, gw_group=gw_group,
            server_address=server_address
        ).stub.namespace_resize(
            NVMeoFClient.pb2.namespace_resize_req(
                subsystem_nqn=nqn, nsid=int(nsid), new_size=new_size_mib
            )
        )
        if resp.status != 0:
            contains_failure = True

    if load_balancing_group:
        resp = NVMeoFClient(
            mgr, gw_group=gw_group, server_address=server_address
        ).stub.namespace_change_load_balancing_group(
            NVMeoFClient.pb2.namespace_change_load_balancing_group_req(
                subsystem_nqn=nqn, nsid=int(nsid), anagrpid=load_balancing_group
            )
        )
        if resp.status != 0:
            contains_failure = True

    if rw_ios_per_second or rw_mbytes_per_second or r_mbytes_per_second \
       or w_mbytes_per_second:
        resp = NVMeoFClient(
            mgr, gw_group=gw_group, server_address=server_address
        ).stub.namespace_set_qos_limits(
            NVMeoFClient.pb2.namespace_set_qos_req(
                subsystem_nqn=nqn,
                nsid=int(nsid),
                rw_ios_per_second=rw_ios_per_second,
                rw_mbytes_per_second=rw_mbytes_per_second,
                r_mbytes_per_second=r_mbytes_per_second,
                w_mbytes_per_second=w_mbytes_per_second,
            )
        )
        if resp.status != 0:
            contains_failure = True

    if trash_image is not None:
        resp = NVMeoFClient(
            mgr, gw_group=gw_group, server_address=server_address
        ).stub.namespace_set_rbd_trash_image(
            NVMeoFClient.pb2.namespace_set_rbd_trash_image_req(
                subsystem_nqn=nqn,
                nsid=int(nsid),
                trash_image=str_to_bool(trash_image)
            )
        )
        if resp.status != 0:
            contains_failure = True

    if location is not None:
        resp = NVMeoFClient(
            mgr, gw_group=gw_group,
            server_address=server_address
        ).stub.namespace_change_location(
            NVMeoFClient.pb2.namespace_change_location_req(
                subsystem_nqn=nqn,
                nsid=int(nsid),
                location=location
            )
        )
        if resp.status != 0:
            contains_failure = True

    if contains_failure:
        _flag_partial_failure()

    response = NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.list_namespaces(
        NVMeoFClient.pb2.list_namespaces_req(subsystem=nqn, nsid=int(nsid))
    )
    return response


@NvmeofCLICommand(
    "nvmeof namespace del",
    model.RequestStatus,
    alias="nvmeof ns del",
    success_message_template="Deleting namespace {nsid} from {nqn}: Successful")
@convert_to_model(model.RequestStatus)
@handle_nvmeof_error
def namespace_del(mgr,
                  nqn: str,
                  nsid: str,
                  force: Optional[str] = "false",
                  gw_group: Optional[str] = None,
                  server_address: Optional[str] = None,
                  traddr: Optional[str] = None,
                  ):
    """Delete an existing NVMeoF namespace"""
    server_address = resolve_nvmeof_server_address(
        server_address=server_address,
        traddr=traddr
    )
    return NVMeoFClient(
        mgr, gw_group=gw_group,
        server_address=server_address
    ).stub.namespace_delete(
        NVMeoFClient.pb2.namespace_delete_req(
            subsystem_nqn=nqn,
            nsid=int(nsid),
            i_am_sure=str_to_bool(force)
        )
    )
