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

    MIGRATION_RETRY_SECONDS = 60

    def __init__(self, *args: Any, **kwargs: Any) -> None:
        super(NVMeoF, self).__init__(*args, **kwargs)
        self._shutdown_event = threading.Event()
        # guards the registry read-modify-write cycles and the collector
        # cache; remote() calls arrive on their own threads
        self._registry_lock = threading.Lock()
        self.nvmeof_collectors: Dict[str, Any] = {}

    def serve(self) -> None:
        # a mon error leaves the migration pending; keep retrying instead
        # of serving an empty registry until the next mgr failover
        while not self._shutdown_event.is_set():
            self._migrate_dashboard_gateways()
            if self.get_store(MIGRATION_DONE_KEY):
                break
            self._shutdown_event.wait(self.MIGRATION_RETRY_SECONDS)
        self._shutdown_event.wait()

    def shutdown(self) -> None:
        self._shutdown_event.set()
        with self._registry_lock:
            collectors = list(self.nvmeof_collectors.values())
            self.nvmeof_collectors.clear()
        for collector in collectors:
            self._close_collector(collector)

    @staticmethod
    def _close_collector(collector: Any) -> None:
        for client in getattr(collector, 'clients', {}).values():
            channel = getattr(client, 'channel', None)
            if channel is not None:
                channel.close()

    def _migrate_dashboard_gateways(self) -> None:
        """Adopt the gateway registry from the dashboard's KV store, once.

        The old key is left in place so that downgrading to a release
        whose dashboard still owns the registry finds its data where it
        expects it; it gets removed a release later.
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
            # mon unhappy; leave the flag unset so serve() retries
            logger.warning("cannot read %s from the mon (%d): %s",
                           DASHBOARD_STORE_KEY, r, err)
            return
        with self._registry_lock:
            # the lock keeps a gateway registration arriving over remote()
            # from being clobbered by the adopted copy
            try:
                parsed = json.loads(out)
            except json.decoder.JSONDecodeError as e:
                logger.error("not adopting malformed dashboard gateway "
                             "registry: %s", e)
            else:
                if isinstance(parsed, dict) and isinstance(
                        parsed.get('gateways'), dict):
                    self._adopt_gateways(parsed['gateways'])
                else:
                    logger.error("not adopting dashboard gateway "
                                 "registry of unexpected shape")
            self.set_store(MIGRATION_DONE_KEY, 'v1')

    def _adopt_gateways(self, adopted):
        """Merge the dashboard's registry into ours, keeping what is here.

        The store is not necessarily empty by the time this runs: a mon
        that is briefly unavailable defers the migration, and cephadm
        registers the gateways it can see over remote() meanwhile.
        Overwriting with the dashboard copy would drop those, and
        skipping the adoption because the store is non-empty would drop
        every gateway cephadm does not happen to re-register just then -
        a daemon that is down, or a group it is not reconciling. So
        merge, and let what is already registered win.
        """
        config = self._load_gateways()
        gateways = config.setdefault('gateways', {})
        adopted_daemons = 0
        for service_name, daemons in adopted.items():
            if not isinstance(daemons, list):
                # v19.2.0 wrote a dict here, carrying no daemon to keep
                continue
            current = gateways.get(service_name)
            if not isinstance(current, list):
                current = []
                gateways[service_name] = current
            known = {daemon.get('daemon_name') for daemon in current
                     if isinstance(daemon, dict)}
            for daemon in daemons:
                if not isinstance(daemon, dict):
                    continue
                if daemon.get('daemon_name') in known:
                    continue
                current.append(daemon)
                known.add(daemon.get('daemon_name'))
                adopted_daemons += 1
        self._save_gateways(config)
        logger.info("adopted %d gateway(s) from the dashboard registry",
                    adopted_daemons)

    def get_nvmeof_collector(self, session_id: str = '', ttl: int = 3600):
        import time

        from .top import NvmeofTopCollector
        STALE_POLL_THRESHOLD = 5  # expire if 5 poll intervals passed without activity

        def _expire_old_sessions():
            now = time.time()
            expired = []

            for _id in list(self.nvmeof_collectors.keys()):
                collector = self.nvmeof_collectors[_id]
                delay = collector.delay
                if delay < 1:  # for first poll iteration
                    expire_time = collector.timestamp + ttl
                else:
                    expire_time = collector.timestamp + (STALE_POLL_THRESHOLD * delay)
                if now > expire_time:
                    expired.append(_id)

            for _id in expired:
                collector = self.nvmeof_collectors.pop(_id, None)
                if collector is not None:
                    self._close_collector(collector)

        if NvmeofTopCollector is None:
            logger.error("nvmeof top collector is unavailable "
                         "(grpc bindings missing)")
            return None

        if not session_id:
            return None

        with self._registry_lock:
            _expire_old_sessions()
            if session_id not in self.nvmeof_collectors:
                self.nvmeof_collectors[session_id] = NvmeofTopCollector(self)
            collector = self.nvmeof_collectors[session_id]
            # count the fetch as activity so a concurrent expiry scan
            # cannot close a collector that is about to be used
            collector.timestamp = time.time()
            return collector

    # ---- gateway command bridge -----------------------------------------

    def api_call(self, method: str, **kwargs: Any) -> Any:
        """Invoke a gateway command on behalf of the dashboard.

        Exception types do not survive remote(), so failures come back
        as an error envelope the caller re-raises on its side.
        """
        api_mod = globals().get('api')
        if api_mod is None:
            # the guarded import at the end of this file failed
            return {'__nvmeof_error__': {
                'kind': 'internal', 'code': None,
                'msg': 'gateway commands are unavailable '
                       '(grpc bindings missing)'}}
        api_mod.reset_partial_failure()
        try:
            result = getattr(api_mod, method)(self, **kwargs)
        except NvmeofGatewayUnavailableError as e:
            return {'__nvmeof_error__': {'kind': 'unavailable',
                                         'msg': str(e), 'code': e.code}}
        except NvmeofStatusError as e:
            return {'__nvmeof_error__': {'kind': 'status',
                                         'msg': str(e), 'code': e.code}}
        except NvmeofError as e:
            return {'__nvmeof_error__': {'kind': 'input',
                                         'msg': str(e), 'code': e.code}}
        except Exception as e:  # pylint: disable=broad-except
            # a raw exception would cross remote() as an opaque
            # RuntimeError; report it as an internal error instead
            logger.exception("api_call %s failed", method)
            return {'__nvmeof_error__': {'kind': 'internal',
                                         'msg': str(e), 'code': None}}
        if api_mod.partial_failure_flagged():
            return {'__nvmeof_partial__': result}
        return result

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
        with self._registry_lock:
            return self._gateway_cfg_add(service_url, name, group,
                                         daemon_name)

    def _gateway_cfg_add(self, service_url: str, name: str, group: str,
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
                for gateway in existing_gateways:
                    if gateway.get('service_url') == service_url:
                        # persists a legacy entry backfilled above
                        self._save_gateways(config)
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
        with self._registry_lock:
            return self._gateway_cfg_rm(name, daemon_name)

    def _gateway_cfg_rm(self, name: str,
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
                                        if daemon.get('daemon_name') != daemon_name]

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

from . import top  # noqa: F401,E402 pylint: disable=wrong-import-position
