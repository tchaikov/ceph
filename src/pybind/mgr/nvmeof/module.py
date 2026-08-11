import errno
import json
import logging
import threading
from typing import Any, Dict, Optional, Tuple

from .cli import NVMeoFCLICommand
from mgr_module import MgrModule

logger = logging.getLogger(__name__)

POOL_NAME = ".nvmeof"

# module-local KV store keys
GATEWAYS_STORE_KEY = "gateways"
MIGRATION_DONE_KEY = "dashboard_config_migrated"
# the registry lived in the dashboard's store before this module owned it
DASHBOARD_STORE_KEY = "mgr/dashboard/_nvmeof_config"


class NVMeoF(MgrModule):
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
