# -*- coding: utf-8 -*-
from typing import Any, Dict, List, Optional, Union

from .errors import NvmeofInvalidInputError

MULTIPLES = ['', "K", "M", "G", "T", "P"]
UNITS = {
    f"{prefix}{suffix}": 1024 ** mult
    for mult, prefix in enumerate(MULTIPLES)
    for suffix in ['', 'B', 'iB']
    if not (prefix == '' and suffix == 'iB')
}


def escape_address_if_ipv6(addr: str) -> str:
    ret_addr = addr
    if ":" in addr and not addr.strip().startswith("["):
        ret_addr = f"[{addr}]"
    return ret_addr


def _normalize_to_list(value: Any) -> List[Any]:
    if value is None:
        return []
    if isinstance(value, list):
        return value
    return [value]


def format_host_updates(args: Dict[str, Any],
                        template_wildcard: str,
                        template_item: str,
                        host_arg: str = 'host_nqn',
                        nqn_arg: str = 'nqn',
                        wildcard_value: str = '*') -> str:
    nqn = args.get(nqn_arg)
    hosts = _normalize_to_list(args.get(host_arg))

    messages: List[str] = []
    for host in hosts:
        if host == wildcard_value:
            messages.append(template_wildcard.format(nqn=nqn))
        else:
            messages.append(template_item.format(nqn=nqn, host_nqn=host))
    return "\n".join(messages)


def convert_to_bytes(size: Union[int, str], default_unit=None):
    if isinstance(size, int):
        number = size
        size = str(size)
    else:
        num_str = ''.join(filter(str.isdigit, size))
        number = int(num_str)
    unit_str = ''.join(filter(str.isalpha, size))
    if not unit_str:
        if not default_unit:
            raise ValueError("default unit was not provided")
        unit_str = default_unit

    if unit_str in UNITS:
        return number * UNITS[unit_str]
    raise ValueError(f"Invalid unit: {unit_str}")


def convert_from_bytes(num_in_bytes):
    units = ['B', 'KB', 'MB', 'GB', 'TB', 'PB']
    size = float(num_in_bytes)
    unit_index = 0

    while size >= 1024 and unit_index < len(units) - 1:
        size /= 1024
        unit_index += 1

    # Round to no decimal if it's an integer, otherwise show 1 decimal place
    if size.is_integer():
        size_str = f"{int(size)}"
    else:
        size_str = f"{size:.1f}"

    return f"{size_str}{units[unit_index]}"


def resolve_nvmeof_server_address(
    *,
    server_address: Optional[str] = None,
    traddr: Optional[str] = None,
    require: bool = False,
) -> Optional[str]:
    sa = (server_address or "").strip() or None
    ta = (traddr or "").strip() or None

    if sa and ta:
        raise NvmeofInvalidInputError(
            "Pass either 'server_address' or deprecated 'traddr', not both.",
            code="server_address_and_traddr_mutually_exclusive",
        )

    resolved = sa or ta

    if require and not resolved:
        raise NvmeofInvalidInputError(
            "Missing required gateway address: "
            "'server_address' (or deprecated 'traddr').",
            code="missing_server_address",
        )

    return resolved
