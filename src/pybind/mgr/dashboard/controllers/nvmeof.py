# -*- coding: utf-8 -*-
# pylint: disable=too-many-lines
import logging
from typing import Any, Dict, List, Optional

import cherrypy
from orchestrator import OrchestratorError

from .. import mgr
from ..exceptions import DashboardException
from nvmeof import model
from ..security import Scope
from nvmeof.utils import resolve_nvmeof_server_address

from ..services.nvmeof_client import get_gateway_locations
from ..services.orchestrator import OrchClient
from ..tools import str_to_bool
from . import APIDoc, APIRouter, BaseController, CreatePermission, \
    DeletePermission, Endpoint, EndpointDoc, Param, ReadPermission, \
    RESTController, UIRouter, UpdatePermission

logger = logging.getLogger(__name__)

NVME_SCHEMA = {
    "available": (bool, "Is NVMe/TCP available?"),
    "message": (str, "Descriptions")
}

try:
    from nvmeof.converters import convert_to_model

    from ..services.nvmeof_client import NVMeoFClient, NVMeoFError2HTTP, \
        empty_response, handle_nvmeof_error, pick
except ImportError as e:
    logger.error("Failed to import NVMeoFClient and related components: %s", e)
else:

    _NVMEOF_KIND2HTTP = {'unavailable': 504, 'internal': 500}

    def _api(method, **kwargs):
        """Call a gateway command in the nvmeof module and re-raise its
        error envelope as DashboardException (exception types do not
        survive remote())."""
        try:
            res = mgr.remote('nvmeof', 'api_call', method, **kwargs)
        except ImportError as e:
            # the nvmeof module is not loaded (always-on, so this means
            # it failed to start)
            raise DashboardException(msg=str(e), code='nvmeof_module_unavailable',
                                     http_status_code=503, component='nvmeof')
        err = res.get('__nvmeof_error__') if isinstance(res, dict) else None
        if err:
            if err['kind'] == 'status':
                status = NVMeoFError2HTTP.get(err['code'], 400)
            else:
                status = _NVMEOF_KIND2HTTP.get(err['kind'], 400)
            raise DashboardException(msg=err['msg'], code=err['code'],
                                     http_status_code=status,
                                     component='nvmeof')
        if isinstance(res, dict) and '__nvmeof_partial__' in res:
            # some of the operations behind the call failed; the REST
            # contract reports this as 202
            cherrypy.response.status = 202
            return res['__nvmeof_partial__']
        return res

    @APIRouter("/nvmeof/gateway", Scope.NVME_OF)
    @APIDoc("NVMe-oF Gateway Management API", "NVMe-oF Gateway")
    class NVMeoFGateway(RESTController):
        @EndpointDoc(
            "Get information about the NVMeoF gateway",
            parameters={
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            }
        )
        def list(self, gw_group: Optional[str] = None, server_address: Optional[str] = None,
                 traddr: Optional[str] = None):
            return _api('gateway_info', gw_group=gw_group,
                        server_address=server_address, traddr=traddr)

        @ReadPermission
        @Endpoint('GET')
        def group(self):
            # pylint: disable=too-many-nested-blocks
            try:
                orch = OrchClient.instance()
                services_result = orch.services.list(service_type='nvmeof')

                if isinstance(services_result, tuple) and len(services_result) == 2:
                    services, count = services_result
                else:
                    services = services_result
                    count = len(services) if services else 0

                if services:
                    result = []
                    for service in services:
                        service_dict = service.to_json() if hasattr(service, 'to_json') else service

                        if isinstance(service_dict, dict):
                            # Extract pool and group from spec
                            spec = service_dict.get('spec', {})
                            if isinstance(spec, dict):
                                pool = spec.get('pool')
                                group = spec.get('group')

                                if pool and group:
                                    # Get hosts list from placement to match location order
                                    placement = service_dict.get('placement', {})
                                    hosts = placement.get('hosts', [])

                                    # Get locations in the same order as hosts
                                    locations = get_gateway_locations(pool, group, hosts)

                                    if 'placement' not in service_dict:
                                        service_dict['placement'] = {}

                                    service_dict['placement']['locations'] = locations

                        result.append(service_dict)

                    return (result, count)

                return services_result
            except OrchestratorError as e:
                # just return none instead of raising an exception
                # since we need this to work regardless of the status
                # of orchestrator in UI
                logger.error('Failed to fetch the gateway groups: %s', e)
                return None
            except Exception as e:  # pylint: disable=broad-except
                logger.error("Unexpected error in group(): %s", e, exc_info=True)
                return None

        @ReadPermission
        @Endpoint('GET', '/version')
        @EndpointDoc(
            "Get the version of the NVMeoF gateway",
            parameters={
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            }
        )
        def version(self, gw_group: Optional[str] = None, server_address: Optional[str] = None,
                    traddr: Optional[str] = None):
            return _api('gateway_version', gw_group=gw_group,
                        server_address=server_address, traddr=traddr)

        @ReadPermission
        @Endpoint('GET', '/log_level')
        @EndpointDoc(
            "Get NVMeoF gateway log level information",
            parameters={
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            }
        )
        def get_log_level(self, gw_group: Optional[str] = None,
                          server_address: Optional[str] = None,
                          traddr: Optional[str] = None):
            return _api('gateway_get_log_level', gw_group=gw_group,
                        server_address=server_address, traddr=traddr)

        @ReadPermission
        @Endpoint('PUT', '/log_level')
        @EndpointDoc(
            "Set NVMeoF gateway log levels",
            parameters={
                "log_level": Param(str, "Log level"),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            }
        )
        def set_log_level(self, log_level: str, gw_group: Optional[str] = None,
                          server_address: Optional[str] = None,
                          traddr: Optional[str] = None):
            return _api('gateway_set_log_level', log_level=log_level,
                        gw_group=gw_group, server_address=server_address,
                        traddr=traddr)

        @ReadPermission
        @Endpoint('GET', '/stats')
        @EndpointDoc(
            "Get NVMeoF statistics for the gateway",
            parameters={
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            }
        )
        def get_gw_stats(self, gw_group: Optional[str] = None,
                         server_address: Optional[str] = None,
                         traddr: Optional[str] = None):
            return _api('gateway_get_stats', gw_group=gw_group,
                        server_address=server_address, traddr=traddr)

        @ReadPermission
        @Endpoint('GET', '/listener_info')
        @EndpointDoc(
            "Get NVMeoF gateway's listeners info",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            }
        )
        def listener_info(self, nqn: str, gw_group: Optional[str] = None,
                          server_address: Optional[str] = None,
                          traddr: Optional[str] = None):
            return _api('gateway_listener_info', nqn=nqn, gw_group=gw_group,
                        server_address=server_address, traddr=traddr)

        @UpdatePermission
        @Endpoint('PUT', '/io_stats')
        @EndpointDoc(
            "Enable or disable IO statistics collection",
            parameters={
                "enabled": Param(bool, "Enable IO statistics collection"),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def set_io_stats_mode(self, enabled: bool, gw_group: Optional[str] = None,
                              server_address: Optional[str] = None,
                              traddr: Optional[str] = None):
            return _api('gateway_set_io_stats_mode', enabled=enabled,
                        gw_group=gw_group, server_address=server_address,
                        traddr=traddr)

        @ReadPermission
        @Endpoint('GET', '/thread_stats')
        @EndpointDoc(
            "Get NVMeoF thread statistics for the gateway",
            parameters={
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            }
        )
        def get_thread_stats(
            self, gw_group: Optional[str] = None,
            server_address: Optional[str] = None,
            traddr: Optional[str] = None
        ):
            return _api('gateway_get_thread_stats', gw_group=gw_group,
                        server_address=server_address, traddr=traddr)

        @UpdatePermission
        @Endpoint('PUT', '/refresh_network')
        @EndpointDoc(
            "Re-evaluate subsystem network masks and update auto-listeners for this gateway",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address"),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def refresh_network(self, nqn: str = "", gw_group: Optional[str] = None,
                            server_address: Optional[str] = None,
                            traddr: Optional[str] = None):
            return _api('gateway_refresh_network', nqn=nqn, gw_group=gw_group,
                        server_address=server_address, traddr=traddr)

    @APIRouter("/nvmeof/spdk", Scope.NVME_OF)
    @APIDoc("NVMe-oF SPDK Management API", "NVMe-oF SPDK")
    class NVMeoFSpdk(RESTController):
        @ReadPermission
        @Endpoint('GET', '/log_level')
        @EndpointDoc(
            "Get NVMeoF gateway spdk log levels",
            parameters={
                "all_log_flags": Param(bool, "Get all log flags", True, None),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            }
        )
        def get_spdk_log_level(
            self, all_log_flags: Optional[bool] = None,
            gw_group: Optional[str] = None, server_address: Optional[str] = None,
            traddr: Optional[str] = None
        ):
            return _api('spdk_log_level_get', all_log_flags=all_log_flags,
                        gw_group=gw_group, server_address=server_address,
                        traddr=traddr)

        @ReadPermission
        @Endpoint('PUT', '/log_level')
        @EndpointDoc(
            "Set NVMeoF gateway spdk log levels",
            parameters={
                "log_level": Param(str, "SPDK log level", True, None),
                "print_level": Param(str, "SPDK print level", True, None),
                "extra_log_flags": Param([str], "Extra log flags", True, None),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            }
        )
        def set_spdk_log_level(self, log_level: Optional[str] = None,
                               print_level: Optional[str] = None,
                               extra_log_flags: Optional[List[str]] = None,
                               gw_group: Optional[str] = None,
                               server_address: Optional[str] = None,
                               traddr: Optional[str] = None):
            return _api('spdk_log_level_set', log_level=log_level,
                        print_level=print_level,
                        extra_log_flags=extra_log_flags, gw_group=gw_group,
                        server_address=server_address, traddr=traddr)

        @ReadPermission
        @Endpoint('PUT', '/log_level/disable')
        @EndpointDoc(
            "Disable NVMeoF gateway spdk log",
            parameters={
                "extra_log_flags": Param([str], "Extra log flags", True, None),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            }
        )
        def disable_spdk_log_level(
            self, extra_log_flags: Optional[List[str]] = None,
            gw_group: Optional[str] = None,
            server_address: Optional[str] = None,
            traddr: Optional[str] = None
        ):
            return _api('spdk_log_level_disable',
                        extra_log_flags=extra_log_flags, gw_group=gw_group,
                        server_address=server_address, traddr=traddr)

    @APIRouter("/nvmeof/subsystem", Scope.NVME_OF)
    @APIDoc("NVMe-oF Subsystem Management API", "NVMe-oF Subsystem")
    class NVMeoFSubsystem(RESTController):
        @pick(field="subsystems")
        @EndpointDoc(
            "List all NVMeoF subsystems",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN to filter by", True, None),
                "serial_number": Param(str, "Serial number to filter by", True, None),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            }
        )
        def list(self, nqn: Optional[str] = None, serial_number: Optional[str] = None,
                 gw_group: Optional[str] = None, server_address: Optional[str] = None,
                 traddr: Optional[str] = None):
            return _api(
                'subsystem_list',
                nqn=nqn,
                serial_number=serial_number,
                gw_group=gw_group,
                server_address=server_address,
                traddr=traddr)

        @pick(field="subsystems", first=True)
        @EndpointDoc(
            "Get information from a specific NVMeoF subsystem",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def get(self, nqn: str, gw_group: Optional[str] = None,
                server_address: Optional[str] = None,
                traddr: Optional[str] = None):
            return _api(
                'subsystem_get',
                nqn=nqn,
                gw_group=gw_group,
                server_address=server_address,
                traddr=traddr)

        @empty_response
        @EndpointDoc(
            "Create a new NVMeoF subsystem",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "max_namespaces": Param(int, "Maximum number of namespaces", True, None),
                "no_group_append": Param(bool, "Do not append gateway group name to the NQN",
                                         True, False),
                "serial_number": Param(str, "Subsystem serial number", True, None),
                "dhchap_key": Param(str, "Subsystem DH-HMAC-CHAP key", True, None),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "network_mask": Param([str],
                                      "Network mask to automatically create listeners",
                                      True, None),
                "port": Param(int, "Port to use for the created listeners", True, None),
                "secure_listeners": Param(bool,
                                          "Make all the auto-listeners for this subsystem secure",
                                          True, False),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
                "model_name": Param(str, "Subsystem model name", True, None),
            },
        )
        def create(self, nqn: str,
                   max_namespaces: Optional[int] = None, no_group_append: Optional[bool] = False,
                   serial_number: Optional[str] = None, dhchap_key: Optional[str] = None,
                   gw_group: Optional[str] = None, server_address: Optional[str] = None,
                   network_mask: Optional[List[str]] = None,
                   port: Optional[int] = None, secure_listeners: Optional[bool] = False,
                   traddr: Optional[str] = None, model_name: Optional[str] = None):
            return _api(
                'subsystem_add',
                nqn=nqn,
                max_namespaces=max_namespaces,
                no_group_append=no_group_append,
                serial_number=serial_number,
                dhchap_key=dhchap_key,
                gw_group=gw_group,
                server_address=server_address,
                network_mask=network_mask,
                port=port,
                secure_listeners=secure_listeners,
                traddr=traddr,
                model_name=model_name)

        @empty_response
        @EndpointDoc(
            "Delete an existing NVMeoF subsystem",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "force": Param(bool, "Force delete", True, False),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def delete(self, nqn: str, force: Optional[str] = "false", gw_group: Optional[str] = None,
                   server_address: Optional[str] = None,
                   traddr: Optional[str] = None):
            return _api(
                'subsystem_del',
                nqn=nqn,
                force=force,
                gw_group=gw_group,
                server_address=server_address,
                traddr=traddr)

        @Endpoint('PUT', '{nqn}/change_key')
        @UpdatePermission
        @EndpointDoc(
            "Change subsystem inband authentication key",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "dhchap_key": Param(str, "Subsystem DH-HMAC-CHAP key"),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        @empty_response
        def change_key(self, nqn: str, dhchap_key: str, gw_group: Optional[str] = None,
                       server_address: Optional[str] = None,
                       traddr: Optional[str] = None):
            return _api(
                'subsystem_change_key',
                nqn=nqn,
                dhchap_key=dhchap_key,
                gw_group=gw_group,
                server_address=server_address,
                traddr=traddr)

        @EndpointDoc(
            "Delete subsystem inband authentication key",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        @empty_response
        def del_key(self, nqn: str, gw_group: Optional[str] = None,
                    server_address: Optional[str] = None,
                    traddr: Optional[str] = None):
            return _api(
                'subsystem_del_key',
                nqn=nqn,
                gw_group=gw_group,
                server_address=server_address,
                traddr=traddr)

        @empty_response
        @EndpointDoc(
            "Add subsystem network mask",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "network_mask": Param(str, "Network mask to add"),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def add_network(self, nqn: str, network_mask: str, gw_group: Optional[str] = None,
                        server_address: Optional[str] = None,
                        traddr: Optional[str] = None):
            return _api(
                'subsystem_add_network',
                nqn=nqn,
                network_mask=network_mask,
                gw_group=gw_group,
                server_address=server_address,
                traddr=traddr)

        @empty_response
        @EndpointDoc(
            "Delete subsystem network mask",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "network_mask": Param(str, "Network mask to remove"),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def del_network(self, nqn: str, network_mask: str, gw_group: Optional[str] = None,
                        server_address: Optional[str] = None,
                        traddr: Optional[str] = None):
            return _api(
                'subsystem_del_network',
                nqn=nqn,
                network_mask=network_mask,
                gw_group=gw_group,
                server_address=server_address,
                traddr=traddr)

        @empty_response
        @EndpointDoc(
            "Add a KMIP server endpoint to the subsystem",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "server_name": Param(str, "Name of the KMIP server the endpoint points to"),
                "address": Param(str, "KMIP server endpoint address", True, None),
                "port": Param(int, "KMIP server endpoint port", True, 5696),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def add_kmip_server_endpoint(self, nqn: str, server_name: str,
                                     address: Optional[str] = None,
                                     port: Optional[int] = 5696, gw_group: Optional[str] = None,
                                     server_address: Optional[str] = None,
                                     traddr: Optional[str] = None):
            return _api(
                'subsystem_add_kmip_server_endpoint',
                nqn=nqn,
                server_name=server_name,
                address=address,
                port=port,
                gw_group=gw_group,
                server_address=server_address,
                traddr=traddr)

        @empty_response
        @EndpointDoc(
            "Delete a KMIP server endpoint from the subsystem",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "server_name": Param(str, "Name of the KMIP server the endpoint points to"),
                "address": Param(str, "KMIP server endpoint address", True, None),
                "port": Param(int, "KMIP server endpoint port", True, 5696),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def del_kmip_server_endpoint(self, nqn: str, server_name: str,
                                     address: Optional[str] = None,
                                     port: Optional[int] = 5696, gw_group: Optional[str] = None,
                                     server_address: Optional[str] = None,
                                     traddr: Optional[str] = None):
            return _api(
                'subsystem_del_kmip_server_endpoint',
                nqn=nqn,
                server_name=server_name,
                address=address,
                port=port,
                gw_group=gw_group,
                server_address=server_address,
                traddr=traddr)

        @EndpointDoc(
            "List KMIP server endpoints for a subsystem or all subsystems",
            parameters={
                "nqn": Param(str, "Only show endpoints for this subsystem NQN", True, None),
                "server_name": Param(str, "Only show endpoints for this KMIP server name",
                                     True, None),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def list_eps(self, nqn: Optional[str] = None, server_name: Optional[str] = None,
                     gw_group: Optional[str] = None, server_address: Optional[str] = None,
                     traddr: Optional[str] = None):
            return _api(
                'subsystem_list_kmip_server_endpoints',
                nqn=nqn,
                server_name=server_name,
                gw_group=gw_group,
                server_address=server_address,
                traddr=traddr)

        @EndpointDoc(
            "Get NVMeoF subsystems",
            parameters={
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            }
        )
        def get_subsystems(
            self,
            gw_group: Optional[str] = None,
            server_address: Optional[str] = None,
            traddr: Optional[str] = None
        ):
            return _api(
                'get_subsystems',
                gw_group=gw_group,
                server_address=server_address,
                traddr=traddr)

    @APIRouter("/nvmeof/subsystem/{nqn}/listener", Scope.NVME_OF)
    @APIDoc("NVMe-oF Subsystem Listener Management API", "NVMe-oF Subsystem Listener")
    class NVMeoFListener(RESTController):
        @pick("listeners")
        @EndpointDoc(
            "List all NVMeoF listeners",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def list(self, nqn: str, gw_group: Optional[str] = None,
                 server_address: Optional[str] = None,
                 traddr: Optional[str] = None):
            return _api('listener_list', nqn=nqn, gw_group=gw_group,
                        server_address=server_address, traddr=traddr)

        @empty_response
        @EndpointDoc(
            "Create a new NVMeoF listener",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "host_name": Param(str, "NVMeoF hostname"),
                "traddr": Param(str, "NVMeoF transport address"),
                "trsvcid": Param(int, "NVMeoF transport service port", True, None),
                "adrfam": Param(int, "NVMeoF address family (0 - IPv4, 1 - IPv6)", True, 0),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "secure": Param(bool, "Use a secure channel", True, False),
                "force": Param(
                    bool,
                    "Allow contradiction in security with existing listeners",
                    True,
                    False
                ),
                "verify_host_name": Param(bool,
                                          "Fail if the host name doesn't match the "
                                          "gateway's host name",
                                          True, False),
            },
        )
        def create(
            self,
            nqn: str,
            host_name: str,
            traddr: str,
            trsvcid: Optional[int] = None,
            adrfam: int = 0,  # IPv4,
            gw_group: Optional[str] = None,
            server_address: Optional[str] = None,
            secure: Optional[bool] = False,
            force: Optional[bool] = False,
            verify_host_name: Optional[bool] = False,
        ):
            return _api('listener_add', nqn=nqn, host_name=host_name,
                        traddr=traddr, trsvcid=trsvcid, adrfam=adrfam,
                        gw_group=gw_group, server_address=server_address,
                        secure=secure, force=force,
                        verify_host_name=verify_host_name)

        @empty_response
        @EndpointDoc(
            "Delete an existing NVMeoF listener",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "host_name": Param(str, "NVMeoF hostname"),
                "traddr": Param(str, "NVMeoF transport address"),
                "trsvcid": Param(int, "NVMeoF transport service port"),
                "adrfam": Param(int, "NVMeoF address family (0 - IPv4, 1 - IPv6)", True, 0),
                "force": Param(
                    bool,
                    (
                        "Delete listener even if there are active connections "
                        "or host name doesn't match"
                    ),
                    True,
                    False
                ),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
            },
        )
        def delete(
            self,
            nqn: str,
            host_name: str,
            traddr: str,
            trsvcid: int,
            adrfam: int = 0,  # IPv4
            force: bool = False,
            gw_group: Optional[str] = None,
            server_address: Optional[str] = None
        ):
            return _api('listener_del', nqn=nqn, host_name=host_name,
                        traddr=traddr, trsvcid=trsvcid, adrfam=adrfam,
                        force=force, gw_group=gw_group,
                        server_address=server_address)

    @APIRouter("/nvmeof/subsystem/{nqn}/namespace", Scope.NVME_OF)
    @APIDoc("NVMe-oF Subsystem Namespace Management API", "NVMe-oF Subsystem Namespace")
    # pylint: disable=too-many-public-methods
    class NVMeoFNamespace(RESTController):
        @pick("namespaces")
        @EndpointDoc(
            "List all NVMeoF namespaces in a subsystem",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN", True, None),
                "nsid": Param(str, "NVMeoF Namespace ID to filter by", True, None),
                "uuid": Param(str, "NVMeoF Namespace UUID to filter by", True, None),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def list(self, nqn: Optional[str] = None, nsid: Optional[str] = None,
                 uuid: Optional[str] = None,
                 gw_group: Optional[str] = None, server_address: Optional[str] = None,
                 traddr: Optional[str] = None):
            return _api(
                'namespace_list',
                nqn=nqn,
                nsid=nsid,
                uuid=uuid,
                gw_group=gw_group,
                server_address=server_address,
                traddr=traddr)

        @pick("namespaces", first=True)
        @EndpointDoc(
            "Get info from specified NVMeoF namespace",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "nsid": Param(str, "NVMeoF Namespace ID"),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def get(self, nqn: str, nsid: str, gw_group: Optional[str] = None,
                server_address: Optional[str] = None,
                traddr: Optional[str] = None):
            return _api(
                'namespace_get',
                nqn=nqn,
                nsid=nsid,
                gw_group=gw_group,
                server_address=server_address,
                traddr=traddr)

        @ReadPermission
        @Endpoint('GET', '{nsid}/io_stats')
        @EndpointDoc(
            "Get IO stats from specified NVMeoF namespace",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "nsid": Param(str, "NVMeoF Namespace ID"),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def io_stats(self, nqn: str, nsid: str, gw_group: Optional[str] = None,
                     server_address: Optional[str] = None,
                     traddr: Optional[str] = None):
            return _api(
                'namespace_get_io_stats',
                nqn=nqn,
                nsid=nsid,
                gw_group=gw_group,
                server_address=server_address,
                traddr=traddr)

        @EndpointDoc(
            "Create a new NVMeoF namespace.",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "rbd_image_name": Param(str, "RBD image name"),
                "rados_namespace": Param(str, "RADOS namespace name", True, None),
                "rbd_pool": Param(str, "RBD pool name"),
                "rbd_data_pool": Param(str, "RBD data pool name", True, None),
                "nsid": Param(str, "Namespace ID", True, None),
                "uuid": Param(str, "UUID", True, None),
                "create_image": Param(bool, "Create RBD image"),
                "size": Param(int, "Deprecated. Use `rbd_image_size` instead"),
                "rbd_image_size": Param(int, "RBD image size"),
                "trash_image": Param(bool, "Trash the RBD image when namespace is removed"),
                "block_size": Param(int, "NVMeoF namespace block size"),
                "load_balancing_group": Param(int, "Load balancing group"),
                "force": Param(
                    bool,
                    "Force create namespace even it image is used by other namespace"
                ),
                "no_auto_visible": Param(
                    bool,
                    "Namespace will be visible only for the allowed hosts"
                ),
                "disable_auto_resize": Param(str, "Disable auto resize", True, None),
                "read_only": Param(str, "Read only namespace", True, None),
                "location": Param(str, "Gateway location for namespace", True, None),
                "encryption_format": Param([str],
                                           "Encryption format(s) to use, LUKS1 or LUKS2, "
                                           "separated by commas",
                                           True, None),
                "encryption_algorithm": Param(str,
                                              "Algorithm to use for encryption",
                                              True, None),
                "key_id": Param([str],
                                "Key ID(s) to use for encryption pass phrases, "
                                "separated by commas",
                                True, None),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        @convert_to_model(model.NamespaceCreation)
        @handle_nvmeof_error
        def create(
            self,
            nqn: str,
            rbd_image_name: str,
            rbd_pool: str = "rbd",
            rbd_data_pool: Optional[str] = None,
            nsid: Optional[str] = None,
            uuid: Optional[str] = None,
            create_image: Optional[bool] = False,
            size: Optional[int] = None,
            rbd_image_size: Optional[int] = None,
            trash_image: Optional[bool] = False,
            block_size: int = 512,
            load_balancing_group: Optional[int] = None,
            force: Optional[bool] = False,
            no_auto_visible: Optional[bool] = False,
            disable_auto_resize: Optional[bool] = False,
            read_only: Optional[bool] = False,
            location: Optional[str] = None,
            gw_group: Optional[str] = None,
            server_address: Optional[str] = None,
            traddr: Optional[str] = None,
            rados_namespace: Optional[str] = None,
            encryption_format: Optional[List[str]] = None,
            encryption_algorithm: Optional[str] = None,
            key_id: Optional[List[str]] = None,
        ):
            server_address = resolve_nvmeof_server_address(
                server_address=server_address,
                traddr=traddr
            )
            encryption_format = encryption_format or []
            key_id = key_id or []
            if len(encryption_format) != len(key_id):
                raise DashboardException(
                    msg="The number of key IDs should match the number of encryption formats",
                    code="key_ids_encryption_formats_mismatch",
                    http_status_code=400,
                    component="nvmeof",
                )
            enc_entries = [
                NVMeoFClient.pb2.encryption_entry(
                    format=f.strip().lower(),
                    key_id=k.strip()
                ) for f, k in zip(encryption_format, key_id)]
            enc_alg = encryption_algorithm.strip().lower() if encryption_algorithm else None
            return NVMeoFClient(
                gw_group=gw_group,
                server_address=server_address
            ).stub.namespace_add(
                NVMeoFClient.pb2.namespace_add_req(
                    subsystem_nqn=nqn,
                    nsid=int(nsid) if nsid else None,
                    uuid=uuid,
                    rbd_image_name=rbd_image_name,
                    rados_namespace_name=rados_namespace,
                    rbd_pool_name=rbd_pool,
                    rbd_data_pool_name=rbd_data_pool,
                    block_size=block_size,
                    create_image=create_image,
                    size=rbd_image_size or size,
                    trash_image=trash_image,
                    anagrpid=load_balancing_group,
                    force=force,
                    no_auto_visible=no_auto_visible,
                    disable_auto_resize=disable_auto_resize,
                    read_only=read_only,
                    location=location,
                    encryption_entries=enc_entries,
                    encryption_algorithm=enc_alg
                )
            )

        @ReadPermission
        @Endpoint('PUT', '{nsid}/set_qos')
        @EndpointDoc(
            "set QOS for specified NVMeoF namespace",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "nsid": Param(str, "NVMeoF Namespace ID"),
                "rw_ios_per_second": Param(int, "Read/Write IOPS"),
                "rw_mbytes_per_second": Param(int, "Read/Write MB/s"),
                "r_mbytes_per_second": Param(int, "Read MB/s"),
                "w_mbytes_per_second": Param(int, "Write MB/s"),
                "force": Param(
                    bool,
                    "Set QOS limits even if they were changed by RBD"
                ),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def set_qos(
            self,
            nqn: str,
            nsid: str,
            rw_ios_per_second: Optional[int] = None,
            rw_mbytes_per_second: Optional[int] = None,
            r_mbytes_per_second: Optional[int] = None,
            w_mbytes_per_second: Optional[int] = None,
            force: Optional[bool] = False,
            gw_group: Optional[str] = None,
            server_address: Optional[str] = None,
            traddr: Optional[str] = None
        ):
            return _api(
                'namespace_set_qos',
                nqn=nqn,
                nsid=nsid,
                rw_ios_per_second=rw_ios_per_second,
                rw_mbytes_per_second=rw_mbytes_per_second,
                r_mbytes_per_second=r_mbytes_per_second,
                w_mbytes_per_second=w_mbytes_per_second,
                force=force,
                gw_group=gw_group,
                server_address=server_address,
                traddr=traddr)

        @ReadPermission
        @Endpoint('PUT', '{nsid}/change_load_balancing_group')
        @EndpointDoc(
            "set the load balancing group for specified NVMeoF namespace",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "nsid": Param(str, "NVMeoF Namespace ID"),
                "load_balancing_group": Param(int, "Load balancing group", True, None),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def change_load_balancing_group(
            self,
            nqn: str,
            nsid: str,
            load_balancing_group: Optional[int] = None,
            gw_group: Optional[str] = None,
            server_address: Optional[str] = None,
            traddr: Optional[str] = None
        ):
            return _api(
                'namespace_change_load_balancing_group',
                nqn=nqn,
                nsid=nsid,
                load_balancing_group=load_balancing_group,
                gw_group=gw_group,
                server_address=server_address,
                traddr=traddr)

        @ReadPermission
        @Endpoint('PUT', '{nsid}/resize')
        @EndpointDoc(
            "resize the specified NVMeoF namespace",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "nsid": Param(str, "NVMeoF Namespace ID"),
                "rbd_image_size": Param(int, "RBD image size"),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        @convert_to_model(model.RequestStatus)
        @handle_nvmeof_error
        def resize(
            self,
            nqn: str,
            nsid: str,
            rbd_image_size: int,
            gw_group: Optional[str] = None,
            server_address: Optional[str] = None,
            traddr: Optional[str] = None
        ):
            server_address = resolve_nvmeof_server_address(
                server_address=server_address,
                traddr=traddr
            )
            mib = 1024 * 1024
            new_size_mib = int((rbd_image_size + mib - 1) / mib)

            return NVMeoFClient(
                gw_group=gw_group,
                server_address=server_address
            ).stub.namespace_resize(
                NVMeoFClient.pb2.namespace_resize_req(
                    subsystem_nqn=nqn, nsid=int(nsid), new_size=new_size_mib
                )
            )

        @ReadPermission
        @Endpoint('PUT', '{nsid}/add_host')
        @EndpointDoc(
            "Adds a host to the specified NVMeoF namespace",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "nsid": Param(str, "NVMeoF Namespace ID"),
                "host_nqn": Param(str, 'NVMeoF host NQN. Use "*" to allow any host.'),
                "force": Param(
                    bool,
                    "Allow adding the host to the namespace even if the host "
                    "has no access to the subsystem"
                ),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def add_host(
            self,
            nqn: str,
            nsid: str,
            host_nqn: str,
            force: Optional[bool] = None,
            gw_group: Optional[str] = None,
            server_address: Optional[str] = None,
            traddr: Optional[str] = None
        ):
            return _api(
                'namespace_add_host',
                nqn=nqn,
                nsid=nsid,
                host_nqn=host_nqn,
                force=force,
                gw_group=gw_group,
                server_address=server_address,
                traddr=traddr)

        @ReadPermission
        @Endpoint('PUT', '{nsid}/del_host')
        @EndpointDoc(
            "Removes a host from the specified NVMeoF namespace",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "nsid": Param(str, "NVMeoF Namespace ID"),
                "host_nqn": Param(str, 'NVMeoF host NQN. Use "*" to allow any host.'),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def del_host(
            self,
            nqn: str,
            nsid: str,
            host_nqn: str,
            gw_group: Optional[str] = None,
            server_address: Optional[str] = None,
            traddr: Optional[str] = None
        ):
            return _api(
                'namespace_del_host',
                nqn=nqn,
                nsid=nsid,
                host_nqn=host_nqn,
                gw_group=gw_group,
                server_address=server_address,
                traddr=traddr)

        @ReadPermission
        @Endpoint('PUT', '{nsid}/change_visibility')
        @EndpointDoc(
            "changes the visibility of the specified NVMeoF namespace to all or selected hosts",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "nsid": Param(str, "NVMeoF Namespace ID"),
                "auto_visible": Param(bool, 'True if visible to all hosts'),
                "force": Param(bool, 'True if visible to all hosts', True, False),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def change_visibility(
            self,
            nqn: str,
            nsid: str,
            auto_visible: str,
            force: Optional[bool] = False,
            gw_group: Optional[str] = None,
            server_address: Optional[str] = None,
            traddr: Optional[str] = None
        ):
            return _api(
                'namespace_change_visibility',
                nqn=nqn,
                nsid=nsid,
                auto_visible=auto_visible,
                force=force,
                gw_group=gw_group,
                server_address=server_address,
                traddr=traddr)

        @ReadPermission
        @Endpoint('PUT', '{nsid}/change_location')
        @EndpointDoc(
            "Change the location of the specified NVMeoF namespace",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "nsid": Param(str, "NVMeoF Namespace ID"),
                "location": Param(str, "Gateway location for namespace"),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def change_location(
            self,
            nqn: str,
            nsid: str,
            location: str,
            gw_group: Optional[str] = None,
            server_address: Optional[str] = None,
            traddr: Optional[str] = None
        ):
            return _api(
                'namespace_change_location',
                nqn=nqn,
                nsid=nsid,
                location=location,
                gw_group=gw_group,
                server_address=server_address,
                traddr=traddr)

        @ReadPermission
        @Endpoint('GET', 'list_hosts')
        @EndpointDoc(
            "List all NVMeoF namespaces with their allowed hosts",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN", True, None),
                "nsid": Param(str, "NVMeoF Namespace ID to filter by", True, None),
                "uuid": Param(str, "NVMeoF Namespace UUID to filter by", True, None),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def list_hosts(
            self, nqn: Optional[str] = None, nsid: Optional[str] = None,
            uuid: Optional[str] = None,
            gw_group: Optional[str] = None, server_address: Optional[str] = None,
            traddr: Optional[str] = None
        ):
            return _api(
                'namespace_list_hosts',
                nqn=nqn,
                nsid=nsid,
                uuid=uuid,
                gw_group=gw_group,
                server_address=server_address,
                traddr=traddr)

        @ReadPermission
        @Endpoint('GET', 'list_locations')
        @EndpointDoc(
            "List namespace distribution per site locations",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN", True, None),
                "nsid": Param(str, "NVMeoF Namespace ID to filter by", True, None),
                "uuid": Param(str, "NVMeoF Namespace UUID to filter by", True, None),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def list_locations(
            self, nqn: Optional[str] = None, nsid: Optional[str] = None,
            uuid: Optional[str] = None,
            gw_group: Optional[str] = None, server_address: Optional[str] = None,
            traddr: Optional[str] = None
        ):
            return _api(
                'namespace_list_locations',
                nqn=nqn,
                nsid=nsid,
                uuid=uuid,
                gw_group=gw_group,
                server_address=server_address,
                traddr=traddr)

        @ReadPermission
        @Endpoint('PUT', '{nsid}/set_auto_resize')
        @EndpointDoc(
            "Enable or disable namespace auto resize when RBD image is resized",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "nsid": Param(str, "NVMeoF Namespace ID"),
                "auto_resize_enabled": Param(
                    bool,
                    'Enable or disable auto resize of '
                    'namespace when RBD image is resized'
                ),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def set_auto_resize(
            self,
            nqn: str,
            nsid: str,
            auto_resize_enabled: bool,
            gw_group: Optional[str] = None,
            server_address: Optional[str] = None,
            traddr: Optional[str] = None
        ):
            return _api(
                'namespace_set_auto_resize',
                nqn=nqn,
                nsid=nsid,
                auto_resize_enabled=auto_resize_enabled,
                gw_group=gw_group,
                server_address=server_address,
                traddr=traddr)

        @ReadPermission
        @Endpoint('PUT', '{nsid}/set_rbd_trash_image')
        @EndpointDoc(
            "changes the trash image on delete of the specified NVMeoF \
                namespace to all or selected hosts",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "nsid": Param(str, "NVMeoF Namespace ID"),
                "rbd_trash_image_on_delete": Param(bool, 'True if active'),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def set_rbd_trash_image(
            self,
            nqn: str,
            nsid: str,
            rbd_trash_image_on_delete: str,
            gw_group: Optional[str] = None,
            server_address: Optional[str] = None,
            traddr: Optional[str] = None
        ):
            return _api(
                'namespace_set_rbd_trash_image',
                nqn=nqn,
                nsid=nsid,
                rbd_trash_image_on_delete=rbd_trash_image_on_delete,
                gw_group=gw_group,
                server_address=server_address,
                traddr=traddr)

        @ReadPermission
        @Endpoint('PUT', '{nsid}/refresh_size')
        @EndpointDoc(
            "refresh the specified NVMeoF namespace to current RBD image size",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "nsid": Param(str, "NVMeoF Namespace ID"),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def refresh_size(
            self,
            nqn: str,
            nsid: str,
            gw_group: Optional[str] = None,
            server_address: Optional[str] = None,
            traddr: Optional[str] = None
        ):
            return _api(
                'namespace_refresh_size',
                nqn=nqn,
                nsid=nsid,
                gw_group=gw_group,
                server_address=server_address,
                traddr=traddr)

        @ReadPermission
        @Endpoint('PUT', '{nsid}/unpin')
        @EndpointDoc(
            "Unpin namespace load balancing group",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "nsid": Param(str, "NVMeoF Namespace ID"),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def unpin_ns(
            self,
            nqn: str,
            nsid: str,
            gw_group: Optional[str] = None,
            server_address: Optional[str] = None,
            traddr: Optional[str] = None
        ):
            return _api(
                'namespace_unpin',
                nqn=nqn,
                nsid=nsid,
                gw_group=gw_group,
                server_address=server_address,
                traddr=traddr)

        @pick("namespaces", first=True)
        @EndpointDoc(
            "Update an existing NVMeoF namespace",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "nsid": Param(str, "NVMeoF Namespace ID"),
                "rbd_image_size": Param(int, "RBD image size"),
                "load_balancing_group": Param(int, "Load balancing group"),
                "rw_ios_per_second": Param(int, "Read/Write IOPS"),
                "rw_mbytes_per_second": Param(int, "Read/Write MB/s"),
                "r_mbytes_per_second": Param(int, "Read MB/s"),
                "w_mbytes_per_second": Param(int, "Write MB/s"),
                "trash_image": Param(bool, "Trash RBD image after removing namespace"),
                "location": Param(str, "Namespace location"),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def update(
            self,
            nqn: str,
            nsid: str,
            rbd_image_size: Optional[int] = None,
            load_balancing_group: Optional[int] = None,
            rw_ios_per_second: Optional[int] = None,
            rw_mbytes_per_second: Optional[int] = None,
            r_mbytes_per_second: Optional[int] = None,
            w_mbytes_per_second: Optional[int] = None,
            trash_image: Optional[bool] = None,
            location: Optional[str] = None,
            gw_group: Optional[str] = None,
            server_address: Optional[str] = None,
            traddr: Optional[str] = None,
        ):
            return _api(
                'namespace_update',
                nqn=nqn,
                nsid=nsid,
                rbd_image_size=rbd_image_size,
                load_balancing_group=load_balancing_group,
                rw_ios_per_second=rw_ios_per_second,
                rw_mbytes_per_second=rw_mbytes_per_second,
                r_mbytes_per_second=r_mbytes_per_second,
                w_mbytes_per_second=w_mbytes_per_second,
                trash_image=trash_image,
                location=location,
                gw_group=gw_group,
                server_address=server_address,
                traddr=traddr)

        @empty_response
        @EndpointDoc(
            "Delete an existing NVMeoF namespace",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "nsid": Param(str, "NVMeoF Namespace ID"),
                "force": Param(str, "Force remove the RBD image", True, False),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def delete(
            self,
            nqn: str,
            nsid: str,
            force: Optional[str] = "false",
            gw_group: Optional[str] = None,
            server_address: Optional[str] = None,
            traddr: Optional[str] = None,
        ):
            return _api(
                'namespace_del',
                nqn=nqn,
                nsid=nsid,
                force=force,
                gw_group=gw_group,
                server_address=server_address,
                traddr=traddr)

    @APIRouter("/nvmeof/subsystem/{nqn}/host", Scope.NVME_OF)
    @APIDoc("NVMe-oF Subsystem Host Allowlist Management API",
            "NVMe-oF Subsystem Host Allowlist")
    class NVMeoFHost(RESTController):
        @pick('hosts')
        @EndpointDoc(
            "List all allowed hosts for an NVMeoF subsystem",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def list(
            self, nqn: str,
            gw_group: Optional[str] = None, server_address: Optional[str] = None,
            traddr: Optional[str] = None
        ):
            return _api('host_list', nqn=nqn, gw_group=gw_group,
                        server_address=server_address, traddr=traddr)

        @empty_response
        @EndpointDoc(
            "Allow hosts to access an NVMeoF subsystem",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "host_nqn": Param(str, 'NVMeoF host NQN. Use "*" to allow any host.'),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def create(
            self, nqn: str, host_nqn: str, dhchap_key: Optional[str] = None,
            dhchap_controller_key: Optional[str] = None,
            psk: Optional[str] = None, gw_group: Optional[str] = None,
            server_address: Optional[str] = None,
            traddr: Optional[str] = None
        ):
            return _api('host_add', nqn=nqn, host_nqn=host_nqn,
                        dhchap_key=dhchap_key,
                        dhchap_controller_key=dhchap_controller_key,
                        psk=psk, gw_group=gw_group,
                        server_address=server_address, traddr=traddr)

        @empty_response
        @EndpointDoc(
            "Disallow hosts from accessing an NVMeoF subsystem",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "host_nqn": Param(str, 'NVMeoF host NQN. Use "*" to disallow any host.'),
                "force": Param(
                    bool,
                    "Delete the host even if it used in a namespace netmask",
                    True, False),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
                "keep_connections": Param(
                    bool,
                    "Do not disconnect existing connections from that host",
                    True, False),
            },
        )
        def delete(self, nqn: str, host_nqn: str, force: Optional[bool] = False,
                   gw_group: Optional[str] = None,
                   server_address: Optional[str] = None,
                   traddr: Optional[str] = None,
                   keep_connections: Optional[bool] = False):
            return _api('host_del', nqn=nqn, host_nqn=host_nqn, force=force,
                        gw_group=gw_group, server_address=server_address,
                        traddr=traddr, keep_connections=keep_connections)

        @empty_response
        @Endpoint('PUT', '{host_nqn}/change_key')
        @UpdatePermission
        @EndpointDoc(
            "Change host DH-HMAC-CHAP key",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "host_nqn": Param(str, 'NVMeoF host NQN'),
                "dhchap_key": Param(str, 'Host DH-HMAC-CHAP key'),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def change_key(
            self, nqn: str, host_nqn: str, dhchap_key: str,
            gw_group: Optional[str] = None, server_address: Optional[str] = None,
            traddr: Optional[str] = None
        ):
            return _api('host_change_key', nqn=nqn, host_nqn=host_nqn,
                        dhchap_key=dhchap_key, gw_group=gw_group,
                        server_address=server_address, traddr=traddr)

        @empty_response
        @Endpoint('PUT', '{host_nqn}/change_controller_key')
        @UpdatePermission
        @EndpointDoc(
            "Change host DH-HMAC-CHAP controller key",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "host_nqn": Param(str, 'NVMeoF host NQN'),
                "dhchap_controller_key": Param(str, 'Host DH-HMAC-CHAP controller key'),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def change_controller_key(
            self, nqn: str, host_nqn: str, dhchap_controller_key: str,
            gw_group: Optional[str] = None, server_address: Optional[str] = None,
            traddr: Optional[str] = None
        ):
            return _api('host_change_controller_key', nqn=nqn,
                        host_nqn=host_nqn,
                        dhchap_controller_key=dhchap_controller_key,
                        gw_group=gw_group, server_address=server_address,
                        traddr=traddr)

        @empty_response
        @Endpoint('PUT', '{host_nqn}/del_key')
        @UpdatePermission
        @EndpointDoc(
            "Delete host DH-HMAC-CHAP key",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "host_nqn": Param(str, 'NVMeoF host NQN.'),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def del_key(
            self, nqn: str, host_nqn: str, gw_group: Optional[str] = None,
            server_address: Optional[str] = None,
            traddr: Optional[str] = None
        ):
            return _api('host_del_key', nqn=nqn, host_nqn=host_nqn,
                        gw_group=gw_group, server_address=server_address,
                        traddr=traddr)

        @empty_response
        @Endpoint('PUT', '{host_nqn}/del_controller_key')
        @UpdatePermission
        @EndpointDoc(
            "Delete host DH-HMAC-CHAP controller key",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "host_nqn": Param(str, 'NVMeoF host NQN.'),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def del_controller_key(
            self, nqn: str, host_nqn: str, gw_group: Optional[str] = None,
            server_address: Optional[str] = None,
            traddr: Optional[str] = None
        ):
            return _api('host_del_controller_key', nqn=nqn, host_nqn=host_nqn,
                        gw_group=gw_group, server_address=server_address,
                        traddr=traddr)

    @APIRouter("/nvmeof/subsystem/{nqn}/connection", Scope.NVME_OF)
    @APIDoc("NVMe-oF Subsystem Connection Management API", "NVMe-oF Subsystem Connection")
    class NVMeoFConnection(RESTController):
        @pick("connections")
        @EndpointDoc(
            "List all NVMeoF Subsystem Connections",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def list(self, nqn: Optional[str] = None,
                 gw_group: Optional[str] = None, server_address: Optional[str] = None,
                 traddr: Optional[str] = None):
            return _api('connection_list', nqn=nqn, gw_group=gw_group,
                        server_address=server_address, traddr=traddr)

        @EndpointDoc(
            "Get the IO statistics for a connection",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "host_nqn": Param(str, "NVMeoF host NQN"),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def get_io_stats(
            self,
            nqn: str,
            host_nqn: str,
            gw_group: Optional[str] = None,
            server_address: Optional[str] = None,
            traddr: Optional[str] = None
        ):
            return _api('connection_get_io_statistics', nqn=nqn,
                        host_nqn=host_nqn, gw_group=gw_group,
                        server_address=server_address, traddr=traddr)

        @EndpointDoc(
            "Reset the IO statistics for a connection",
            parameters={
                "nqn": Param(str, "NVMeoF subsystem NQN"),
                "host_nqn": Param(str, "NVMeoF host NQN"),
                "gw_group": Param(str, "NVMeoF gateway group", True, None),
                "server_address": Param(str, "NVMeoF gateway address", True, None),
                "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
            },
        )
        def reset_io_stats(
            self,
            nqn: str,
            host_nqn: str,
            gw_group: Optional[str] = None,
            server_address: Optional[str] = None,
            traddr: Optional[str] = None
        ):
            return _api('connection_reset_io_statistics', nqn=nqn,
                        host_nqn=host_nqn, gw_group=gw_group,
                        server_address=server_address, traddr=traddr)

    @UIRouter('/nvmeof', Scope.NVME_OF)
    class NVMeoFTcpUI(BaseController):
        @Endpoint('GET', '/status')
        @ReadPermission
        @EndpointDoc("Display NVMe/TCP service status",
                     responses={200: NVME_SCHEMA})
        def status(self) -> dict:
            status: Dict[str, Any] = {'available': True, 'message': None}
            orch_backend = mgr.get_module_option_ex('orchestrator', 'orchestrator')
            if orch_backend == 'cephadm':
                orch = OrchClient.instance()
                orch_status = orch.status()
                if not orch_status['available']:
                    status["available"] = False
                    status["message"] = 'Orchestrator is not available'
            return status
        # UI API for adding one or more than one hosts to subsystem

        @Endpoint('POST', "/subsystem/{subsystem_nqn}/host")
        @EndpointDoc("Add one or more initiator hosts to an NVMeoF subsystem",
                     parameters={
                         'subsystem_nqn': (str, 'Subsystem NQN'),
                         "allow_all": Param(bool, 'Allow all hosts. Default is True.'),
                         "hosts": Param(List, 'List containg host nqn and dhchap key'),
                         "gw_group": Param(str, "NVMeoF gateway group"),
                         "server_address": Param(str, "NVMeoF gateway address", True, None),
                         "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
                     })
        @empty_response
        @handle_nvmeof_error
        @CreatePermission
        def add(self, subsystem_nqn: str,
                gw_group: str,
                hosts: Optional[list[dict]] = None,
                allow_all: bool = True,
                server_address: Optional[str] = None,
                traddr: Optional[str] = None):
            server_address = resolve_nvmeof_server_address(
                server_address=server_address,
                traddr=traddr
            )
            response = None

            if allow_all:
                return NVMeoFClient(gw_group=gw_group,
                                    server_address=server_address).stub.add_host(
                    NVMeoFClient.pb2.add_host_req(
                        subsystem_nqn=subsystem_nqn,
                        host_nqn="*",
                        dhchap_key=None
                    ))

            for h in (hosts or []):
                nqn = h["host_nqn"]
                key = h.get("dhchap_key")

                response = NVMeoFClient(gw_group=gw_group,
                                        server_address=server_address).stub.add_host(
                    NVMeoFClient.pb2.add_host_req(
                        subsystem_nqn=subsystem_nqn,
                        host_nqn=nqn,
                        dhchap_key=key
                    )
                )
                if response.status != 0:
                    return response
            return response

        # UI API for deleting one or more than one hosts to subsystem

        @Endpoint(method='DELETE', path="/subsystem/{subsystem_nqn}/host/{host_nqn}")
        @EndpointDoc("Remove on or more initiator hosts from an NVMeoF subsystem",
                     parameters={
                         "subsystem_nqn": Param(str, "NVMeoF subsystem NQN"),
                         "host_nqn": Param(str, 'Comma separated list of NVMeoF host NQN.'),
                         "gw_group": Param(str, "NVMeoF gateway group"),
                         "server_address": Param(str, "NVMeoF gateway address", True, None),
                         "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
                     })
        @empty_response
        @handle_nvmeof_error
        @DeletePermission
        def remove(self, subsystem_nqn: str,
                   host_nqn: str,
                   gw_group: str,
                   server_address: Optional[str] = None,
                   traddr: Optional[str] = None):
            server_address = resolve_nvmeof_server_address(
                server_address=server_address,
                traddr=traddr
            )
            response = None
            to_delete_nqns = host_nqn.split(',')

            for del_nqn in to_delete_nqns:
                response = NVMeoFClient(gw_group=gw_group,
                                        server_address=server_address).stub.remove_host(
                    NVMeoFClient.pb2.remove_host_req(subsystem_nqn=subsystem_nqn, host_nqn=del_nqn)
                )
                if response.status != 0:
                    return response
                logger.info("removed host %s from subsystem %s", del_nqn, subsystem_nqn)

            return response
        # UI API for adding one or more than one hosts to namespace

        @Endpoint('POST', "/namespace/{nsid}/host")
        @EndpointDoc("Add one or more initiator hosts to an NVMeoF subsystem",
                     parameters={
                         "subsystem_nqn": Param(str, "NVMeoF subsystem NQN"),
                         "nsid": Param(str, "NVMeoF Namespace ID"),
                         "host_nqn": Param(str, 'Comma separated list of NVMeoF host NQN.'),
                         "force": Param(
                             bool,
                             "Allow adding the host to the namespace even if the host "
                             "has no access to the subsystem"
                         ),
                         "gw_group": Param(str, "NVMeoF gateway group", True, None),
                         "server_address": Param(str, "NVMeoF gateway address", True, None),
                         "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
                     })
        @empty_response
        @handle_nvmeof_error
        @CreatePermission
        def add_namesapce_initiator(self, nsid: str,
                                    subsystem_nqn: str,
                                    gw_group: str,
                                    host_nqn: str = "",
                                    force: Optional[bool] = None,
                                    server_address: Optional[str] = None,
                                    traddr: Optional[str] = None):
            server_address = resolve_nvmeof_server_address(
                server_address=server_address,
                traddr=traddr
            )
            response = None
            all_host_nqns = host_nqn.split(',')
            force = str_to_bool(force) if force else None

            for nqn in all_host_nqns:
                response = NVMeoFClient(gw_group=gw_group,
                                        server_address=server_address).stub.namespace_add_host(
                    NVMeoFClient.pb2.namespace_add_host_req(subsystem_nqn=subsystem_nqn,
                                                            nsid=int(nsid),
                                                            host_nqn=nqn,
                                                            force=force)
                )
                if response.status != 0:
                    return response
            return response
        # UI API for deleting one or more than one hosts to namespace

        @Endpoint(method='DELETE', path="/namespace/{nsid}/host")
        @EndpointDoc("Remove on or more initiator hosts from an NVMeoF subsystem",
                     parameters={
                         "subsystem_nqn": Param(str, "NVMeoF subsystem NQN"),
                         "nsid": Param(str, "NVMeoF Namespace ID"),
                         "host_nqn": Param(str, 'Comma separated list of NVMeoF host NQN.'),
                         "gw_group": Param(str, "NVMeoF gateway group", True, None),
                         "server_address": Param(str, "NVMeoF gateway address", True, None),
                         "traddr": Param(str, "NVMeoF gateway address (deprecated)", True, None),
                     })
        @empty_response
        @handle_nvmeof_error
        @DeletePermission
        def remove_namespace_initiator(self,
                                       nsid: str,
                                       subsystem_nqn: str,
                                       host_nqn: str,
                                       gw_group: str,
                                       server_address: Optional[str] = None,
                                       traddr: Optional[str] = None):
            server_address = resolve_nvmeof_server_address(
                server_address=server_address,
                traddr=traddr
            )
            response = None
            to_delete_nqns = host_nqn.split(',')

            for del_nqn in to_delete_nqns:
                response = NVMeoFClient(gw_group=gw_group,
                                        server_address=server_address).stub.namespace_delete_host(
                    NVMeoFClient.pb2.namespace_delete_host_req(
                        subsystem_nqn=subsystem_nqn,
                        nsid=int(nsid),
                        host_nqn=del_nqn,
                    )
                )
                if response.status != 0:
                    return response
                logger.info("removed host %s from namespace %s", del_nqn, nsid)

            return response
