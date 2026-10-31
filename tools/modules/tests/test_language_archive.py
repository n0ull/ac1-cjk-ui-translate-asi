"""One language candidate supports distinct host runs without mixing their identities."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import runtime_archive as archive
import runtime_candidate as candidate
import runtime_install as deploy
import test_language_deployment as language
import test_runtime_archive as runs
import test_runtime_install as fixtures


class LanguageArchiveTests(unittest.TestCase):
    make_candidate = fixtures.InstallationTests.make_candidate
    seal_candidate = fixtures.InstallationTests.seal_candidate
    snapshot = fixtures.InstallationTests.snapshot
    prepare = language.LanguageDeploymentTests.prepare

    def setUp(self):
        language.LanguageDeploymentTests.setUp(self)
        self.logs = self.work / "logs"
        self.logs.mkdir()
        (self.work / "inputs").mkdir()
        self.policy = self.work / "inputs/policy.txt"
        self.policy.write_bytes(b"")
        self.log = self.logs / f"run_{runs.RUN}.log"
        self.strings = self.logs / f"strings_{runs.RUN}.txt"
        self.strings.write_text(
            "# === run 2026-10-20 17:00:35 pid=42 词典=runtime package keys=2 ===\n"
            "[×1] 替换 caller=00492EC7 BACK\n",
            encoding="utf-8",
        )
        (self.logs / f"crash_{runs.RUN}.log").write_bytes(b"")
        manifest = candidate.verify(self.target)
        raw = (self.target / "install/scripts" / candidate.PACKAGE).read_bytes()
        self.package_line = f"[RTP] language=zh embedded_digest={raw[64:96].hex()} package_version=2 dictionary_rules=2"
        self.asi_line = (
            "[RTP] asi_sha256=" + manifest["files"]["install/scripts/" + candidate.ASI]["sha256"]
        )
        self.write_log("dx9")

    def write_log(self, host):
        identity = candidate.HOSTS[host]
        self.log.write_text(
            runs.log_line(f"=== ac1_cjk 已注入 pid=42 run={runs.RUN} ===", "17:00:34")
            + runs.log_line(
                f"[启动] 宿主 ✓ exe={identity['exe']} 基址=0x00400000 大小=3 字节 CRC32={identity['crc32']}（基准一致）"
            )
            + runs.log_line(self.package_line)
            + runs.log_line(self.asi_line)
            + runs.log_line("[RTP] single package active dictionary_keys=2 text_replace=1")
            + runs.log_line(runs.RESOURCE)
            + runs.log_line(runs.TEXT, "17:02:35"),
            encoding="utf-8",
        )

    def collect(self, name, **kwargs):
        return archive.archive_run(
            self.target, self.logs, runs.RUN, self.work / name, policy=self.policy, **kwargs
        )

    def test_either_declared_host_is_archived_with_its_own_identity(self):
        for host in ("dx9", "dx10"):
            self.write_log(host)
            before = self.snapshot(self.logs)
            report = self.collect(host)
            self.assertEqual(report["status"], "ARCHIVED")
            self.assertEqual(report["format_version"], 2)
            self.assertEqual(
                report["run"]["observations"]["binary_pair_identity"], "REPORTED_MATCHES"
            )
            self.assertEqual(
                report["run"]["observations"]["selected_host"][0], candidate.HOSTS[host]["exe"]
            )
            self.assertEqual(before, self.snapshot(self.logs))

    def test_wrong_language_package_or_asi_never_inherits_a_clean_result(self):
        original = self.log.read_text(encoding="utf-8")
        for name, text, code in (
            (
                "wrong-language",
                original.replace("language=zh", "language=ja"),
                "LOG_PACKAGE_IDENTITY_MISSING_OR_MISMATCH",
            ),
            (
                "wrong-digest",
                original.replace("embedded_digest=", "embedded_digest=f"),
                "PACKAGE_IDENTITY_UNRECOGNIZED",
            ),
            (
                "missing-asi",
                original.replace(runs.log_line(self.asi_line), ""),
                "LOG_ASI_IDENTITY_MISSING_OR_MISMATCH",
            ),
        ):
            with self.subTest(name=name):
                self.log.write_text(text, encoding="utf-8")
                report = self.collect(name)
                self.assertIn(code, {row["code"] for row in report["issues"]})
                self.assertEqual(report["status"], "ARCHIVED_WITH_ATTENTION")

    def test_mixed_hosts_are_not_a_single_accepted_run(self):
        with self.log.open("a", encoding="utf-8") as stream:
            stream.write(
                runs.log_line(
                    "[启动] 宿主 ✓ exe=AssassinsCreed_Dx10.exe CRC32=352441C2", "17:02:35"
                )
            )
        report = self.collect("mixed")
        self.assertFalse(report["run"]["observations"]["declared_host_matches"])

    def test_formal_multi_host_receipt_matches_and_other_language_does_not(self):
        self.prepare()
        deploy.install_plan(self.plan, self.receipt)
        report = self.collect("formal", receipt=self.receipt)
        self.assertEqual(report["deployment"]["verification"], "DECLARED_TARGET_MATCHES")
        other = language.language_candidate(self.work / "other", "ja")
        with self.assertRaisesRegex(ValueError, "does not target"):
            archive.archive_run(
                other,
                self.logs,
                runs.RUN,
                self.work / "wrong-receipt",
                policy=self.policy,
                receipt=self.receipt,
            )
        self.assertFalse((self.work / "wrong-receipt").exists())

    def test_language_policy_must_be_explicit(self):
        with self.assertRaisesRegex(ValueError, "explicit --notranslate"):
            archive.archive_run(
                self.target, self.logs, runs.RUN, self.work / "no-policy", policy=None
            )


if __name__ == "__main__":
    unittest.main()
