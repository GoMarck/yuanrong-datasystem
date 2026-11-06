import copy
from pathlib import Path
import sys
import unittest
from unittest.mock import MagicMock

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from deploy_compare_ub_rdma import Run


class TestBatchPodWait(unittest.TestCase):
    def make_run(self, ready=True):
        initial = {'items': [{'metadata': {'name': name}, 'status': {}}
                             for name in ('client-a', 'client-b')]}
        updated = copy.deepcopy(initial)
        for item in updated['items']:
            item['status'] = {'podIP': '192.0.2.1', 'hostIP': '192.0.2.2',
                              'conditions': [{'type': 'Ready', 'status': 'True' if ready else 'False'}]}
        run = Run.__new__(Run)
        run.kube_json = MagicMock(side_effect=[initial, updated])
        run.command = MagicMock()
        return run

    def test_one_wait_then_refresh_ips(self):
        run = self.make_run()
        pods = run.pods('bench', 'client-', 2)
        run.command.assert_called_once_with(
            ['kubectl', '-n', 'bench', 'wait', '--for=condition=Ready',
             'pod/client-a', 'pod/client-b', '--timeout=300s'], timeout=330)
        self.assertEqual(run.kube_json.call_count, 2)
        self.assertEqual(pods['client-a']['status']['podIP'], '192.0.2.1')

    def test_unready_or_changed_pod_set_is_rejected(self):
        run = self.make_run(ready=False)
        with self.assertRaises(RuntimeError):
            run.pods('bench', 'client-', 2)
        run = self.make_run()
        run.kube_json.side_effect = [{'items': [{'metadata': {'name': 'client-a'}}]}, {'items': []}]
        with self.assertRaises(RuntimeError):
            run.pods('bench', 'client-', 1)
