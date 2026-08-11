# pylint: disable=unexpected-keyword-arg

import functools
import logging
from typing import Any, Callable, Dict, List, Optional

from ..exceptions import DashboardException
from ..services.ceph_service import CephService
from .nvmeof_conf import NvmeofGatewaysConfig, is_mtls_enabled

logger = logging.getLogger("nvmeof_client")

try:
    # if the protobuf version is newer than what we generated with
    # proto file import will fail (because of differences between what's
    # available in centos and ubuntu).
    # this "hack" should be removed once we update both the
    # distros; centos and ubuntu.
    import os
    os.environ["PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION"] = "python"

    import grpc  # type: ignore
    import grpc._channel  # type: ignore
    from google.protobuf.message import Message  # type: ignore

    from nvmeof.client import NVMeoFClient as _NVMeoFClientCore
    from nvmeof.client import \
        handle_nvmeof_error as _handle_nvmeof_error_core  # type: ignore
    from nvmeof.errors import NvmeofError, NvmeofGatewayUnavailableError, \
        NvmeofStatusError

    from nvmeof.proto import gateway_pb2 as pb2  # type: ignore
    from nvmeof.proto import gateway_pb2_grpc as pb2_grpc  # type: ignore
except ImportError:
    grpc = None
else:

    class _DashboardConf:
        """Bridge the core client's registry lookups to the dashboard
        services, keeping the existing test patch points effective."""

        @staticmethod
        def get_service_info(group=None):
            return NvmeofGatewaysConfig.get_service_info(group)

        @staticmethod
        def get_gateways_config():
            return NvmeofGatewaysConfig.get_gateways_config()

        @staticmethod
        def is_mtls_enabled(service_name):
            return is_mtls_enabled(service_name)

        @staticmethod
        def get_tls_bundle(service_name, daemon_name):
            return NvmeofGatewaysConfig.get_nvmeof_tls_bundle(service_name,
                                                              daemon_name)

    class NVMeoFClient(_NVMeoFClientCore):
        def __init__(self, gw_group: Optional[str] = None,
                     server_address: Optional[str] = None):
            super().__init__(_DashboardConf, gw_group=gw_group,
                             server_address=server_address)

    import errno

    NVMeoFError2HTTP = {
        # errno errors
        errno.EPERM: 403,  # 1
        errno.ENOENT: 404,  # 2
        errno.EACCES: 403,  # 13
        errno.EEXIST: 409,  # 17
        errno.ENODEV: 404,  # 19
        # JSONRPC Spec: https://www.jsonrpc.org/specification#error_object
        -32602: 422,  # Invalid Params
        -32603: 500,  # Internal Error
    }

    def handle_nvmeof_error(func: Callable[..., Message]) -> Callable[..., Message]:
        """Adapt the module-level error handling to the REST API: the
        nvmeof module raises transport-agnostic errors, this maps them
        onto DashboardException with the right HTTP status."""
        core = _handle_nvmeof_error_core(func)

        @functools.wraps(func)
        def wrapper(*args, **kwargs) -> Message:
            try:
                return core(*args, **kwargs)
            except NvmeofGatewayUnavailableError as e:
                raise DashboardException(
                    msg=str(e),
                    code=e.code,
                    http_status_code=504,
                    component="nvmeof",
                )
            except NvmeofStatusError as e:
                raise DashboardException(
                    msg=str(e),
                    code=e.code,
                    http_status_code=NVMeoFError2HTTP.get(e.code, 400),  # type: ignore[arg-type]
                    component="nvmeof",
                )
            except NvmeofError as e:
                raise DashboardException(
                    msg=str(e),
                    code=e.code,
                    http_status_code=400,
                    component="nvmeof",
                )

        return wrapper

    def empty_response(func: Callable[..., Message]) -> Callable[..., None]:
        @functools.wraps(func)
        def wrapper(*args, **kwargs) -> None:
            func(*args, **kwargs)

        return wrapper


    # pylint: disable-next=redefined-outer-name
    def pick(field: str, first: bool = False,
             ) -> Callable[..., Callable[..., object]]:
        def decorator(func: Callable[..., Dict]) -> Callable[..., object]:
            @functools.wraps(func)
            def wrapper(*args, **kwargs) -> object:
                model = func(*args, **kwargs)
                field_to_ret = model[field]
                if first:
                    field_to_ret = field_to_ret[0]
                return field_to_ret
            return wrapper
        return decorator


def get_gateway_locations(pool: str, group: str, hosts: Optional[List[str]] = None):
    """
    Get locations for gateways in a service group using nvme-gw show command.

    Args:
        pool: The RBD pool name
        group: The NVMeoF gateway group name
        hosts: Optional list of hostnames to match locations to (in order)

    Returns:
        If hosts provided: List of location strings matching the order of hosts
        If hosts not provided: List of unique location strings (sorted)
    """
    try:  # pylint: disable=too-many-nested-blocks
        if not pool or not group:
            logger.warning('Pool or group not provided for location lookup: pool=%s, group=%s',
                           pool, group)
            return []

        result = CephService.send_command('mon', 'nvme-gw show',
                                          pool=pool, group=group)

        # Build a mapping of hostname to location
        host_to_location = {}
        if isinstance(result, dict):
            gateways = result.get('Created Gateways:', [])

            for gw in gateways:
                if isinstance(gw, dict):
                    gw_id = gw.get('gw-id', '')
                    location = gw.get('location', '')

                    # Extract hostname from gw-id (format: client.nvmeof.pool.group.hostname.xxx)
                    if gw_id:
                        parts = gw_id.split('.')
                        if len(parts) >= 5:
                            hostname = parts[4]
                            if location:
                                host_to_location[hostname] = location
                        else:
                            # If format is unexpected, log warning
                            logger.warning('Unexpected gateway ID format: %s', gw_id)

        # If hosts list provided, return locations in the same order
        if hosts:
            locations = []
            for host in hosts:
                # Get location for this host, empty string if not found
                location = host_to_location.get(host, '')
                if not location:
                    logger.debug('No location found for host: %s', host)
                locations.append(location)
            return locations

        # Otherwise return unique sorted locations
        unique_locations = set(host_to_location.values())
        return sorted(list(unique_locations))

    except Exception as e:  # pylint: disable=broad-except
        logger.error('Failed to get gateway locations for pool=%s, group=%s: %s',
                     pool, group, e)
        return []
