"""Contract checks use only disposable regular files, never a real device."""
import json
import signal
import time
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "knurl"


class CliContract(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="knurl-test-")
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.work = self.base / "work"
        self.out = self.base / "out"

    def run_cli(self, *extra, ok=True):
        args = [str(BIN), "--work-dir", str(self.work), "--output-dir", str(self.out),
                "--mode", "gen2", "--buffered", "--file-sizes", "65536",
                "--block-sizes", "4096", "--concurrency", "1", "--patterns", "seq-read",
                "--repetitions", "1", "--duration", "0.05", "--warmup", "0"]
        result = subprocess.run(args + list(extra), text=True, capture_output=True, timeout=30)
        if ok:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def test_help(self):
        result = subprocess.run([str(BIN), "--help"], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0)
        self.assertIn("--work-dir", result.stdout + result.stderr)

    def test_setup_labels_are_generic_metadata(self):
        for label in ("mac", "paco", "baseline-candidate"):
            with self.subTest(label=label):
                self.work = self.base / ("work-" + label)
                self.out = self.base / ("out-" + label)
                self.run_cli("--mode", label)
                manifest = json.loads((self.out / "manifest.json").read_text())
                self.assertEqual(manifest["mode"], label)
                records = list((self.out / "trials").glob("*.json"))
                self.assertEqual(len(records), 1)
                self.assertEqual(json.loads(records[0].read_text())["config"]["mode"], label)

    def test_unsafe_setup_labels_fail_before_preparation(self):
        for label in ("", "bad/label", 'bad"label', "bad label", "-leading", "a" * 65):
            with self.subTest(label=label):
                self.run_cli("--mode", label, ok=False)
                self.assertFalse(self.work.exists())

    def test_default_setup_label_is_baseline(self):
        result = subprocess.run([str(BIN), "--work-dir", str(self.work),
                                 "--output-dir", str(self.out), "--buffered",
                                 "--file-sizes", "65536", "--block-sizes", "4096",
                                 "--concurrency", "1", "--patterns", "seq-read",
                                 "--repetitions", "1", "--duration", "0.05", "--warmup", "0"],
                                text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads((self.out / "manifest.json").read_text())["mode"], "baseline")

    def test_unowned_directory_is_preserved(self):
        self.work.mkdir()
        valuable = self.work / "valuable.txt"
        valuable.write_bytes(b"must survive")
        self.run_cli(ok=False)
        self.assertEqual(valuable.read_bytes(), b"must survive")
        self.assertEqual(list(self.work.iterdir()), [valuable])

    def test_symlink_work_directory_is_rejected(self):
        target = self.base / "target"
        target.mkdir()
        sentinel = target / "sentinel"
        sentinel.write_bytes(b"untouched")
        self.work.symlink_to(target, target_is_directory=True)
        self.run_cli(ok=False)
        self.assertEqual(sentinel.read_bytes(), b"untouched")
        self.assertEqual(list(target.iterdir()), [sentinel])

    def test_symlink_data_file_is_rejected(self):
        self.run_cli("--prepare-only")
        files = list(self.work.glob("knurl-*.dat"))
        self.assertEqual(len(files), 1)
        sentinel = self.base / "sentinel"
        sentinel.write_bytes(b"untouched")
        files[0].unlink()
        files[0].symlink_to(sentinel)
        self.run_cli(ok=False)
        self.assertEqual(sentinel.read_bytes(), b"untouched")

    def test_device_path_is_rejected(self):
        self.work = Path("/dev/null")
        self.run_cli(ok=False)

    def test_invalid_block_size_fails_before_preparation(self):
        self.run_cli("--block-sizes", "131072", ok=False)
        self.assertFalse(self.work.exists())

    def test_trial_counters_and_histograms(self):
        self.run_cli("--patterns", "seq-read,seq-write,rand-mix70read30write")
        records = [json.loads(p.read_text()) for p in sorted((self.out / "trials").glob("*.json"))]
        self.assertEqual(len(records), 3)
        for record in records:
            self.assertTrue(record["complete"])
            self.assertTrue(record["verified"])
            self.assertGreater(record["measured_seconds"], 0)
            self.assertEqual(record["errors"], 0)
            total_ops = 0
            for kind in ("read", "write"):
                counters = record[kind]
                total_ops += counters["ops"]
                self.assertEqual(counters["bytes"], counters["ops"] * 4096)
                lat = counters["latency_ns"]
                self.assertEqual(sum(lat["histogram"]), counters["ops"])
                self.assertEqual(len(lat["histogram"]), 1024)
                self.assertGreaterEqual(lat["sum"], 0)
                if counters["ops"]:
                    self.assertGreater(lat["mean"], 0)
                    self.assertLessEqual(lat["p50"], lat["p95"])
                    self.assertLessEqual(lat["p95"], lat["p99"])
                    self.assertLessEqual(lat["p99"], lat["p999"])
            self.assertGreater(total_ops, 0)
            pattern = record["config"]["pattern"]
            if pattern == "seq-read":
                self.assertEqual(record["write"]["ops"], 0)
            elif pattern == "seq-write":
                self.assertEqual(record["read"]["ops"], 0)
            else:
                self.assertGreater(record["read"]["ops"], 0)
                self.assertGreater(record["write"]["ops"], 0)

    def test_resume_does_not_duplicate_trials_and_rejects_drift(self):
        self.run_cli()
        paths = sorted((self.out / "trials").glob("*.json"))
        before = {p.name: p.read_bytes() for p in paths}
        self.assertEqual(len(before), 1)
        self.run_cli()
        self.assertEqual({p.name: p.read_bytes() for p in (self.out / "trials").glob("*.json")}, before)
        self.run_cli("--seed", "987654321", ok=False)
        self.assertEqual({p.name: p.read_bytes() for p in (self.out / "trials").glob("*.json")}, before)

    def test_interrupt_never_marks_partial_trial_complete(self):
        self.run_cli("--prepare-only")
        args = [str(BIN), "--work-dir", str(self.work), "--output-dir", str(self.out),
                "--mode", "gen2", "--buffered", "--file-sizes", "65536",
                "--block-sizes", "4096", "--concurrency", "1", "--patterns", "seq-read",
                "--repetitions", "1", "--duration", "30", "--warmup", "0"]
        with tempfile.TemporaryFile(mode="w+") as log:
            proc = subprocess.Popen(args, stdout=log, stderr=log, text=True)
            try:
                deadline = time.monotonic() + 5
                started = False
                while time.monotonic() < deadline and proc.poll() is None:
                    log.seek(0)
                    if "Trial 1/1:" in log.read():
                        started = True
                        break
                    time.sleep(0.01)
                self.assertTrue(started, "trial did not start promptly")
                proc.send_signal(signal.SIGINT)
                self.assertEqual(proc.wait(timeout=5), 130)
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait(timeout=5)
            progress = json.loads((self.out / "progress.json").read_text())
            self.assertEqual(progress["status"], "interrupted")
            self.assertEqual(progress["completed"], 0)
            for path in (self.out / "trials").glob("*.json"):
                self.assertFalse(json.loads(path.read_text())["complete"])

    def test_integrity_detects_corruption(self):
        self.run_cli("--prepare-only")
        files = list(self.work.glob("knurl-*.dat"))
        self.assertEqual(len(files), 1)
        self.run_cli("--verify")
        with files[0].open("r+b") as stream:
            stream.seek(4096)
            value = stream.read(1)
            stream.seek(4096)
            stream.write(bytes([value[0] ^ 0xFF]))
        self.run_cli("--verify", ok=False)


if __name__ == "__main__":
    unittest.main()
