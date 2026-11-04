"""Prevent comparison support from changing the approved rc44 DS hot paths."""
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
BASE = '2ff37e2f7c5fb45f3feea7c3fb969b8f84b920d6'


class Rc44SourceGuard(unittest.TestCase):
    def baseline(self, path):
        return subprocess.check_output(['git', 'show', f'{BASE}:{path}'], cwd=ROOT).decode().replace('\r\n', '\n')

    def test_original_execution_and_measurement_files_are_identical(self):
        for name in ('pipeline/pipeline.cpp', 'pipeline/pipeline.h', 'pipeline/cache_reader.cpp',
                     'common/thread_pool.h', 'common/rate_gate.h', 'pipeline/data_pattern.h',
                     'pipeline/stop.cpp', 'metrics/metrics.cpp', 'rpc/peer_client.cpp'):
            path = 'tests/kvtest/src/' + name
            with self.subTest(path=path):
                self.assertEqual((ROOT / path).read_text(encoding='utf-8'), self.baseline(path))

    def test_worker_and_notify_only_bind_new_ops_at_construction(self):
        for name, boundary in (('pipeline/kv_worker.cpp', 'KVWorker::~KVWorker()'),
                               ('rpc/notify_dispatcher.cpp', 'void NotifyDispatcher::DispatchNotify')):
            path = 'tests/kvtest/src/' + name
            current = (ROOT / path).read_text(encoding='utf-8').split(boundary, 1)[1]
            baseline = self.baseline(path).split(boundary, 1)[1]
            self.assertEqual(current, baseline)

    def test_product_sdk_worker_and_coordinator_are_not_modified(self):
        paths = subprocess.check_output(['git', 'diff', '--name-only', BASE, '--',
                                         'src', 'include', 'cmake', 'third_party'], cwd=ROOT).decode()
        self.assertEqual(paths.strip(), '')


if __name__ == '__main__':
    unittest.main()
