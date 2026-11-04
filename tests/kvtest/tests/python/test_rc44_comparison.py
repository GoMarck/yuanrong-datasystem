import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import deploy_compare_ub_rdma as compare
from kvtest_report import render_html_report


class Rc44ComparisonTest(unittest.TestCase):
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
