"""Bounded local-only checkout contracts and known statistical reference values."""
import json
import math
import os
from pathlib import Path
import re
import shlex
import socket
import struct
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "knurl-check"


class CheckoutContracts(unittest.TestCase):
    def run_check(self, *args, ok=True, timeout=30):
        result = subprocess.run([str(BIN), *args], capture_output=True, text=True, timeout=timeout)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            return json.loads(result.stdout)
        self.assertNotEqual(result.returncode, 0, result.stderr + result.stdout)
        return result

    def test_inventory_is_read_only_and_escapes_paths(self):
        with tempfile.TemporaryDirectory(prefix='knurl-"inventory-') as path:
            before = set(Path(path).iterdir())
            data = self.run_check("inventory", "--target", path)
            self.assertTrue(data["complete"])
            inv = data["modules"][0]
            self.assertEqual(inv["module"], "inventory")
            self.assertFalse(inv["private_identifiers_collected"])
            self.assertGreater(inv["logical_cpus"], 0)
            self.assertGreater(inv["ram_total_bytes"], 0)
            self.assertTrue(inv["filesystem"]["available"])
            self.assertEqual(inv["filesystem"]["target"], path)
            self.assertEqual(set(Path(path).iterdir()), before)
            self.assertEqual(inv["storage_health"]["status"], "unavailable")

    def test_invalid_inventory_target_is_reported(self):
        with tempfile.TemporaryDirectory() as path:
            result = self.run_check("inventory", "--target", str(Path(path) / "missing"), ok=False)
            data = json.loads(result.stdout)
            self.assertFalse(data["complete"])
            self.assertFalse(data["modules"][0]["filesystem"]["available"])

    def test_disk_requires_explicit_paths(self):
        data = self.run_check("disk")
        self.assertEqual(data["modules"][0]["status"], "not_run")
        self.run_check("disk", "--work-dir", "/unused", ok=False)

    def test_disk_small_direct_integrity_and_log_preservation(self):
        with tempfile.TemporaryDirectory() as path:
            work = Path(path) / "work"
            out = Path(path) / "out"
            args = ("disk", "--work-dir", str(work), "--output-dir", str(out),
                    "--disk-bytes", "65536", "--duration", ".02", "--repetitions", "1")
            data = self.run_check(*args)
            disk = data["modules"][0]
            self.assertEqual(disk["status"], "passed")
            self.assertTrue(disk["full_byte_verification_before_and_after"])
            self.assertEqual(disk["expected_and_completed_trials"], 10)
            files = {p: p.read_bytes() for p in out.rglob("*") if p.is_file()}
            self.assertTrue(files)
            self.run_check(*args, ok=False)
            self.assertEqual({p: p.read_bytes() for p in out.rglob("*") if p.is_file()}, files)

    def test_bounded_options_reject_before_work(self):
        for args in (("cpu", "--duration", "nan"), ("cpu", "--duration", "6"),
                     ("memory", "--memory-bytes", "1073741825"),
                     ("cpu", "--threads", "0"), ("cpu", "--repetitions", "31"),
                     ("disk", "--disk-bytes", "4096")):
            with self.subTest(args=args):
                self.run_check(*args, ok=False)

    def test_cpu_known_answer_and_operation_accounting(self):
        data = self.run_check("cpu", "--threads", "2", "--duration", ".005", "--repetitions", "2")
        cpu = data["modules"][0]
        self.assertTrue(data["complete"])
        self.assertTrue(cpu["complete"])
        self.assertEqual(cpu["integer_self_test"], "43fc3d1de5ef95df")
        self.assertEqual({c["name"] for c in cpu["cases"]}, {"integer_hash_chain", "exact_fraction_dot_product"})
        for case in cpu["cases"]:
            self.assertEqual(case["summary"]["count"], 2)
            for trial in case["repetitions"]:
                self.assertTrue(trial["verified"])
                self.assertEqual(trial["error_code"], 0)
                self.assertGreaterEqual(trial["seconds"], .005 - 1e-6)
                self.assertGreater(trial["operations"], 0)
                self.assertAlmostEqual(trial["rate"] / (trial["operations"] / trial["seconds"]), 1, places=10)

    def test_memory_budget_integrity_and_traffic_accounting(self):
        data = self.run_check("memory", "--threads", "2", "--memory-bytes", "65536",
                              "--duration", ".005", "--repetitions", "2")
        memory = data["modules"][0]
        self.assertTrue(data["complete"])
        self.assertTrue(memory["complete"])
        self.assertEqual(memory["allocation_budget_bytes"], 65536)
        kinds = {"scalar_sequential_read", "scalar_sequential_write", "memcpy", "random_pointer_chase"}
        self.assertEqual({c["name"] for c in memory["cases"]}, kinds)
        for case in memory["cases"]:
            size = case["working_set_bytes_per_thread"]
            self.assertLessEqual(size * 2 * 2, 65536)
            self.assertEqual(case["summary"]["count"], 2)
            for trial in case["repetitions"]:
                self.assertTrue(trial["verified"])
                self.assertEqual(trial["error_code"], 0)
                self.assertGreaterEqual(trial["seconds"], .005 - 1e-6)
                self.assertGreater(trial["iterations"], 0)
                self.assertGreater(trial["value"], 0)
                if case["name"] == "random_pointer_chase":
                    self.assertEqual(trial["traffic_bytes"], 0)
                    self.assertEqual(trial["accesses"], trial["iterations"])
                else:
                    factor = 2 if case["name"] == "memcpy" else 1
                    self.assertEqual(trial["traffic_bytes"], trial["iterations"] * size * factor)
                    self.assertAlmostEqual(trial["value"] / (trial["traffic_bytes"] / trial["seconds"]), 1, places=10)

    def test_all_checkout_omits_unrequested_disk(self):
        data = self.run_check("check", "--duration", ".005", "--repetitions", "1", "--memory-bytes", "16384")
        self.assertTrue(data["complete"])
        modules = {m["module"]: m for m in data["modules"]}
        self.assertEqual(set(modules), {"inventory", "cpu", "memory", "network", "disk"})
        self.assertEqual(modules["disk"]["status"], "not_run")

    def test_loopback_is_verified_and_not_physical_link_claim(self):
        data = self.run_check("network", "--repetitions", "2")
        network = data["modules"][0]
        self.assertTrue(data["complete"])
        self.assertTrue(network["verified"])
        self.assertFalse(network["physical_link_test"])
        self.assertEqual(network["scope"], "loopback_selftest")
        self.assertEqual(len(network["trials"]), 6)
        for trial in network["trials"]:
            self.assertGreater(trial["sent_bytes"], 0)
            self.assertGreater(trial["throughput_mib_s"], 0)
            self.assertTrue(all(x > 0 for x in trial["rtt_us"]))
        for summary in network["summary_by_payload"]:
            self.assertEqual(summary["throughput"]["count"], 2)
            self.assertEqual(summary["throughput"]["ci_method"], "student_t_0.975_df_n_minus_1")

    def test_external_client_requires_explicit_host(self):
        self.run_check("client", ok=False, timeout=5)

    def test_server_rejects_oversized_wire_request(self):
        with tempfile.TemporaryFile(mode="w+") as log:
            proc = subprocess.Popen([str(BIN), "server", "--bind", "127.0.0.1", "--port", "0", "--timeout", "2"],
                                    stdout=subprocess.PIPE, stderr=log, text=True)
            try:
                deadline = time.monotonic() + 5
                port = None
                while time.monotonic() < deadline and proc.poll() is None:
                    log.seek(0)
                    match = re.search(r"port (\d+)", log.read())
                    if match:
                        port = int(match.group(1))
                        break
                    time.sleep(.01)
                self.assertIsNotNone(port)
                with socket.create_connection(("127.0.0.1", port), timeout=2) as peer:
                    peer.sendall(struct.pack("!8I", 0x4B4E4E31, 1, 1, 0xFFFFFFFF, 1, 0, 0, 0))
                stdout, _ = proc.communicate(timeout=5)
                self.assertNotEqual(proc.returncode, 0)
                self.assertFalse(json.loads(stdout)["verified"])
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.communicate(timeout=5)

    def test_summary_matches_known_reference(self):
        source = ('#include "knurl_check.h"\n'
                  'int main(void){double v[]={1,2,3},m[]={1,1,2,2,3},z[]={-1,0,1};'
                  'putchar(91);knurl_check_summary(stdout,v,3,"test");putchar(44);'
                  'knurl_check_summary(stdout,m,5,"test");putchar(44);'
                  'knurl_check_summary(stdout,v,1,"test");putchar(44);'
                  'knurl_check_summary(stdout,v,0,"test");putchar(44);'
                  'knurl_check_summary(stdout,z,3,"test");putchar(93);return 0;}\n')
        with tempfile.TemporaryDirectory() as path:
            src = Path(path) / "reference.c"
            exe = Path(path) / "reference"
            src.write_text(source)
            command = shlex.split(os.environ.get("CC", "cc"))
            build = subprocess.run(command + ["-std=c11", "-I", str(ROOT / "include"), str(src),
                                               str(ROOT / "src/check_common.c"), "-lm", "-o", str(exe)],
                                   capture_output=True, text=True, timeout=30)
            self.assertEqual(build.returncode, 0, build.stderr)
            data, modes, single, empty, zero_mean = json.loads(subprocess.check_output([str(exe)], text=True, timeout=5))
            self.assertEqual((data["min"], data["max"], data["mean"], data["median"], data["p95"]), (1, 3, 2, 2, 3))
            self.assertEqual(data["sd"], 1)
            self.assertEqual(data["sample_std"], 1)
            self.assertEqual(data["cv"], .5)
            self.assertEqual((data["p5"], data["p99"]), (1, 3))
            self.assertEqual(data["exact_mode"], [])
            self.assertEqual(data["exact_mode_status"], "none/all unique")
            self.assertEqual(modes["exact_mode"], [1, 2])
            self.assertEqual(modes["exact_mode_status"], "mode(s)")
            self.assertAlmostEqual(modes["sample_std"], math.sqrt(.7), places=12)
            self.assertAlmostEqual(modes["cv"], math.sqrt(.7) / 1.8, places=12)
            self.assertEqual(modes["percentile_method"], "nearest_rank")
            self.assertIsNone(single["sd"])
            self.assertIsNone(single["sample_std"])
            self.assertIsNone(single["cv"])
            self.assertIsNone(single["mean_ci95_low"])
            self.assertEqual(single["exact_mode"], [])
            self.assertEqual(empty["count"], 0)
            self.assertEqual(empty["exact_mode"], [])
            self.assertEqual(empty["exact_mode_status"], "none/no samples")
            self.assertIsNone(zero_mean["cv"])
            self.assertEqual(data["ci_degrees_of_freedom"], 2)
            self.assertAlmostEqual(data["mean_ci95_low"], 2 - 4.30265272975 / math.sqrt(3), places=10)
            self.assertAlmostEqual(data["mean_ci95_high"], 2 + 4.30265272975 / math.sqrt(3), places=10)


if __name__ == "__main__":
    unittest.main()
