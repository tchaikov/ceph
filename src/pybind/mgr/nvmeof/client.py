# -*- coding: utf-8 -*-
import errno
import functools
import logging
from typing import Callable

import grpc  # type: ignore
import grpc._channel  # type: ignore
from google.protobuf.message import Message  # type: ignore

from .errors import NvmeofGatewayUnavailableError, NvmeofStatusError

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
