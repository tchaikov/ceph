# -*- coding: utf-8 -*-
import logging

from mgr_module import CLICheckNonemptyFileInput

from ..cli import DBCLICommand

logger = logging.getLogger(__name__)

# The gateway registry is owned by the nvmeof module; the commands below
# are kept as documented entry points and forward to it.


@DBCLICommand.Read('dashboard nvmeof-gateway-list')
def list_nvmeof_gateways(mgr):
    '''
    List NVMe-oF gateways
    '''
    return tuple(mgr.remote('nvmeof', 'gateway_cfg_list'))


@DBCLICommand.Write('dashboard nvmeof-gateway-add')
@CLICheckNonemptyFileInput(desc='NVMe-oF gateway configuration')
def add_nvmeof_gateway(mgr, inbuf, name: str, group: str, daemon_name: str):
    '''
    Add NVMe-oF gateway configuration. Gateway URL read from -i <file>
    '''
    return tuple(mgr.remote('nvmeof', 'gateway_cfg_add', service_url=inbuf,
                            name=name, group=group, daemon_name=daemon_name))


@DBCLICommand.Write('dashboard nvmeof-gateway-rm')
def remove_nvmeof_gateway(mgr, name: str, daemon_name: str = ''):
    '''
    Remove NVMe-oF gateway configuration
    '''
    return tuple(mgr.remote('nvmeof', 'gateway_cfg_rm', name=name,
                            daemon_name=daemon_name))
