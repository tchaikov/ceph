import errno
import json

import nvmeof.module as nvmeof_mod


def make_mgr():
    mgr = nvmeof_mod.NVMeoF.__new__(nvmeof_mod.NVMeoF)
    mgr._store = {}
    mgr.get_store = lambda key, default=None: mgr._store.get(key, default)
    mgr.set_store = lambda key, val: mgr._store.__setitem__(key, val)
    return mgr


def stored_gateways(mgr):
    return json.loads(mgr._store[nvmeof_mod.GATEWAYS_STORE_KEY])['gateways']


class TestGatewayRegistry:
    def test_add_gateway(self):
        mgr = make_mgr()
        r, out, err = mgr.gateway_cfg_add(service_url='http://nvmf:port',
                                          name='nvmeof.pool.group',
                                          group='group',
                                          daemon_name='nvmeof_daemon')
        assert (r, out, err) == (0, 'Success', '')
        assert stored_gateways(mgr) == {
            'nvmeof.pool.group': [{
                'group': 'group',
                'daemon_name': 'nvmeof_daemon',
                'service_url': 'http://nvmf:port'
            }]
        }

    def test_add_converts_legacy_dict_config(self):
        mgr = make_mgr()
        mgr._store[nvmeof_mod.GATEWAYS_STORE_KEY] = json.dumps({
            'gateways': {'nvmeof.pool': {'service_url': 'http://nvmf:port'}}
        })
        mgr.gateway_cfg_add(service_url='http://nvmf:port', name='nvmeof.pool',
                            group='', daemon_name='nvmeof_daemon')
        assert stored_gateways(mgr) == {
            'nvmeof.pool': [{
                'daemon_name': 'nvmeof_daemon',
                'group': '',
                'service_url': 'http://nvmf:port'
            }]
        }

    def test_add_to_existing_service(self):
        mgr = make_mgr()
        mgr.gateway_cfg_add(service_url='http://nvmf:port', name='nvmeof.pool',
                            group='', daemon_name='nvmeof_daemon')
        mgr.gateway_cfg_add(service_url='http://nvmf-2:port', name='nvmeof.pool',
                            group='', daemon_name='nvmeof_daemon_2')
        assert stored_gateways(mgr)['nvmeof.pool'] == [
            {'daemon_name': 'nvmeof_daemon', 'group': '',
             'service_url': 'http://nvmf:port'},
            {'daemon_name': 'nvmeof_daemon_2', 'group': '',
             'service_url': 'http://nvmf-2:port'},
        ]

    def test_add_second_service(self):
        mgr = make_mgr()
        mgr.gateway_cfg_add(service_url='http://nvmf:port', name='nvmeof.pool',
                            group='', daemon_name='nvmeof_daemon')
        mgr.gateway_cfg_add(service_url='http://nvmf-2:port',
                            name='nvmeof2.pool.group',
                            group='group', daemon_name='nvmeof_daemon_2')
        assert set(stored_gateways(mgr)) == {'nvmeof.pool', 'nvmeof2.pool.group'}

    def test_rm_gateway(self):
        mgr = make_mgr()
        mgr.gateway_cfg_add(service_url='http://nvmf:port',
                            name='nvmeof.pool.group',
                            group='group', daemon_name='nvmeof_daemon')
        r, out, err = mgr.gateway_cfg_rm(name='nvmeof.pool.group')
        assert (r, out, err) == (0, 'Success', '')
        assert stored_gateways(mgr) == {}

    def test_rm_single_daemon_keeps_others(self):
        mgr = make_mgr()
        mgr.gateway_cfg_add(service_url='http://nvmf:port', name='nvmeof.pool',
                            group='', daemon_name='d1')
        mgr.gateway_cfg_add(service_url='http://nvmf-2:port', name='nvmeof.pool',
                            group='', daemon_name='d2')
        mgr.gateway_cfg_rm(name='nvmeof.pool', daemon_name='d1')
        assert [g['daemon_name'] for g in stored_gateways(mgr)['nvmeof.pool']] == ['d2']

    def test_rm_unknown_gateway(self):
        mgr = make_mgr()
        r, out, err = mgr.gateway_cfg_rm(name='no-such')
        assert r == -errno.EINVAL
        assert 'does not exist' in err

    def test_list(self):
        mgr = make_mgr()
        r, out, err = mgr.gateway_cfg_list()
        assert r == 0
        assert json.loads(out) == {'gateways': {}}


class TestDashboardMigration:
    def run_migration(self, mgr, mon_result):
        mgr.mon_command = lambda cmd: mon_result
        mgr._migrate_dashboard_gateways()

    def test_adopts_dashboard_config(self):
        mgr = make_mgr()
        payload = json.dumps({'gateways': {'svc': []}})
        self.run_migration(mgr, (0, payload, ''))
        assert mgr._store[nvmeof_mod.GATEWAYS_STORE_KEY] == payload
        assert mgr._store[nvmeof_mod.MIGRATION_DONE_KEY] == 'v1'

    def test_nothing_to_migrate(self):
        mgr = make_mgr()
        self.run_migration(mgr, (-errno.ENOENT, '', 'not found'))
        assert nvmeof_mod.GATEWAYS_STORE_KEY not in mgr._store
        assert mgr._store[nvmeof_mod.MIGRATION_DONE_KEY] == 'v1'

    def test_does_not_clobber_own_store(self):
        mgr = make_mgr()
        mgr._store[nvmeof_mod.GATEWAYS_STORE_KEY] = '{"gateways": {"mine": []}}'
        self.run_migration(mgr, (0, '{"gateways": {"theirs": []}}', ''))
        assert 'mine' in stored_gateways(mgr)
        assert mgr._store[nvmeof_mod.MIGRATION_DONE_KEY] == 'v1'

    def test_malformed_payload_not_adopted(self):
        mgr = make_mgr()
        self.run_migration(mgr, (0, 'not json', ''))
        assert nvmeof_mod.GATEWAYS_STORE_KEY not in mgr._store
        assert mgr._store[nvmeof_mod.MIGRATION_DONE_KEY] == 'v1'

    def test_mon_error_retries_next_start(self):
        mgr = make_mgr()
        self.run_migration(mgr, (-errno.EIO, '', 'mon unhappy'))
        assert nvmeof_mod.MIGRATION_DONE_KEY not in mgr._store

    def test_idempotent(self):
        mgr = make_mgr()
        mgr._store[nvmeof_mod.MIGRATION_DONE_KEY] = 'v1'
        calls = []
        mgr.mon_command = lambda cmd: calls.append(cmd)
        mgr._migrate_dashboard_gateways()
        assert calls == []
