"""Read-only installation inspection through the same interface as the CLI."""

import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import runtime_candidate as candidate  # noqa: E402
import runtime_fixtures as fixtures  # noqa: E402
import runtime_install as install  # noqa: E402


class InstallationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ac1-install-test-")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.game, self.candidate = self.work / "game", self.work / "candidate"
        self.scripts = self.game / "scripts"
        self.scripts.mkdir(parents=True)
        # Independent standard CRC32 vectors replace only the host fixture table;
        # actual file reading, CRC calculation and candidate validation still run.
        hosts = {
            "dx9": dict(exe="AssassinsCreed_Dx9.exe", crc32="CBF43926"),
            "dx10": dict(exe="AssassinsCreed_Dx10.exe", crc32="352441C2"),
        }
        host_patch = patch.object(candidate, "HOSTS", hosts)
        host_patch.start()
        self.addCleanup(host_patch.stop)
        self.make_candidate(hosts["dx10"])
        (self.game / hosts["dx9"]["exe"]).write_bytes(b"123456789")
        (self.game / hosts["dx10"]["exe"]).write_bytes(b"abc")
        (self.game / "DataPC.forge").write_bytes(b"owned resource fixture")
        (self.game / "dinput8.dll").write_bytes(b"loader capability is not inferred")
        for name in install.PROJECT_FILES:
            (self.scripts / name).write_bytes(
                (self.candidate / "install/scripts" / name).read_bytes()
            )
        (self.scripts / "EaglePatchAC1.asi").write_bytes(b"another mod")
        (self.scripts / "SubtitleSynchAC1.asi").write_bytes(b"subtitle mod")
        (self.scripts / "SubtitleSynchAC1.ini").write_text("user configuration", encoding="utf-8")
        (self.scripts / "AC1_CJK").mkdir()
        (self.scripts / "AC1_CJK/run_old.log").write_text("old evidence", encoding="utf-8")

    def make_candidate(self, host):
        fixtures.language_candidate(self.candidate, hosts=[host])

    def seal_candidate(self, manifest):
        fixtures.seal_candidate(self.candidate, manifest)

    def inspect(self, name="first", baseline=None):
        return install.inspect_installation(
            self.candidate, self.game, self.work / "reports" / name, baseline
        )

    def snapshot(self, root):
        return {
            str(path.relative_to(root)): (candidate.fingerprint(path), path.stat().st_mtime_ns)
            for path in root.rglob("*")
            if path.is_file()
        }

    def test_installed_pair_and_both_host_columns_are_read_without_changes(self):
        game_before, candidate_before = self.snapshot(self.game), self.snapshot(self.candidate)
        report = self.inspect()
        self.assertEqual(report["hosts"][0]["state"], "MATCHES")
        self.assertEqual(report["project"]["state"], "MATCHES")
        self.assertEqual(report["issues"], [])
        self.assertEqual(report["hosts"][0]["actual"]["crc32"], "352441C2")
        self.assertEqual(report["loader"]["synchronous_initializeasi"], "NOT_VERIFIED_BY_INSPECTOR")
        self.assertEqual(self.snapshot(self.game), game_before)
        self.assertEqual(self.snapshot(self.candidate), candidate_before)
        self.make_candidate(candidate.HOSTS["dx9"])
        self.assertEqual(self.inspect("dx9")["hosts"][0]["actual"]["crc32"], "CBF43926")

    def test_absent_pair_is_distinct_from_partial_or_different(self):
        for name in install.PROJECT_FILES:
            (self.scripts / name).unlink()
        report = self.inspect()
        self.assertEqual(report["project"]["state"], "ABSENT")
        self.assertFalse(report["issues"])
        (self.scripts / candidate.ASI).write_bytes(b"previous version")
        report = self.inspect("partial")
        self.assertEqual(report["project"]["state"], "PARTIAL")
        self.assertIn("PROJECT_PAIR_PARTIAL", [row["code"] for row in report["issues"]])
        (self.scripts / candidate.PACKAGE).write_bytes(b"previous package")
        self.assertEqual(self.inspect("different")["project"]["state"], "DIFFERENT")

    def test_wrong_host_missing_host_and_missing_archive_are_reported(self):
        host = self.game / "AssassinsCreed_Dx10.exe"
        host.write_bytes(b"another build")
        self.assertEqual(self.inspect()["hosts"][0]["state"], "CRC_MISMATCH")
        host.unlink()
        (self.game / "DataPC.forge").unlink()
        codes = [row["code"] for row in self.inspect("missing")["issues"]]
        self.assertIn("HOST_MISSING", codes)
        self.assertIn("RESOURCE_ARCHIVE_MISSING", codes)

    def test_old_project_enabled_asis_are_listed_and_never_removed(self):
        legacy = self.scripts / "ac1_cjk.asi"
        legacy.write_bytes(b"old forge plugin")
        (self.scripts / "AC1_CJK_Forge.txt").write_text("old sidecar", encoding="utf-8")
        before = self.snapshot(self.game)
        report = self.inspect()
        self.assertEqual(
            [row["path"] for row in report["project"]["conflicts"]], ["scripts/ac1_cjk.asi"]
        )
        self.assertEqual(len(report["project"]["other_project_files"]), 2)
        self.assertEqual(self.snapshot(self.game), before)

    def test_disabled_legacy_plugin_is_observed_without_active_conflict(self):
        (self.scripts / "AC1_RT_Probe.asi.off").write_bytes(b"previous observer")
        report = self.inspect()
        self.assertFalse(report["project"]["conflicts"])
        self.assertEqual(report["project"]["other_project_files"][0]["activation"], "DISABLED")

    def test_renamed_and_nested_runtime_duplicates_are_detected(self):
        nested = self.scripts / "other"
        nested.mkdir()
        (nested / "copy.asi").write_bytes(
            (self.scripts / candidate.ASI).read_bytes() + b"older distinct bytes"
        )
        (self.game / "unexpected.asi").write_bytes((self.scripts / candidate.ASI).read_bytes())
        paths = {row["path"] for row in self.inspect()["project"]["conflicts"]}
        self.assertEqual(paths, {"scripts/other/copy.asi", "unexpected.asi"})

    def test_unknown_third_party_plugin_is_not_claimed_as_project(self):
        (self.scripts / "new_mod.asi").write_bytes(b"user owned mod")
        report = self.inspect()
        self.assertFalse(report["issues"])
        self.assertIn("scripts/new_mod.asi", report["third_party"]["files"])

    def test_subtitle_filename_toggle_is_separate_from_project_integrity(self):
        self.inspect()
        enabled = self.scripts / "SubtitleSynchAC1.asi"
        enabled.rename(enabled.with_name("SubtitleSynchAC1.asi.off"))
        before = self.snapshot(self.game)
        report = self.inspect("disabled", self.work / "reports/first/inspection.json")
        self.assertEqual(report["project"]["state"], "MATCHES")
        self.assertFalse(report["issues"])
        change = report["third_party"]["changes"]["renamed"]
        self.assertEqual(len(change), 1)
        self.assertTrue(change[0]["activation_changed"])
        self.assertEqual(self.snapshot(self.game), before)

    def test_third_party_configuration_change_is_preserved_and_reported(self):
        self.inspect()
        config = self.scripts / "SubtitleSynchAC1.ini"
        config.write_text("user changed configuration", encoding="utf-8")
        report = self.inspect("changed", self.work / "reports/first/inspection.json")
        self.assertFalse(report["issues"])
        self.assertEqual(
            report["third_party"]["changes"]["modified"][0]["path"], "scripts/SubtitleSynchAC1.ini"
        )
        self.assertEqual(config.read_text(encoding="utf-8"), "user changed configuration")

    def test_nested_logs_and_assets_are_not_hashed_but_nested_asis_are(self):
        (self.scripts / "AC1_CJK/big.dmp").write_bytes(b"unmanaged dump")
        (self.scripts / "AC1_CJK/third_party.asi").write_bytes(b"nested plugin")
        report = self.inspect()
        self.assertIn("scripts/AC1_CJK/third_party.asi", report["third_party"]["files"])
        self.assertNotIn("scripts/AC1_CJK/big.dmp", report["third_party"]["files"])
        self.assertNotIn("scripts/AC1_CJK/run_old.log", report["third_party"]["files"])

    def test_invalid_candidate_or_baseline_publishes_no_report(self):
        (self.candidate / "install/LICENSE").write_bytes(b"tampered")
        with self.assertRaisesRegex(ValueError, "digest mismatch"):
            self.inspect()
        self.assertFalse((self.work / "reports/first").exists())
        self.make_candidate(candidate.HOSTS["dx10"])
        baseline = self.work / "baseline/invalid.json"
        baseline.parent.mkdir()
        baseline.write_text("{}", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "same game directory"):
            self.inspect(baseline=baseline)
        self.assertFalse((self.work / "reports/first").exists())

    def test_baseline_from_another_installation_is_rejected(self):
        self.inspect()
        baseline = self.work / "reports/first/inspection.json"
        value = candidate.read_json(baseline)
        value["game"] = str(self.work / "another-game")
        baseline.write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "same game directory"):
            self.inspect("second", baseline)

    def test_malformed_baseline_file_identity_is_rejected(self):
        self.inspect()
        baseline = self.work / "reports/first/inspection.json"
        value = candidate.read_json(baseline)
        value["third_party"]["files"]["scripts/SubtitleSynchAC1.asi"]["sha256"] = "z" * 64
        baseline.write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Invalid baseline"):
            self.inspect("invalid-fingerprint", baseline)

    def test_publication_failure_does_not_leave_a_partial_report(self):
        before = self.snapshot(self.game)
        with patch.object(Path, "rename", side_effect=PermissionError("publication denied")):
            with self.assertRaises(PermissionError):
                self.inspect()
        self.assertFalse((self.work / "reports/first").exists())
        self.assertFalse(list((self.work / "reports").glob(".runtime-inspect-*")))
        self.assertEqual(self.snapshot(self.game), before)

    def test_outputs_cannot_modify_candidate_game_or_old_report(self):
        self.inspect()
        baseline = self.work / "reports/first/inspection.json"
        for root in (self.game, self.candidate, baseline.parent):
            with self.assertRaisesRegex(ValueError, "outside"):
                install.inspect_installation(
                    self.candidate, self.game, root / "new-output", baseline
                )
        before = baseline.read_bytes()
        with self.assertRaises(FileExistsError):
            self.inspect()
        self.assertEqual(baseline.read_bytes(), before)

    def test_file_change_during_read_rejects_the_snapshot(self):
        host = self.game / "AssassinsCreed_Dx10.exe"
        original_open = Path.open

        class ChangingReader:
            def __init__(self, stream):
                self.stream, self.changed = stream, False

            def __enter__(self):
                return self

            def __exit__(self, *args):
                self.stream.close()

            def fileno(self):
                return self.stream.fileno()

            def read(self, size):
                data = self.stream.read(size)
                if not self.changed:
                    self.changed = True
                    with original_open(host, "ab") as writer:
                        writer.write(b"changed while reading")
                return data

        def changing_open(path, mode="r", *args, **kwargs):
            stream = original_open(path, mode, *args, **kwargs)
            return ChangingReader(stream) if path == host and mode == "rb" else stream

        with patch.object(Path, "open", changing_open):
            with self.assertRaisesRegex(ValueError, "changed during inspection"):
                self.inspect()
        self.assertFalse((self.work / "reports/first").exists())

    def test_linked_roots_and_nested_plugin_links_do_not_escape(self):
        alias = self.work / "linked-game"
        outside = self.work / "outside.asi"
        outside.write_bytes(b"outside file")
        try:
            alias.symlink_to(self.game, target_is_directory=True)
            (self.scripts / "linked.asi").symlink_to(outside)
        except OSError as error:
            self.skipTest(f"Symlink creation unavailable: {error}")
        with self.assertRaisesRegex(ValueError, "Linked path"):
            install.inspect_installation(self.candidate, alias, self.work / "escaped")
        report = self.inspect()
        self.assertEqual(report["issues"][0]["code"], "LINKED_ENTRIES_NOT_INSPECTED")
        self.assertNotIn("scripts/linked.asi", report["third_party"]["files"])

    def test_unreadable_nested_directory_does_not_silently_hide_plugins(self):
        folder = self.scripts / "unreadable"
        folder.mkdir()
        original_scan = install.os.scandir

        def denied_scan(path):
            if Path(path) == folder:
                raise PermissionError("cannot inspect plugin directory")
            return original_scan(path)

        with patch.object(install.os, "scandir", denied_scan):
            with self.assertRaisesRegex(PermissionError, "cannot inspect"):
                self.inspect()
        self.assertFalse((self.work / "reports/first").exists())

    def test_cli_reports_content_issues_and_preserves_inspection_output(self):
        (self.scripts / candidate.PACKAGE).unlink()
        out = self.work / "cli"
        with contextlib.redirect_stdout(io.StringIO()):
            result = install.main(
                [
                    "inspect",
                    "--candidate",
                    str(self.candidate),
                    "--game",
                    str(self.game),
                    "--out",
                    str(out),
                ]
            )
        self.assertEqual(result, 1)
        self.assertEqual(
            candidate.read_json(out / "inspection.json")["project"]["state"], "PARTIAL"
        )

    def test_help_is_independent_of_working_directory(self):
        result = subprocess.run(
            [sys.executable, str(TOOLS / "runtime_install.py"), "inspect", "--help"],
            cwd=self.work,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--candidate", result.stdout)


if __name__ == "__main__":
    unittest.main()
