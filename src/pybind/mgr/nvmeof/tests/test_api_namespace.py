# -*- coding: utf-8 -*-
"""Tests for the namespace commands that carry logic beyond a plain
stub call: size conversion, input validation, host/location listings
and the multi-step update with its partial-failure reporting."""
import threading
from unittest.mock import MagicMock, patch

import pytest

import nvmeof.api as api
import nvmeof.module as nvmeof_mod
from nvmeof.errors import NvmeofInvalidInputError
from nvmeof.proto import gateway_pb2 as pb2


def make_client(response_by_method):
    """A stand-in for NVMeoFClient whose stub returns canned pb2
    responses; also records the requests it was driven with."""
    client = MagicMock()
    instance = client.return_value
    for method, response in response_by_method.items():
        getattr(instance.stub, method).return_value = response
    client.pb2 = pb2
    instance.pb2 = pb2
    return client


class TestNamespaceAdd:
    def test_size_is_converted_to_bytes(self):
        client = make_client({
            'namespace_add': pb2.nsid_status(status=0, error_message='', nsid=7)})
        with patch.object(api, 'NVMeoFClient', client):
            res = api.namespace_add(None, nqn='nqn.x', rbd_pool='rbd',
                                    rbd_image_name='img', size='1MB')
        assert res['nsid'] == 7
        req = client.return_value.stub.namespace_add.call_args[0][0]
        assert req.size == 1024 * 1024

    def test_size_and_rbd_image_size_are_mutually_exclusive(self):
        with patch.object(api, 'NVMeoFClient', make_client({})):
            with pytest.raises(NvmeofInvalidInputError) as excinfo:
                api.namespace_add(None, nqn='nqn.x', rbd_pool='rbd',
                                  rbd_image_name='img', size='1MB',
                                  rbd_image_size='1MB')
        assert excinfo.value.code == 'can_use_size_or_rbd_image_size_but_not_both'

    def test_encryption_formats_must_match_key_ids(self):
        with patch.object(api, 'NVMeoFClient', make_client({})):
            with pytest.raises(NvmeofInvalidInputError) as excinfo:
                api.namespace_add(None, nqn='nqn.x', rbd_pool='rbd',
                                  rbd_image_name='img',
                                  encryption_format=['aes_xts_128'],
                                  key_id=[])
        assert excinfo.value.code == 'key_ids_encryption_formats_mismatch'


class TestNamespaceResize:
    def test_resize_converts_size(self):
        client = make_client({
            'namespace_resize': pb2.req_status(status=0, error_message='')})
        with patch.object(api, 'NVMeoFClient', client):
            res = api.namespace_resize(None, nqn='nqn.x', nsid='1',
                                       rbd_image_size='2MB')
        assert res['status'] == 0
        req = client.return_value.stub.namespace_resize.call_args[0][0]
        assert req.new_size == 2  # MiB, converted from bytes


def _one_namespace_response():
    ns = pb2.namespace_cli(nsid=1, ns_subsystem_nqn='nqn.x',
                           load_balancing_group=2, hosts=['nqn.host'])
    return pb2.namespaces_info(status=0, error_message='',
                               subsystem_nqn='nqn.x', namespaces=[ns])


class TestNamespaceListings:
    def test_list_hosts_takes_module_instance(self):
        client = make_client({'list_namespaces': _one_namespace_response()})
        with patch.object(api, 'NVMeoFClient', client):
            res = api.namespace_list_hosts(None, nqn='nqn.x')
        assert res['namespaces'] == [
            {'nqn': 'nqn.x', 'nsid': 1, 'hosts': ['nqn.host']}]

    def test_list_locations_aggregates_by_group(self):
        client = make_client({'list_namespaces': _one_namespace_response()})
        with patch.object(api, 'NVMeoFClient', client):
            res = api.namespace_list_locations(None, nqn='nqn.x')
        assert res['locations'] == [
            {'subsystem': 'nqn.x', 'load_balancing_group': 2,
             'location': '<default>', 'namespace_count': 1}]


