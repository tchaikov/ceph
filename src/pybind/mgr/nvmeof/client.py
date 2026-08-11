# -*- coding: utf-8 -*-
import errno
import functools
import logging
from typing import Callable, Dict, Optional

import grpc  # type: ignore
import grpc._channel  # type: ignore
from google.protobuf.message import Message  # type: ignore

from .errors import NvmeofError, NvmeofGatewayUnavailableError, \
    NvmeofInvalidInputError, NvmeofStatusError
from .proto import gateway_pb2 as pb2  # type: ignore
from .proto import gateway_pb2_grpc as pb2_grpc  # type: ignore

logger = logging.getLogger(__name__)


def handle_nvmeof_error(func: Callable[..., Message]) -> Callable[..., Message]:
    @functools.wraps(func)
    def wrapper(*args, **kwargs) -> Message:
        try:
            response = func(*args, **kwargs)
        except grpc._channel._InactiveRpcError as e:  # pylint: disable=protected-access
            raise NvmeofGatewayUnavailableError(e.details(), code=e.code())

        status = getattr(response, "status", None)
        error_message = getattr(response, "error_message", None)

        # Normalize the response so callers do not see a non-zero status
        # in a successful reply.
        if status == errno.EREMOTE:
            response.status = 0
            if hasattr(response, "error_message"):
                response.error_message = ""
            return response
        if status not in (None, 0):
            raise NvmeofStatusError(error_message or "NVMeoF operation failed",
                                    code=status)
        return response

    return wrapper


class NVMeoFClient(object):
    """grpc client for one NVMe-oF gateway.

    ``conf`` supplies the gateway registry lookups; the nvmeof module
    passes itself, the dashboard passes a bridge to its services. It
    must provide get_service_info(group), get_gateways_config(),
    is_mtls_enabled(service_name) and
    get_tls_bundle(service_name, daemon_name).
    """

    pb2 = pb2

    def __init__(self, conf, gw_group: Optional[str] = None,
                 server_address: Optional[str] = None):

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
                res = conf.get_service_info()
            else:
                res = conf.get_service_info(gw_group)
            if res is None:
                raise NvmeofError("Gateway group does not exist")
            service_name, self.gateway_addr, self.daemon_name = res
        except TypeError as e:
            raise NvmeofError(f'Unable to retrieve the gateway info: {e}')

        # While creating listener need to direct request to the gateway
        # address where listener is supposed to be added.
        if server_address:
            gateways_info = conf.get_gateways_config()
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
                raise NvmeofInvalidInputError(
                    f"No gateway found matching server address: {server_address}",
                    code='server_address_not_found',
                )
        enable_auth = conf.is_mtls_enabled(service_name)
        if enable_auth:
            tls_bundle = conf.get_tls_bundle(service_name, self.daemon_name)
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
