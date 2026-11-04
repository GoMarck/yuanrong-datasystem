import argparse
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import deploy_compare_ub_rdma as compare
import deploy_client as client
from kvtest_report import render_html_report


class Rc44ComparisonTest(unittest.TestCase):
    def generated_client_config(self, scenario, backend):
        plan = compare.plan_from_scenario(scenario)
        command = compare.client_gen_command(plan, backend, 'ns', 'client-',
                                            Path('/case'), '10.0.0.1:31511', scenario)
        parser = argparse.ArgumentParser()
        client._add_gen_config_args(parser)
        args = parser.parse_args(list(map(str, command[3:])))
        return client._build_config('pipeline', args)

    def test_comparison_defaults_use_transport_reads_and_random_numa(self):
        scenario = json.loads((compare.ROOT / 'comparison/scenario.2-nodes-1clients.json').read_text())
        scenario.pop('client_numa_node', None)
        scenario['components']['datasystem']['client']['config'].pop('enable_local_cache', None)
        for backend in ('datasystem', 'mooncake'):
            with self.subTest(backend=backend):
                config = self.generated_client_config(scenario, backend)
                self.assertTrue(config['random_numa_node'])
                self.assertNotIn('numa_node', config)
                if backend == 'datasystem':
                    self.assertFalse(config['connect_options']['enable_local_cache'])

    def test_comparison_preserves_explicit_cache_and_fixed_numa(self):
        scenario = json.loads((compare.ROOT / 'comparison/scenario.2-nodes-1clients.json').read_text())
        scenario['client_numa_node'] = 0
        scenario['components']['datasystem']['client']['config']['enable_local_cache'] = True
        for backend in ('datasystem', 'mooncake'):
            with self.subTest(backend=backend):
                config = self.generated_client_config(scenario, backend)
                self.assertFalse(config['random_numa_node'])
                self.assertEqual(config['numa_node'], 0)
                if backend == 'datasystem':
                    self.assertTrue(config['connect_options']['enable_local_cache'])

    def test_shipped_scenarios_use_random_numa_and_disable_local_cache(self):
        for path in sorted((compare.ROOT / 'comparison').glob('scenario.*.json')):
            with self.subTest(path=path.name):
                scenario = json.loads(path.read_text())
                for backend in compare.configured_systems(scenario):
                    config = self.generated_client_config(scenario, backend)
                    self.assertTrue(config['random_numa_node'])
                    if backend == 'datasystem':
                        self.assertFalse(config['connect_options']['enable_local_cache'])

    def test_invalid_numa_nodes_are_rejected(self):
        scenario = json.loads((compare.ROOT / 'comparison/scenario.2-nodes-1clients.json').read_text())
        for value in (-1, True, '0', 0.5):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, 'client_numa_node'):
                scenario['client_numa_node'] = value
                compare.plan_from_scenario(scenario)

    def test_ds_uses_sdk_access_logs_and_reports_coverage(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            client = root / 'collected/ds-client-0-node_0'
            metrics = client / 'metrics_test'
            metrics.mkdir(parents=True)
            summary = metrics / 'run_summary.txt'
            summary.touch()
            (client / 'ds_client_access_1.log').write_text(
                'time | 0 | DS_KV_CLIENT_SET | 301 | 8 | {key:kv_test_0_1} | OK\n'
                'time | 0 | DS_KV_CLIENT_GET | 404 | 8 | {key:kv_test_0_1} | OK\n'
                'time | 0 | DS_KV_CLIENT_SET | 9999 | 8 | {key:other} | OK\n')
            counts = {op: {'total': 2, 'fail': 0} for op in ('setStringView', 'getBuffer')}
            stats = compare.aggregate_api_latency_samples({summary: counts})
            self.assertEqual(stats['setStringView']['avg_us'], 301)
            self.assertEqual(stats['getBuffer']['total'], 1)
            evidence = json.loads((root / 'latency-source.json').read_text())
            self.assertEqual(evidence['source'], 'datasystem_sdk_access_log')
            self.assertEqual(evidence['clients'][0]['coverage']['getBuffer'], 0.5)

    def test_mc_rejects_unflushed_export(self):
        with tempfile.TemporaryDirectory() as directory:
            summary = Path(directory) / 'run_summary.txt'
            (summary.parent / 'api_latency_samples.csv').write_text(
                'op,latency_us,status_code\nput,100,0\nget_buffer,200,0\n')
            counts = {op: {'total': 1, 'fail': 0} for op in ('put', 'get_buffer')}
            with self.assertRaisesRegex(RuntimeError, 'status'):
                compare.aggregate_api_latency_samples({summary: counts})

    def test_mc_export_status_and_counts_are_verified(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            summary = root / 'run_summary.txt'
            (root / 'api_latency_samples.csv').write_text(
                'op,latency_us,status_code\nput,100,0\nget_buffer,200,0\n')
            status = root / 'api_latency_samples.status.txt'
            status.write_text('{"io_error":false,"dropped_samples":0,"written_samples":2}')
            counts = {op: {'total': 1, 'fail': 0} for op in ('put', 'get_buffer')}
            stats = compare.aggregate_api_latency_samples({summary: counts})
            self.assertEqual(stats['get_buffer']['avg_us'], 200)
            status.write_text('{"io_error":false,"dropped_samples":1,"written_samples":2}')
            with self.assertRaisesRegex(RuntimeError, 'incomplete'):
                compare.aggregate_api_latency_samples({summary: counts})

    def test_external_window_needs_no_new_cpp_stats(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'execution.json').write_text('{"duration_seconds":1}')
            with patch.object(compare.time, 'monotonic', side_effect=[10, 12, 12]):
                compare.wait_pipeline_writers(root, None)
            self.assertIn('rc44', (root / 'drain-status.json').read_text())

    def test_html_labels_sources_and_coverage(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            case = root / 'datasystem'
            case.mkdir()
            stats = {op: {'total': 1, 'fail': 0, 'avg_us': 301, 'p50_us': 301, 'p95_us': 301,
                          'p99_us': 301} for op in ('setStringView', 'getBuffer')}
            compare.write_json(case / 'api_latency_summary.json', stats)
            compare.write_json(case / 'latency-source.json', {
                'source': 'datasystem_sdk_access_log', 'clients': [{
                    'summary': 'collected/ds-client-0-node_0/metrics/run_summary.txt',
                    'samples': {'getBuffer': 1}, 'operations': {'getBuffer': 2}, 'coverage': {'getBuffer': 0.5}}]})
            render_html_report(root, ('datasystem',), {})
            report = (root / 'report.html').read_text(encoding='utf-8')
            self.assertIn('datasystem_sdk_access_log', report)
            self.assertIn('50.00%', report)
            self.assertIn('两者边界不同', report)

    def test_backend_binary_preserves_shared_path_compatibility(self):
        scenario = {'artifacts': {'kvtest_binary': '/shared/kvtest',
                                 'kvtest_binaries': {'datasystem': '/native/kvtest'}}}
        self.assertEqual(compare.backend_binary(scenario, 'datasystem').name, 'kvtest')
        self.assertEqual(compare.backend_binary(scenario, 'mooncake'), Path('/shared/kvtest').resolve())


if __name__ == '__main__':
    unittest.main()
