# -*- coding: utf-8 -*-
"""Exceptions raised by the NVMe-oF control plane.

These deliberately carry no HTTP semantics: the dashboard maps them to
DashboardException with the appropriate status code, while the CLI maps
them to an errno.
"""


class NvmeofError(Exception):
    """Base class; ``code`` is an errno, a JSONRPC error code or a grpc
    status, depending on the subclass."""

    def __init__(self, msg, code=None):
        super().__init__(msg)
        self.code = code


class NvmeofGatewayUnavailableError(NvmeofError):
    """The gateway could not be reached over grpc."""


class NvmeofStatusError(NvmeofError):
    """The gateway replied with a non-zero status."""


class NvmeofInvalidInputError(NvmeofError):
    """A request parameter is missing or inconsistent."""
