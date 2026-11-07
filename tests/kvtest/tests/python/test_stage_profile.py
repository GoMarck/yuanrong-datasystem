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
                         manifest['files']['mooncake-store/include/benchmark_profile.h']['profile'])


if __name__ == '__main__':
    unittest.main()
