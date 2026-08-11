import errno
import json
import threading

import nvmeof.module as nvmeof_mod


def make_mgr():
    mgr = nvmeof_mod.NVMeoF.__new__(nvmeof_mod.NVMeoF)
    mgr._store = {}
    mgr._registry_lock = threading.Lock()
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
        gateways = stored_gateways(mgr)
        assert 'mine' in gateways
        # a non-empty store must not cost us the dashboard's entries
        assert 'theirs' in gateways
        assert mgr._store[nvmeof_mod.MIGRATION_DONE_KEY] == 'v1'

    def test_adopts_alongside_a_registration_that_beat_it(self):
        """cephadm can register over remote() before the migration runs.

        A mon that is briefly unavailable defers the migration, so the
        store is already populated on the retry; the gateways cephadm
        has not re-registered still have to be taken over.
        """
        mgr = make_mgr()
        mgr.gateway_cfg_add('http://1.2.3.4:5500', 'svc', 'grp', 'daemon-a')
        payload = json.dumps({'gateways': {
            'svc': [{'service_url': 'http://1.2.3.4:5500',
                     'group': 'grp', 'daemon_name': 'daemon-a'},
                    {'service_url': 'http://1.2.3.5:5500',
                     'group': 'grp', 'daemon_name': 'daemon-b'}],
            'other-svc': [{'service_url': 'http://1.2.3.6:5500',
                           'group': 'other', 'daemon_name': 'daemon-c'}],
        }})
        self.run_migration(mgr, (0, payload, ''))
        gateways = stored_gateways(mgr)
        # the daemon cephadm registered is kept, and not duplicated
        assert [d['daemon_name'] for d in gateways['svc']] == [
            'daemon-a', 'daemon-b']
        # the service cephadm never mentioned is adopted whole
        assert [d['daemon_name'] for d in gateways['other-svc']] == ['daemon-c']
        assert mgr._store[nvmeof_mod.MIGRATION_DONE_KEY] == 'v1'

    def test_registry_of_unexpected_shape_not_adopted(self):
        mgr = make_mgr()
        self.run_migration(mgr, (0, '{"gateways": []}', ''))
        assert nvmeof_mod.GATEWAYS_STORE_KEY not in mgr._store
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


class TestApiCallEnvelope:
    def test_wraps_module_errors(self):
        import nvmeof.api as api_mod
        from nvmeof.errors import NvmeofStatusError
        mgr = make_mgr()

        def boom(_mgr):
            raise NvmeofStatusError('gateway said no', code=2)

        api_mod.failing = boom
        try:
            res = nvmeof_mod.NVMeoF.api_call(mgr, 'failing')
        finally:
            del api_mod.failing
        assert res == {'__nvmeof_error__': {
            'kind': 'status', 'msg': 'gateway said no', 'code': 2}}
