import copy
import argparse
import json
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch, MagicMock

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
import deploy_compare_ub_rdma as compare
import deploy_common as common
import deploy_coordinator as coordinator
import deploy_worker as worker
import deploy_client as client
from kvtest_report import render_html_report


def scenario():
    config = json.loads((compare.ROOT / 'comparison/scenario.2-nodes-1clients.json').read_text())
    config['systems'] = ['datasystem']
    config['components']['datasystem']['client']['config']['service_discovery'] = 'jf'
    config['datasystem_deployment'] = {'mode': 'standalone_jf', 'jf': {'node': 'master'}}
    return config


class JfComparisonTest(unittest.TestCase):
    def test_client_uses_jf_without_static_coordinator(self):
        config = scenario()
        plan = compare.plan_from_scenario(config)
        command = list(map(str, compare.client_gen_command(plan, 'datasystem', 'ns', 'ds-client-',
                            Path('/case'), '10.0.0.1:9999', config)))
        self.assertIn('--jf', command)
        self.assertNotIn('--coordinator-address', command)
        self.assertEqual(command[command.index('--service') + 1], 'kvcache_coordinator')
        legacy = copy.deepcopy(config)
        legacy.pop('datasystem_deployment')
        command = list(map(str, compare.client_gen_command(plan, 'datasystem', 'ns', 'ds-client-',
                            Path('/case'), '10.0.0.2:31511', legacy)))
        self.assertIn('--coordinator-address', command)
        self.assertNotIn('--jf', command)

    def test_native_client_config_contains_only_jf_discovery(self):
        parser = argparse.ArgumentParser()
        client._add_gen_config_args(parser)
        args = parser.parse_args(['--jf', '10.0.0.1:9999', '--service', 'kvcache_coordinator'])
        config = client._build_config('pipeline', args)
        self.assertEqual(config['jf_address'], '10.0.0.1:9999')
        self.assertNotIn('coordinator_address', config)
        self.assertNotIn('etcd_address', config)

    def test_bad_mode_is_rejected_before_deployment(self):
        config = scenario()
        config['datasystem_deployment']['mode'] = 'standalon'
        with self.assertRaisesRegex(ValueError, 'datasystem_deployment'):
            compare.plan_from_scenario(config)

    def test_jf_port_and_unknown_fields_are_rejected(self):
        for item in ({'port': 0}, {'port': True}, {'ttl_seconds': 0}, {'node': 'bad node'}, {'unknown': 1}):
            config = scenario()
            config['datasystem_deployment']['jf'].update(item)
            with self.subTest(item=item), self.assertRaises(ValueError):
                compare.plan_from_scenario(config)

    def test_service_args_use_matching_native_package(self):
        config = scenario()
        config['artifacts'] = {'kvtest_binaries': {'datasystem': '/native/kvtest'}}
        plan = compare.plan_from_scenario(config)
        args = list(map(str, compare.datasystem_service_args(config, plan, 'coordinator', '10.0.0.1:9999', None)))
        self.assertIn('--standalone', args)
        self.assertNotIn('--whl', args)
        self.assertEqual(args[args.index('--binary') + 1], str(Path('/native/coordinator_test').resolve()))
        self.assertEqual(args[args.index('--expected-member-count') + 1], '1')

    def test_standalone_cli_does_not_require_whl(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / 'scenario.json'
            path.write_text(json.dumps(scenario()))
            with patch.object(compare, 'execute_trial', return_value=0) as execute:
                self.assertEqual(compare.main(['--scenario', str(path), '--mc-image', 'test-image',
                                               '--output', str(root / 'result')]), 0)
            self.assertIsNone(execute.call_args.args[1].ds_whl)

    def test_standalone_template_preserves_metadata_and_order(self):
        for module, address_key in ((worker, 'worker_address'), (coordinator, 'coordinator_address')):
            with self.subTest(module=module.__name__), tempfile.TemporaryDirectory() as directory:
                text = '{\n  "' + address_key + '": {"description": "keep", "value": "old"},\n  "log_dir": {"value": "/logs"}\n}\n'
                path = Path(directory) / 'config.json'
                path.write_bytes(text.encode())
                args = SimpleNamespace(config=str(path), set=[], namespace='ns', jf='jf:9999',
                    service='service', remote_dir='/data/native', remote_config='/data/config.json',
                    port=31501, enable_procmon=False, timeout=30, ttl=30, expected_member_count=1,
                    preserve_config_template=True)
                pods = [{'name': 'pod-0', 'ip': '10.0.0.2'}]
                with patch.object(module, 'start_service_standalone', return_value=True) as start, \
                        patch.object(module, 'check_process', return_value=(None, 'dead', None), create=True):
                    self.assertEqual(module.cmd_start_standalone(args, pods), 0)
                kwargs = start.call_args.kwargs
                self.assertEqual(kwargs['config_template_text'], text)
                self.assertEqual(kwargs['config'][address_key]['description'], 'keep')

    def test_common_uploads_strict_template_text(self):
        source = '{\n "worker_address": {"description":"keep", "value":"old"}\n}\n'
        uploaded = []
        def capture(pod, ns, local, remote, **kwargs):
            uploaded.append(Path(local).read_text())
        with patch.object(common, 'kubectl_cp_to', side_effect=capture), \
                patch.object(common, 'upload_launcher', return_value='/launcher.py'), \
                patch.object(common, '_launch_via_launcher', return_value=('123', 0.1)), \
                patch.object(common, 'kubectl_exec_raw', return_value='123'):
            result = common.start_service_standalone({'name': 'pod-0', 'ip': '10.0.0.1'}, 'ns',
                'worker_test', '/data/native', '/data/config.json', 'jf:9999', 'service',
                config={'worker_address': {'description': 'keep', 'value': 'new'}},
                enable_procmon=False, config_template_text=source)
        self.assertTrue(result)
        self.assertEqual(uploaded, [source.replace('"old"', '"new"')])

    def test_jf_placement_registration_and_log_collection(self):
        config = scenario()
        plan = compare.plan_from_scenario(config)
        run = MagicMock()
        run.addresses = {'master': '10.1.0.1'}
        run.pods.return_value = {'ds-jf-0-master': {
            'metadata': {'name': 'ds-jf-0-master'}, 'status': {'podIP': '10.2.0.1'}}}
        discovery = compare.start_jf_mock(run, config, plan, 'ns', 'image', Path('/control.yaml'))
        self.assertEqual(run.deploy_pods.call_args.args[4:], ('10.1.0.1:1', '1', '1Gi'))
        self.assertEqual(discovery['jf_address'], '10.2.0.1:9999')
        run.command.return_value = '{"instances":["10.2.0.2:31511"]}'
        registration = compare.wait_jf_registration(run, 'ns', discovery, ['10.2.0.2:31511'])
        self.assertEqual(registration['instances'], ['10.2.0.2:31511'])
        with tempfile.TemporaryDirectory() as directory:
            case = Path(directory)
            compare.write_json(case / 'service-discovery.json', discovery)
            run.command.reset_mock()
            compare.collect_datasystem_services(run, 'ns', case)
        commands = [list(map(str, c.args[0])) for c in run.command.call_args_list]
        self.assertEqual(len(commands), 3)
        self.assertIn('/data/standalone/worker', commands[1])
        self.assertIn('/data/jf', commands[2])

    def test_html_records_jf_mode_and_pod_role(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            case = root / 'datasystem'
            case.mkdir()
            stats = {op: {'total': 1, 'fail': 0, 'avg_us': 1, 'p50_us': 1, 'p95_us': 1, 'p99_us': 1}
                     for op in ('setStringView', 'getBuffer')}
            compare.write_json(case / 'api_latency_summary.json', stats)
            compare.write_json(case / 'service-discovery.json', {'mode': 'standalone_jf',
                'jf': {'node': 'master', 'service': 'kvcache_coordinator'}, 'jf_address': '10.0.0.1:9999',
                'registration': {'instances': ['10.0.0.2:31511']}, 'native_package': '/native'})
            compare.write_json(case / 'pods.json', {'items': [{'metadata': {'name': 'ds-jf-0-master'},
                                                             'spec': {'nodeName': 'master'}}]})
            render_html_report(root, ('datasystem',), {})
            html = (root / 'report.html').read_text(encoding='utf-8')
            for value in ('standalone_jf', '10.0.0.1:9999', '10.0.0.2:31511', '<th>JF</th>'):
                self.assertIn(value, html)


if __name__ == '__main__':
    unittest.main()
