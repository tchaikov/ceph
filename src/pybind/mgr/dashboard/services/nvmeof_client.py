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

    from .proto import gateway_pb2 as pb2  # type: ignore
    from .proto import gateway_pb2_grpc as pb2_grpc  # type: ignore
except ImportError:
    grpc = None
else:

    class NVMeoFClient(object):
        pb2 = pb2

        def __init__(self, gw_group: Optional[str] = None, server_address: Optional[str] = None):

            def encode_tls_bundle(bundle: Dict[str, str]) -> Dict[str, bytes]:
                """Encode TLS bundle string values to bytes for gRPC."""
                encoded: Dict[str, bytes] = {}
                for key, value in bundle.items():
                    if isinstance(value, str):
                        encoded[key] = value.encode('utf-8')
                    else:
                        encoded[key] = value
                return encoded

            logger.info("Initiating nvmeof gateway connection...")
            try:
                if not gw_group:
                    res = NvmeofGatewaysConfig.get_service_info()
                else:
                    res = NvmeofGatewaysConfig.get_service_info(gw_group)
                if res is None:
                    raise DashboardException("Gateway group does not exist")
                service_name, self.gateway_addr, self.daemon_name = res
            except TypeError as e:
                raise DashboardException(
                    f'Unable to retrieve the gateway info: {e}'
                )

            # While creating listener need to direct request to the gateway
            # address where listener is supposed to be added.
            if server_address:
                gateways_info = NvmeofGatewaysConfig.get_gateways_config()
                matched_gateway = next(
                    (
                        gateway
                        for gateways in gateways_info['gateways'].values()
                        for gateway in gateways
                        if server_address in gateway['service_url']
                    ),
                    None
                )
                if matched_gateway:
                    self.daemon_name = matched_gateway.get('daemon_name')
                    self.gateway_addr = matched_gateway.get('service_url')
                    logger.debug("Gateway address set to: %s", self.gateway_addr)
                else:
                    raise DashboardException(
                        msg=f"No gateway found matching server address: {server_address}",
                        code='server_address_not_found',
                        component='nvmeof',
                        http_status_code=400
                    )
            enable_auth = is_mtls_enabled(service_name)
            if enable_auth:
                tls_bundle = NvmeofGatewaysConfig.get_nvmeof_tls_bundle(service_name,
                                                                        self.daemon_name)
                if tls_bundle:
                    logger.info('Securely connecting to: %s', self.gateway_addr)
                    encoded_tls_bundle = encode_tls_bundle(tls_bundle)
                    credentials = grpc.ssl_channel_credentials(
                        root_certificates=encoded_tls_bundle['server_cert'],
                        private_key=encoded_tls_bundle['client_key'],
                        certificate_chain=encoded_tls_bundle['client_cert'],
                    )
                    self.channel = grpc.secure_channel(self.gateway_addr, credentials)
                else:
                    self.channel = None
                    logger.error("Cannot obtain nvmeof TLS bundle for the service %s (gw: %s)",
                                 service_name, self.gateway_addr)
            else:
                logger.info("Insecurely connecting to: %s", self.gateway_addr)
                self.channel = grpc.insecure_channel(self.gateway_addr)
            self.service_name = service_name
            if self.channel is not None:
                self.stub = pb2_grpc.GatewayStub(self.channel)

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
        @functools.wraps(func)
        def wrapper(*args, **kwargs) -> Message:
            try:
                response = func(*args, **kwargs)
            except grpc._channel._InactiveRpcError as e:  # pylint: disable=protected-access
                raise DashboardException(
                    msg=e.details(),
                    code=e.code(),
                    http_status_code=504,
                    component="nvmeof",
                )

            status = getattr(response, "status", None)
            error_message = getattr(response, "error_message", None)

            # Normalize the response so callers do not see a non-zero status in
            # a successful HTTP response.
            if status == errno.EREMOTE:
                response.status = 0
                if hasattr(response, "error_message"):
                    response.error_message = ""
                return response
            if status not in (None, 0):
                raise DashboardException(
                    msg=error_message or "NVMeoF operation failed",
                    code=status,
                    http_status_code=NVMeoFError2HTTP.get(status, 400),  # type: ignore[arg-type]
                    component="nvmeof",
                )
            return response

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
