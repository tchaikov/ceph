import errno
import json
import logging
import threading
from typing import Any, Dict, Optional, Tuple

import orchestrator
from orchestrator import OrchestratorError

from .cli import NVMeoFCLICommand
from .errors import NvmeofError, NvmeofGatewayUnavailableError, \
    NvmeofInvalidInputError, NvmeofStatusError
from mgr_module import MgrModule

logger = logging.getLogger(__name__)

POOL_NAME = ".nvmeof"

# module-local KV store keys
GATEWAYS_STORE_KEY = "gateways"
MIGRATION_DONE_KEY = "dashboard_config_migrated"
# the registry lived in the dashboard's store before this module owned it
DASHBOARD_STORE_KEY = "mgr/dashboard/_nvmeof_config"


class NVMeoF(orchestrator.OrchestratorClientMixin, MgrModule):
    CLICommand = NVMeoFCLICommand

    def __init__(self, *args: Any, **kwargs: Any) -> None:
        super(NVMeoF, self).__init__(*args, **kwargs)
        self._shutdown_event = threading.Event()

    def serve(self) -> None:
        self._migrate_dashboard_gateways()
        self._shutdown_event.wait()

    def shutdown(self) -> None:
        self._shutdown_event.set()

    def _migrate_dashboard_gateways(self) -> None:
        """Adopt the gateway registry from the dashboard's KV store, once.

        The dashboard keeps rewriting its copy on every read, so the old
        key is left in place; it gets removed a release after the
        dashboard side stops writing it.
        """
        if self.get_store(MIGRATION_DONE_KEY):
            return
        r, out, err = self.mon_command({
            'prefix': 'config-key get',
            'key': DASHBOARD_STORE_KEY,
        })
        if r == -errno.ENOENT:
            self.set_store(MIGRATION_DONE_KEY, 'v1')
            return
        if r != 0:
            # mon unhappy; leave the flag unset so the next start retries
            logger.warning("cannot read %s from the mon (%d): %s",
                           DASHBOARD_STORE_KEY, r, err)
            return
        if not self.get_store(GATEWAYS_STORE_KEY):
            try:
                json.loads(out)
            except json.decoder.JSONDecodeError as e:
                logger.error("not adopting malformed dashboard gateway "
                             "registry: %s", e)
            else:
                self.set_store(GATEWAYS_STORE_KEY, out)
                logger.info("adopted the gateway registry from the dashboard")
        self.set_store(MIGRATION_DONE_KEY, 'v1')

    # ---- gateway command bridge -----------------------------------------

    def api_call(self, method: str, **kwargs: Any) -> Any:
        """Invoke a gateway command on behalf of the dashboard.

        Exception types do not survive remote(), so failures come back
        as an error envelope the caller re-raises on its side.
        """
        from . import api
        try:
            return getattr(api, method)(self, **kwargs)
        except NvmeofGatewayUnavailableError as e:
            return {'__nvmeof_error__': {'kind': 'unavailable',
                                         'msg': str(e), 'code': e.code}}
        except NvmeofStatusError as e:
            return {'__nvmeof_error__': {'kind': 'status',
                                         'msg': str(e), 'code': e.code}}
        except NvmeofError as e:
            return {'__nvmeof_error__': {'kind': 'input',
                                         'msg': str(e), 'code': e.code}}

    # ---- registry lookups for the grpc client ---------------------------

    def get_service_info(self, group: Optional[str] = None) -> Optional[Any]:
        try:
            gateways = self._load_gateways().get('gateways', {})
            if not gateways:
                return None
            if group:
                return self._get_name_url_for_group(gateways, group)
            return self._get_default_service(gateways)
        except (KeyError, IndexError) as e:
            raise NvmeofError(f'NVMe-oF configuration is not set: {e}')

    def is_mtls_enabled(self, service_name: str) -> bool:
        try:
            services = orchestrator.raise_if_exception(
                self.describe_service(service_name=service_name))
            return services[0].spec.enable_auth
        except (OrchestratorError, IndexError):
            return False

    def get_tls_bundle(self, service_name: str,
                       daemon_name: str) -> Optional[Dict[str, str]]:
        try:
            return orchestrator.raise_if_exception(
                self.get_nvmeof_tls_bundle(service_name, daemon_name))
        except OrchestratorError:
            # just return None if any orchestrator error is raised
            # otherwise nvmeof api will raise this error and doesn't proceed.
            return None

    def _get_name_url_for_group(self, gateways: Dict[str, Any],
                                group: str) -> Optional[Any]:
        try:
            for service_name, svc_config in gateways.items():
                # get the group name of the service and match it against
                # the group name provided
                services = orchestrator.raise_if_exception(
                    self.describe_service(service_name=service_name))
                if group == services[0].spec.group:
                    daemons = orchestrator.raise_if_exception(
                        self.list_daemons(service_name=service_name))
                    running = [d.to_dict()['daemon_name'] for d in daemons
                               if d.to_dict()['status_desc'] == 'running']
                    config = next((c for c in svc_config
                                   if c['daemon_name'] in running), None)
                    if config:
                        return (service_name, config['service_url'],
                                config['daemon_name'])
            return None
        except OrchestratorError:
            return self._get_default_service(gateways)

    @staticmethod
    def _get_default_service(gateways: Dict[str, Any]) -> Optional[Any]:
        if gateways:
            gateway_keys = list(gateways.keys())
            # if there are more than 1 gateway, rather than chosing a random
            # gateway from any of the group, raise an exception to make it
            # clear that we need to specify the group name in the API request.
            if len(gateway_keys) > 1:
                raise NvmeofInvalidInputError(
                    "Multiple NVMe-oF gateway groups are configured. "
                    "Please specify the 'gw_group' parameter in the request.")
            service_name = gateway_keys[0]
            return (service_name, gateways[service_name][0]['service_url'],
                    gateways[service_name][0]['daemon_name'])
        return None

    # ---- gateway registry ------------------------------------------------
    #
    # The registry maps a service name to the gateways the dashboard and
    # the CLI talk to. cephadm writes it while deploying, the dashboard
    # reads it through remote(). Expected failures are reported as
    # (retval, stdout, stderr) tuples because exception types do not
    # survive a remote() call.

    def _load_gateways(self) -> Dict[str, Any]:
        raw = self.get_store(GATEWAYS_STORE_KEY, '{"gateways": {}}')
        return json.loads(raw)

    def _save_gateways(self, config: Dict[str, Any]) -> None:
        self.set_store(GATEWAYS_STORE_KEY, json.dumps(config))

    def get_gateways_config(self) -> Dict[str, Any]:
        return self._load_gateways()

    def gateway_cfg_list(self) -> Tuple[int, str, str]:
        return 0, json.dumps(self._load_gateways()), ''

    def gateway_cfg_add(self, service_url: str, name: str, group: str,
                        daemon_name: str) -> Tuple[int, str, str]:
        config = self._load_gateways()

        if name in config.get('gateways', {}):
            # the nvmeof dashboard config used in v19.2.0 saves the below
            # to a dict. Converting that to a list so that the upgrade
            # properly migrate it to the newer format, and also keep it empty.
            if isinstance(config['gateways'][name], dict):
                config['gateways'][name] = []
            else:
                existing_gateways = config['gateways'][name]
                for gateway in existing_gateways:
                    if 'daemon_name' not in gateway:
                        gateway['daemon_name'] = daemon_name
                        break
                    if gateway['service_url'] == service_url:
                        return 0, 'Success', ''

        new_gateway = {
            'service_url': service_url,
            'group': group,
            'daemon_name': daemon_name
        }

        if name in config.get('gateways', {}):
            config['gateways'][name].append(new_gateway)
        else:
            config['gateways'][name] = [new_gateway]

        self._save_gateways(config)
        return 0, 'Success', ''

    def gateway_cfg_rm(self, name: str,
                       daemon_name: Optional[str] = None) -> Tuple[int, str, str]:
        config = self._load_gateways()
        if name not in config['gateways']:
            return (-errno.EINVAL, '',
                    "NVMe-oF gateway '{}' does not exist".format(name))

        if not daemon_name:
            del config['gateways'][name]
        else:
            # remove the daemon from the list of gateways
            config['gateways'][name] = [daemon for daemon in config['gateways'][name]
                                        if daemon['daemon_name'] != daemon_name]

            # if there are no more daemons in the list, remove the gateway
            if not config['gateways'][name]:
                del config['gateways'][name]

        self._save_gateways(config)
        return 0, 'Success', ''

    def _pool_exists(self, pool_name: str) -> bool:
        logger.info(f"checking if pool {pool_name} exists")
        pool_exists = self.pool_exists(pool_name)
        if pool_exists:
            logger.info(f"pool {pool_name} already exists")
        else:
            logger.info(f"pool {pool_name} doesn't exist")
        return pool_exists

    def _create_pool(self, pool_name: str) -> None:
        try:
            self.create_pool(pool_name)
            logger.info(f"Pool '{pool_name}' created.")
        except Exception:
            logger.error(f"Error creating pool '{pool_name}'", exc_info=True)
            raise

    def _enable_application(self, pool_name: str, application_name: str) -> None:
        try:
            self.appify_pool(pool_name, application_name)
            logger.info(f"'{application_name}' application enabled on pool '{pool_name}'.")
        except Exception:
            logger.error(
                f"Failed to enable '{application_name}' application on '{pool_name}'",
                exc_info=True
            )
            raise

    def create_pool_if_not_exists(self) -> None:
        if not self._pool_exists(POOL_NAME):
            self._create_pool(POOL_NAME)
            self._enable_application(POOL_NAME, 'nvmeof-meta')


try:
    from . import api  # noqa: F401,E402 pylint: disable=wrong-import-position
except ImportError as e:
    logger.warning("gateway commands unavailable: %s", e)
