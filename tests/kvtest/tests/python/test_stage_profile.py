import importlib.util
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('stage_profile', ROOT / 'summarize_stage_profile.py')
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)


def sample(seq, count, total, final=False, phase='ds.api.set', instance='pod:123:1'):
    buckets = [0] * len(profile.BUCKET_NS)
    buckets[2] = count
    return dict(schema=1, instance=instance, seq=seq, final=final, phase=phase,
                count=count, sum_ns=total, max_ns=500, requested_bytes=count * 100,
                failed=0, unknown=0, buckets=buckets)


class StageProfileTest(unittest.TestCase):
    def test_cumulative_snapshots_are_not_added(self):
        rows, warnings = profile.summarize([sample(1, 2, 500), sample(2, 4, 1500, True)])
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]['count'], 4)
        self.assertEqual(rows[0]['mean_us'], 0.375)
        self.assertEqual(rows[0]['p99_upper_us'], 0.5)
        self.assertFalse(warnings)

    def test_exclude_warmup_and_preserve_lifetime_max_label(self):
        rows, _ = profile.summarize([sample(1, 2, 500), sample(2, 4, 1500, True)], start_seq=1)
        self.assertEqual(rows[0]['count'], 2)
        self.assertEqual(rows[0]['mean_us'], 0.5)
        self.assertEqual(rows[0]['max_lifetime_us'], 0.5)

    def test_rollup_uses_weighted_sum_and_keeps_roles(self):
        rows, _ = profile.summarize([sample(1, 2, 500, True),
                                    sample(1, 6, 2400, True, instance='pod2:123:1'),
                                    sample(1, 1, 100, True, phase='ds.ub.write', instance='worker:123:1')])
        self.assertEqual(rows[0]['count'], 8)
        self.assertAlmostEqual(rows[0]['mean_us'], 0.3625)
        self.assertEqual(rows[1]['source'], 'service')

    def test_nonmonotonic_counter_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'nonmonotonic'):
            profile.summarize([sample(1, 4, 1500), sample(2, 2, 500, True)], start_seq=1)

    def test_percentiles_do_not_use_a_racing_maximum(self):
        row = sample(1, 1, 100000)
        row['buckets'] = [0] * len(profile.BUCKET_NS)
        row['buckets'][10] = 1
        row['max_ns'] = 500
        rows, _ = profile.summarize([row])
        self.assertEqual(rows[0]['p99_upper_us'], 128)
        row['buckets'][10] = 0
        row['buckets'][-1] = 1
        rows, _ = profile.summarize([row])
        self.assertIsNone(rows[0]['p99_upper_us'])

    def test_duplicates_are_deduplicated_and_conflicts_rejected(self):
        import json
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'worker.log'
            row = sample(1, 2, 500, True)
            line = profile.MARKER + json.dumps(row) + '\n'
            path.write_text('normal log\n' + line + line)
            self.assertEqual(len(profile.read_snapshots([path])), 1)
            row['sum_ns'] += 1
            path.write_text(line + profile.MARKER + json.dumps(row) + '\n')
            with self.assertRaisesRegex(ValueError, 'conflicting duplicate'):
                profile.read_snapshots([path])

    def test_missing_profile_and_truncated_json_are_errors(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'empty.log'
            path.write_text('normal log\n')
            with self.assertRaisesRegex(ValueError, 'no stage snapshots'):
                profile.read_snapshots([path])
            path.write_text(profile.MARKER + '{\n')
            with self.assertRaisesRegex(ValueError, 'empty.log:1:'):
                profile.read_snapshots([path])

    def test_detail_records_reconcile_and_keep_conditional_steps_per_frame(self):
        import json
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'detail.log'
            row = dict(schema=1, instance='pod:123:2', seq=2, final=True, id=1,
                       group='ds.detail.get.query.ub_request.total', start_ns=100, duration_ns=1000,
                       overflow=False, outcome=-1, steps=[dict(phase='ds.detail.get.query.prepare', duration_ns=1000)])
            row2 = {**row, 'id': 2, 'steps': [dict(phase='ds.detail.get.query.call', duration_ns=1000)]}
            path.write_text(''.join('[KVTEST_DETAIL_SAMPLE] ' + json.dumps(r) + '\n' for r in (row, row, row2)))
            rows, samples, _ = profile.summarize_details([path], [sample(2, 1, 1, True)], start_seq=1)
            self.assertEqual(len(samples), 2)
            self.assertEqual(len(rows), 2)
            self.assertEqual(rows[0]['source'], 'client')
            self.assertEqual(rows[0]['mean_per_frame_us'], 0.5)
            self.assertEqual(sum(r['share_of_group_pct'] for r in rows), 100)
            row['duration_ns'] = 999
            path.write_text('[KVTEST_DETAIL_SAMPLE] ' + json.dumps(row) + '\n')
            with self.assertRaisesRegex(ValueError, 'do not reconcile'):
                profile.summarize_details([path], [])

    def test_detail_drop_and_overflow_are_reported(self):
        import json
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'detail.log'
            row = dict(schema=1, instance='pod:123:2', seq=2, final=True, id=1,
                       group='ds.detail.get.query.total', start_ns=100, duration_ns=2000,
                       overflow=True, outcome=-1, steps=[dict(phase='ds.detail.get.query.call', duration_ns=1000)])
            status = dict(schema=1, instance=row['instance'], seq=2, final=True, dropped_lifetime=1)
            path.write_text('[KVTEST_DETAIL_SAMPLE] ' + json.dumps(row) + '\n' +
                            '[KVTEST_DETAIL_STATUS] ' + json.dumps(status) + '\n')
            rows, _, warnings = profile.summarize_details([path], [])
            self.assertFalse(rows)
            self.assertTrue(any('overflow' in w for w in warnings))
            self.assertTrue(any('dropped' in w for w in warnings))

    def test_process_resources_are_not_added_across_recorder_instances(self):
        import json
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'resource.log'
            first = dict(schema=1, instance='pod:123:1', seq=1, final=False, monotonic_ns=1000000,
                         user_cpu_us=100, system_cpu_us=0, voluntary_switches=1, involuntary_switches=0,
                         minor_faults=1, major_faults=0)
            last = {**first, 'instance': 'pod:123:2', 'seq': 2, 'monotonic_ns': 2000000, 'user_cpu_us': 300}
            path.write_text(''.join('[KVTEST_PROFILE_RESOURCE] ' + json.dumps(r) + '\n'
                                    for r in (first, first, last, last)))
            rows = profile.read_resources([path], 1)
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]['cpu_us'], 200)
            self.assertEqual(rows[0]['average_cpu_cores'], 0.2)

    def test_full_event_counter_is_not_labeled_as_sampled_duration(self):
        row = sample(1, 2, 0, True, phase='ds.detail.get.query.total.calls')
        row['detail_sample_every'] = 64
        rows, _ = profile.summarize([row])
        self.assertEqual(rows[0]['kind'], 'counter')
        self.assertEqual(rows[0]['sample_every'], 1)


