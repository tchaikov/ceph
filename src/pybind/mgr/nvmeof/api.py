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
from typing import Optional

from . import model
from .cli import NvmeofCLICommand
from .client import NVMeoFClient, handle_nvmeof_error
from .converters import convert_to_model
from .utils import format_host_updates, resolve_nvmeof_server_address


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
