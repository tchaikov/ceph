# -*- coding: utf-8 -*-
"""Gateway commands.

Each function is a "ceph nvmeof ..." CLI command and doubles as the
implementation behind the dashboard's REST endpoints, which reach it
through NVMeoF.api_call(). The first argument is the nvmeof module
instance, which supplies the registry and mTLS lookups for the grpc
client.

Commands migrate here from dashboard/controllers/nvmeof.py one
resource at a time; connections are first.
"""
from functools import partial
from typing import List, Optional

from . import model
from .cli import NvmeofCLICommand
from .client import NVMeoFClient, handle_nvmeof_error
from .converters import convert_to_model
from .utils import escape_address_if_ipv6, format_host_updates, \
    resolve_nvmeof_server_address, str_to_bool


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