class MooncakePatchTest(unittest.TestCase):
    def setUp(self):
        import subprocess
        import hashlib
        import json
        spec = importlib.util.spec_from_file_location('moon_stage_profile', ROOT / 'tools/mooncake/stage_profile.py')
        self.helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.helper)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.source = Path(self.temp.name) / 'source'
        self.bundle = Path(self.temp.name) / 'bundle'
        self.source.mkdir()
        self.bundle.mkdir()
        subprocess.run(['git', 'init', '-q', str(self.source)], check=True)
        (self.source / 'source.cpp').write_text('old\n')
        subprocess.run(['git', '-C', str(self.source), 'add', 'source.cpp'], check=True)
        subprocess.run(['git', '-C', str(self.source), '-c', 'core.hooksPath=/dev/null',
                        '-c', 'commit.gpgsign=false', '-c', 'user.name=Test', '-c', 'user.email=test@example.invalid',
                        'commit', '-qm', 'fixture'], check=True)
        patch = ('diff --git a/source.cpp b/source.cpp\n--- a/source.cpp\n+++ b/source.cpp\n'
                 '@@ -1 +1 @@\n-old\n+new\n')
        (self.bundle / 'stage_profile.patch').write_text(patch)
        manifest = dict(revision=self.helper.git(self.source, 'rev-parse', 'HEAD'),
                        patch_sha256=hashlib.sha256(patch.encode()).hexdigest(),
                        files={'source.cpp': {'base': hashlib.sha256(b'old\n').hexdigest(),
                                              'profile': hashlib.sha256(b'new\n').hexdigest()}})
        (self.bundle / 'stage_profile_manifest.json').write_text(json.dumps(manifest))

    def test_apply_check_repeat_and_remove(self):
        self.assertEqual(self.helper.update(self.source, 'check', self.bundle), 'base')
        self.assertEqual(self.helper.update(self.source, 'apply', self.bundle), 'profile')
        self.assertEqual(self.helper.update(self.source, 'apply', self.bundle), 'profile')
        self.assertEqual((self.source / 'source.cpp').read_text(), 'new\n')
        self.assertEqual(self.helper.update(self.source, 'remove', self.bundle), 'base')
        self.assertEqual(self.helper.update(self.source, 'remove', self.bundle), 'base')
        self.assertEqual((self.source / 'source.cpp').read_text(), 'old\n')

    def test_user_source_edits_are_preserved_and_rejected(self):
        (self.source / 'source.cpp').write_text('user edit\n')
        with self.assertRaisesRegex(ValueError, 'source differs'):
            self.helper.update(self.source, 'apply', self.bundle)
        self.assertEqual((self.source / 'source.cpp').read_text(), 'user edit\n')

    def test_canonical_header_matches_published_patch_manifest(self):
        import hashlib
        import json
        manifest = json.loads((ROOT / 'tools/mooncake/stage_profile_manifest.json').read_text())
        header = ROOT.parents[1] / 'src/datasystem/common/metrics/benchmark_profile.h'
        self.assertEqual(hashlib.sha256(header.read_bytes()).hexdigest(),
                         manifest['files']['mooncake-common/include/kvtest_benchmark_profile.h']['profile'])


if __name__ == '__main__':
    unittest.main()