def make_module():
    mgr = nvmeof_mod.NVMeoF.__new__(nvmeof_mod.NVMeoF)
    mgr._registry_lock = threading.Lock()
    return mgr


class TestNamespaceUpdate:
    def _client(self, qos_status):
        return make_client({
            'namespace_set_qos_limits':
                pb2.req_status(status=qos_status, error_message=''),
            'list_namespaces': _one_namespace_response(),
        })

    def test_update_success_returns_plain_result(self):
        mgr = make_module()
        with patch.object(api, 'NVMeoFClient', self._client(qos_status=0)):
            res = mgr.api_call('namespace_update', nqn='nqn.x', nsid='1',
                               rw_ios_per_second=100)
        assert '__nvmeof_partial__' not in res
        assert res['namespaces'][0]['nsid'] == 1

    def test_update_partial_failure_is_enveloped(self):
        mgr = make_module()
        with patch.object(api, 'NVMeoFClient', self._client(qos_status=1)):
            res = mgr.api_call('namespace_update', nqn='nqn.x', nsid='1',
                               rw_ios_per_second=100)
        assert set(res) == {'__nvmeof_partial__'}
        assert res['__nvmeof_partial__']['namespaces'][0]['nsid'] == 1

    def test_partial_flag_does_not_leak_into_next_call(self):
        mgr = make_module()
        with patch.object(api, 'NVMeoFClient', self._client(qos_status=1)):
            mgr.api_call('namespace_update', nqn='nqn.x', nsid='1',
                         rw_ios_per_second=100)
        with patch.object(api, 'NVMeoFClient', self._client(qos_status=0)):
            res = mgr.api_call('namespace_update', nqn='nqn.x', nsid='1',
                               rw_ios_per_second=100)
        assert '__nvmeof_partial__' not in res


class TestNamespaceUpdateGateway:
    """Every call the update makes has to reach the group the caller
    named, not whichever gateway the registry hands out by default."""

    @pytest.mark.parametrize('update', [
        {'rbd_image_size': 1024 ** 3},
        {'load_balancing_group': 3},
        {'rw_ios_per_second': 100},
        {'trash_image': True},
        {'location': 'dc1'},
    ])
    def test_update_uses_requested_gateway(self, update):
        ok = pb2.req_status(status=0, error_message='')
        client = make_client({
            'namespace_resize': ok,
            'namespace_change_load_balancing_group': ok,
            'namespace_set_qos_limits': ok,
            'namespace_set_rbd_trash_image': ok,
            'namespace_change_location': ok,
            'list_namespaces': _one_namespace_response(),
        })
        with patch.object(api, 'NVMeoFClient', client):
            make_module().api_call('namespace_update', nqn='nqn.x', nsid='1',
                                   gw_group='group-b', **update)

        assert client.call_args_list
        for call in client.call_args_list:
            assert call.kwargs.get('gw_group') == 'group-b'


class TestApiCallEnvelope:
    def test_unexpected_exception_becomes_internal_envelope(self):
        mgr = make_module()
        with patch.object(api, 'namespace_get',
                          MagicMock(side_effect=ValueError('boom')),
                          create=True):
            res = mgr.api_call('namespace_get', nqn='nqn.x', nsid='1')
        assert res['__nvmeof_error__']['kind'] == 'internal'
        assert 'boom' in res['__nvmeof_error__']['msg']

    def test_invalid_input_becomes_input_envelope(self):
        mgr = make_module()
        with patch.object(api, 'NVMeoFClient', make_client({})):
            res = mgr.api_call('namespace_add', nqn='nqn.x', rbd_pool='rbd',
                               rbd_image_name='img', size='1MB',
                               rbd_image_size='1MB')
        assert res['__nvmeof_error__']['kind'] == 'input'
        assert res['__nvmeof_error__']['code'] == \
            'can_use_size_or_rbd_image_size_but_not_both'
