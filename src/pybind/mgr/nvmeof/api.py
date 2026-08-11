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
from typing import Optional

from . import model
from .cli import NvmeofCLICommand
from .client import NVMeoFClient, handle_nvmeof_error
from .converters import convert_to_model
from .utils import resolve_nvmeof_server_address


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
