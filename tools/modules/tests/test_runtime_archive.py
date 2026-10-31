"""Read-only run archiving through the public function and CLI interface."""

import contextlib
import hashlib
import io
import json
import subprocess
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import runtime_archive as archive  # noqa: E402
import runtime_candidate as candidate  # noqa: E402
import runtime_install as deploy  # noqa: E402
import test_runtime_install as fixtures  # noqa: E402

RUN = "20261020_170034"
RESOURCE_BASE = "[状态] RTP gate=1 atlas_mask=15 uploaded_mask=15 expanded=4 uploads=4 fonts=4 glyphs=5820 retained=27424 rejects=0 faults=0 font_failed=0"
RESOURCE = RESOURCE_BASE + " font_active=2 font_peak=4 font_limit=32 font_full=0"
TEXT = "[状态] FUNNEL 触发=19 替换=7 未命中=2(ASCII 2/非ASCII 0) 跳过=空串 10/同值 0/词典未就绪 0/结构不可信 0/其它 0 容量不足=0 异常=0 样本=7+2/32"


def log_line(message, time="17:00:35", level="INFO"):
    return f"[2026-10-20 {time}][{level}] {message}\n"


class ArchiveTests(unittest.TestCase):
    make_candidate = fixtures.InstallationTests.make_candidate
    seal_candidate = fixtures.InstallationTests.seal_candidate
    snapshot = fixtures.InstallationTests.snapshot

    def setUp(self):
        fixtures.InstallationTests.setUp(self)
        dictionary = self.candidate / "evidence/package/dictionary-effective.json"
        dictionary.write_text(json.dumps({"BACK": "返回", "START": "开始"}), encoding="utf-8")
        manifest = candidate.read_json(self.candidate / "manifest.json")
        build = self.candidate / "evidence/package/build-report.json"
        receipt = candidate.read_json(build)
        receipt["dictionary"] = dict(rules_version=2, stats=dict(keys=2))
        build.write_text(json.dumps(receipt), encoding="utf-8")
        self.seal_candidate(manifest)
        self.logs, self.inputs = self.work / "logs", self.work / "inputs"
        self.logs.mkdir()
        self.inputs.mkdir()
        self.policy = self.inputs / "notranslate.txt"
        self.policy.write_text("_Yes\tinternal key\n", encoding="utf-8")
        self.run_log = self.logs / f"run_{RUN}.log"
        self.strings = self.logs / f"strings_{RUN}.txt"
        self.crash = self.logs / f"crash_{RUN}.log"
        self.run_log.write_text(
            log_line(f"=== ac1_cjk 已注入 pid=42 run={RUN} ===", "17:00:34")
            + log_line(
                "[启动] 宿主 ✓ exe=AssassinsCreed_Dx10.exe 基址=0x00400000 大小=3 字节 CRC32=352441C2（基准一致）"
            )
            + log_line(
                "[RTP] asi_sha256="
                + manifest["files"]["install/scripts/" + candidate.ASI]["sha256"]
            )
            + log_line(
                "[RTP] language=zh embedded_digest="
                + (self.candidate / "install/scripts" / candidate.PACKAGE).read_bytes()[64:96].hex()
                + " package_version=2 dictionary_rules=2"
            )
            + log_line("[RTP] single package active dictionary_keys=2 text_replace=1")
            + log_line(RESOURCE)
            + log_line(TEXT, "17:02:35"),
            encoding="utf-8",
        )
        self.strings.write_text(
            "# === run 2026-10-20 17:00:35 pid=42 词典=runtime package keys=2 ===\n"
            "[×1] 替换 caller=00BF5137 BACK\n"
            "[×2] 未命中 caller=00BF5137 UNTRANSLATED MENU\n",
            encoding="utf-8",
        )
        self.crash.write_bytes(b"")
        self.feedback = self.inputs / "feedback.json"
        self.feedback.write_text(
            json.dumps(
                dict(
                    format_version=1,
                    kind="AC1_RUNTIME_USER_FEEDBACK",
                    observations=[dict(run=RUN, text="运行正常。", source="test human statement")],
                )
            ),
            encoding="utf-8",
        )

    def collect(self, name="archive", **kwargs):
        return archive.archive_run(
            self.candidate, self.logs, RUN, self.work / name, policy=self.policy, **kwargs
        )

    def append(self, message, **kwargs):
        with self.run_log.open("a", encoding="utf-8") as stream:
            stream.write(log_line(message, **kwargs))

    def test_stable_run_is_sealed_without_changing_inputs_or_certifying_native(self):
        before = self.snapshot(self.work)
        report = self.collect(feedback=self.feedback)
        self.assertEqual(report["status"], "ARCHIVED")
        self.assertEqual(report["native"], "NOT_CERTIFIED_BY_TOOL")
        observed = report["run"]["observations"]
        self.assertEqual(observed["observed_log_span_seconds"], 121)
        self.assertEqual(observed["active_play_seconds"], "NOT_MEASURED")
        self.assertEqual(observed["run_completion"], "NOT_PROVEN")
        self.assertEqual(observed["text"]["last"]["fields"]["replaced"], 7)
        self.assertEqual(report["feedback"]["observations"][0]["text"], "运行正常。")
        self.assertEqual(report["deployment"]["verification"], "NOT_PROVEN")
        after = self.snapshot(self.work)
        self.assertTrue(all(after[name] == value for name, value in before.items()))
        out = self.work / "archive"
        for line in (out / "SHA256SUMS").read_text().splitlines():
            digest, relative = line.split("  ", 1)
            self.assertEqual(hashlib.sha256((out / relative).read_bytes()).hexdigest(), digest)
        audit = candidate.read_json(out / "text-audit/audit.json")
        self.assertEqual(audit["summary"]["categories"]["review"], 1)
        self.assertEqual(audit["inventories"][0]["path"], str(out / "logs" / self.strings.name))
        self.assertEqual(audit["entries"][1]["text"], "UNTRANSLATED MENU")
        self.assertNotIn(".runtime-archive-", (out / "archive.json").read_text(encoding="utf-8"))

    def test_missing_logs_are_preserved_as_unknown_not_empty_crash_or_clean_counters(self):
        self.crash.unlink()
        self.run_log.unlink()
        report = self.collect()
        self.assertEqual(report["status"], "ARCHIVED_WITH_ATTENTION")
        missing = [row["path"] for row in report["issues"] if row["code"] == "LOG_FILE_MISSING"]
        self.assertEqual(len(missing), 2)
        self.assertEqual(report["run"]["observations"]["state"], "MISSING")
        self.assertNotIn("resources", report["run"]["observations"])
        self.assertIsNone(report["snapshot"]["files"][f"logs/crash_{RUN}.log"]["captured"])

    def test_current_font_tracking_counters_are_preserved(self):
        report = self.collect()
        self.assertEqual(report["status"], "ARCHIVED")
        fields = report["run"]["observations"]["resources"]["last"]["fields"]
        self.assertEqual(
            {key: fields[key] for key in archive.FONT_TRACK_FIELDS},
            dict(font_active=2, font_peak=4, font_limit=32, font_full=0),
        )

    def test_incomplete_or_impossible_font_tracking_does_not_report_a_clean_status(self):
        original = self.run_log.read_text(encoding="utf-8")
        invalid = (
            "",
            " font_active=2",
            " font_active=5 font_peak=4 font_limit=32 font_full=0",
            " font_active=2 font_peak=33 font_limit=32 font_full=0",
            " font_active=0 font_peak=0 font_limit=0 font_full=0",
            " font_active=2 font_peak=4 font_limit=32 font_full=-1",
            " font_active=2 font_peak=4 font_limit=32 font_full=0 font_active=2",
        )
        for index, suffix in enumerate(invalid):
            with self.subTest(suffix=suffix):
                self.run_log.write_text(
                    original + log_line(RESOURCE_BASE + suffix, time="17:02:36"), encoding="utf-8"
                )
                report = self.collect(name=f"invalid-tracking-{index}")
                self.assertIsNone(report["run"]["observations"]["resources"]["last"])
                self.assertIn(
                    "RESOURCE_STATUS_UNRECOGNIZED", {row["code"] for row in report["issues"]}
                )

    def test_nonzero_tracking_exhaustion_is_an_error_even_without_a_matching_reject_snapshot(self):
        self.append(
            RESOURCE_BASE + " font_active=32 font_peak=32 font_limit=32 font_full=1",
            time="17:02:36",
        )
        report = self.collect()
        self.assertEqual(report["status"], "ARCHIVED_WITH_ATTENTION")
        errors = report["run"]["observations"]["nonzero_error_samples"]
        self.assertEqual(errors[-1]["counters"], dict(font_full=1))

    def test_nonempty_crash_warning_and_early_error_followed_by_zero_are_retained(self):
        self.append(RESOURCE.replace("faults=0", "faults=3"))
        self.append(TEXT.replace("容量不足=0", "容量不足=2"))
        self.append(RESOURCE)
        self.append(TEXT)
        self.append("native operation failed", level="WARN")
        self.crash.write_bytes(b"C0000005 historical hook stack")
        report = self.collect()
        codes = {row["code"] for row in report["issues"]}
        self.assertTrue(
            {"NONZERO_ERROR_COUNTERS", "CRASH_LOG_NONEMPTY", "WARNING_OR_ERROR_LINES"} <= codes
        )
        samples = report["run"]["observations"]["nonzero_error_samples"]
        self.assertEqual(samples[0]["counters"], {"faults": 3})
        self.assertEqual(samples[1]["counters"], {"capacity_failures": 2})
        self.assertEqual(report["run"]["observations"]["resources"]["last"]["fields"]["faults"], 0)

    def test_err_preserves_severity_and_retired_error_spelling_is_unparsed(self):
        for level in ("ERR", "ERROR"):
            self.append("!! dictionary rejected: unsupported_value_unit", level=level)
        report = self.collect()
        warnings = report["run"]["observations"]["warnings"]
        self.assertEqual([row["level"] for row in warnings], ["ERR"])
        self.assertIn("unsupported_value_unit", warnings[0]["message"])
        codes = {row["code"] for row in report["issues"]}
        self.assertIn("WARNING_OR_ERROR_LINES", codes)
        self.assertIn("UNPARSED_LOG_LINES", codes)

    def test_missing_or_malformed_last_status_is_not_read_as_zero(self):
        self.append("[状态] RTP gate=1 faults=invalid")
        self.append("[状态] FUNNEL 替换=not-a-number")
        report = self.collect()
        summary = report["run"]["observations"]
        self.assertIsNone(summary["resources"]["last"])
        self.assertIsNone(summary["text"]["last"])
        self.assertEqual(report["status"], "ARCHIVED_WITH_ATTENTION")
        self.run_log.write_text(
            log_line(f"=== ac1_cjk 已注入 pid=42 run={RUN} ==="), encoding="utf-8"
        )
        report = self.collect("without-counters")
        self.assertIsNone(report["run"]["observations"]["text"]["last"])
        self.assertIn("TEXT_STATUS_MISSING", {row["code"] for row in report["issues"]})

    def test_unchanged_counter_is_preserved(self):
        self.append(TEXT.replace("/同值 0/", "/同值 3/"))
        report = self.collect()
        fields = report["run"]["observations"]["text"]["last"]["fields"]
        self.assertEqual(fields["skip_unchanged"], 3)
        self.assertNotIn("TEXT_STATUS_UNRECOGNIZED", {row["code"] for row in report["issues"]})

    def test_wrong_host_mixed_run_and_disabled_replacement_do_not_become_clean(self):
        self.append("=== ac1_cjk 已注入 pid=43 run=20261020_180000 ===")
        self.append("[启动] 宿主 ✓ exe=AssassinsCreed_Dx9.exe CRC32=CBF43926")
        self.append("[RTP] single package active dictionary_keys=2 text_replace=0")
        report = self.collect()
        codes = {row["code"] for row in report["issues"]}
        self.assertIn("RUN_IDENTITY_MISSING_OR_MIXED", codes)
        self.assertIn("LOG_HOST_MISSING_OR_MISMATCH", codes)
        self.assertIn("PACKAGE_ACTIVATION_UNRECOGNIZED_OR_REPLACEMENT_DISABLED", codes)
        self.assertEqual(report["text_audit"]["status"], "UNAVAILABLE")

    def test_invalid_utf8_and_trailing_log_fragment_are_archived_as_attention(self):
        self.run_log.write_bytes(self.run_log.read_bytes() + b"\xff")
        report = self.collect()
        self.assertEqual(report["run"]["observations"]["state"], "UNREADABLE_UTF8")
        self.assertEqual(
            (self.work / "archive/logs" / self.run_log.name).read_bytes(), self.run_log.read_bytes()
        )
        self.run_log.write_bytes(log_line("partial data").encode()[:-1])
        report = self.collect("fragment")
        self.assertIn("RUN_TRAILING_PARTIAL_LINE", {row["code"] for row in report["issues"]})

    def test_inventory_pid_time_mixed_headers_or_malformed_rows_never_silently_audit(self):
        original = self.strings.read_text(encoding="utf-8")
        for label, content in (
            ("pid", original.replace("pid=42", "pid=999")),
            ("time", original.replace("2026-10-20", "2026-10-19")),
            ("multiple", original + original),
            ("malformed", original + "broken row\n"),
        ):
            with self.subTest(label=label):
                self.strings.write_text(content, encoding="utf-8")
                report = self.collect(label)
                self.assertEqual(report["text_audit"]["status"], "UNAVAILABLE")
                self.assertEqual(report["status"], "ARCHIVED_WITH_ATTENTION")

    def test_feedback_must_belong_to_selected_run_and_does_not_propagate_to_other_runs(self):
        value = candidate.read_json(self.feedback)
        value["observations"][0]["run"] = "20261020_170016"
        self.feedback.write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "selected run"):
            self.collect(feedback=self.feedback)
        self.assertFalse((self.work / "archive").exists())
        self.assertFalse(list(self.work.glob(".runtime-archive-*")))

    def formal_receipt(self):
        plan, receipt = self.work / "plan", self.work / "deployment"
        deploy.plan_installation(self.candidate, self.game, plan)
        host = self.game / "AssassinsCreed_Dx10.exe"

        class Guard:
            evidence = dict(files={host.name: candidate.fingerprint(host)})

            def check(self):
                pass

        with patch.object(deploy, "ClosedGame", lambda game: contextlib.nullcontext(Guard())):
            deploy.install_plan(plan, receipt)
        return receipt

    def test_formal_receipt_is_checked_without_reading_game_or_modifying_receipt(self):
        receipt = self.formal_receipt()
        before = self.snapshot(receipt)
        self.game.rename(self.work / "game-offline")
        report = self.collect(receipt=receipt)
        self.assertEqual(report["deployment"]["verification"], "DECLARED_TARGET_MATCHES")
        self.assertEqual(report["deployment"]["recorded_result"], "DEPLOYED")
        self.assertEqual(self.snapshot(receipt), before)

    def test_interrupted_formal_deployment_is_archived_as_attention(self):
        receipt = self.formal_receipt()
        (receipt / "result.json").unlink()
        (receipt / "result.json.sha256").unlink()
        report = self.collect(receipt=receipt)
        self.assertEqual(report["status"], "ARCHIVED_WITH_ATTENTION")
        self.assertEqual(report["deployment"]["recorded_result"], "INTERRUPTED")
        self.assertIn("DEPLOYMENT_NOT_COMPLETED", {row["code"] for row in report["issues"]})

    def test_receipt_for_other_candidate_or_bad_seal_is_rejected_without_publication(self):
        receipt = self.formal_receipt()
        manifest = candidate.read_json(self.candidate / "manifest.json")
        manifest["version"] = "another-candidate"
        self.seal_candidate(manifest)
        with self.assertRaisesRegex(ValueError, "selected candidate"):
            self.collect(receipt=receipt)
        self.assertFalse((self.work / "archive").exists())
        (receipt / "transaction.json.sha256").write_text("0" * 64, encoding="ascii")
        with self.assertRaisesRegex(ValueError, "digest mismatch"):
            self.collect("bad-seal", receipt=receipt)

    def changing_reader(self, always=False):
        original = Path.open
        mutated = False
        run_log = self.run_log

        class Reader:
            def __init__(self, stream):
                self.stream, self.changed = stream, False

            def __enter__(self):
                return self

            def __exit__(self, *args):
                self.stream.close()

            def fileno(self):
                return self.stream.fileno()

            def read(self, size):
                nonlocal mutated
                block = self.stream.read(size)
                if block and not self.changed and (always or not mutated):
                    self.changed, mutated = True, True
                    with original(run_log, "ab") as writer:
                        writer.write(log_line("concurrent log write", time="17:02:35").encode())
                return block

        def opening(path, mode="r", *args, **kwargs):
            stream = original(path, mode, *args, **kwargs)
            return Reader(stream) if path == run_log and mode == "rb" else stream

        return patch.object(Path, "open", opening)

    def test_growing_log_retries_entire_set_then_archives_stable_copy(self):
        with self.changing_reader():
            report = self.collect()
        self.assertEqual(report["snapshot"]["state"], "STABLE")
        self.assertEqual(len(report["snapshot"]["attempts"]), 2)
        self.assertEqual(
            (self.work / "archive/logs" / self.run_log.name).read_bytes(), self.run_log.read_bytes()
        )

    def test_persistently_growing_logs_publish_incomplete_without_clean_assessment(self):
        with self.changing_reader(always=True):
            report = self.collect(feedback=self.feedback)
        self.assertEqual(report["status"], "INCOMPLETE_SNAPSHOT")
        self.assertEqual(len(report["snapshot"]["attempts"]), 3)
        self.assertEqual(report["run"]["observations"]["state"], "NOT_ASSESSED")
        self.assertEqual(report["text_audit"]["status"], "NOT_ASSESSED")
        self.assertEqual(report["feedback"]["observations"], [])
        self.assertFalse((self.work / "archive/text-audit").exists())

    def test_later_inventory_read_invalidates_earlier_run_copy_and_retries_set(self):
        original = Path.open
        changed = False
        run_log = self.run_log

        class Reader:
            def __init__(self, stream):
                self.stream = stream

            def __enter__(self):
                return self

            def __exit__(self, *args):
                self.stream.close()

            def fileno(self):
                return self.stream.fileno()

            def read(self, size):
                nonlocal changed
                data = self.stream.read(size)
                if data and not changed:
                    changed = True
                    with original(run_log, "ab") as writer:
                        writer.write(log_line("late run write", time="17:02:35").encode())
                return data

        def opening(path, mode="r", *args, **kwargs):
            stream = original(path, mode, *args, **kwargs)
            return Reader(stream) if path == self.strings and mode == "rb" else stream

        with patch.object(Path, "open", opening):
            report = self.collect()
        self.assertEqual(len(report["snapshot"]["attempts"]), 2)
        self.assertEqual(report["snapshot"]["state"], "STABLE")
        self.assertEqual(
            (self.work / "archive/logs" / run_log.name).read_bytes(), run_log.read_bytes()
        )

    def test_candidate_change_during_capture_rejects_archive_even_if_new_seal_is_valid(self):
        original = Path.open
        changed = False

        def opening(path, mode="r", *args, **kwargs):
            nonlocal changed
            stream = original(path, mode, *args, **kwargs)
            if path == self.run_log and mode == "rb" and not changed:
                changed = True
                manifest = candidate.read_json(self.candidate / "manifest.json")
                manifest["version"] = "concurrent-new-candidate"
                self.seal_candidate(manifest)
            return stream

        with patch.object(Path, "open", opening):
            with self.assertRaisesRegex(ValueError, "Candidate changed"):
                self.collect()
        self.assertFalse((self.work / "archive").exists())
        self.assertFalse(list(self.work.glob(".runtime-archive-*")))

    def test_empty_selection_invalid_run_input_overlap_and_invalid_candidate_reject(self):
        for run in ("../outside", "20261399_000000", "20261020_235959"):
            with self.assertRaises(ValueError):
                archive.archive_run(
                    self.candidate, self.logs, run, self.work / "archive", policy=self.policy
                )
        for root in (self.logs, self.candidate, self.inputs, self.game):
            logs = self.scripts / "AC1_CJK" if root == self.game else self.logs
            with self.assertRaisesRegex(ValueError, "outside"):
                archive.archive_run(self.candidate, logs, RUN, root / "output", policy=self.policy)
        (self.candidate / "install/LICENSE").write_bytes(b"invalid")
        with self.assertRaisesRegex(ValueError, "digest mismatch"):
            self.collect()
        self.assertFalse((self.work / "archive").exists())

    def test_links_are_rejected_and_unrelated_logs_are_not_copied(self):
        (self.logs / "unrelated.dmp").write_bytes(b"do not copy")
        report = self.collect()
        self.assertNotIn("unrelated.dmp", str(report))
        self.assertFalse((self.work / "archive/logs/unrelated.dmp").exists())
        alias = self.work / "logs-link"
        try:
            alias.symlink_to(self.logs, target_is_directory=True)
        except OSError as error:
            self.skipTest(f"Symlink creation unavailable: {error}")
        with self.assertRaisesRegex(ValueError, "Linked path"):
            archive.archive_run(
                self.candidate, alias, RUN, self.work / "linked-out", policy=self.policy
            )

    def test_output_failure_and_existing_output_leave_inputs_and_old_reports_intact(self):
        before = self.snapshot(self.work)
        with patch.object(Path, "rename", side_effect=PermissionError("publication denied")):
            with self.assertRaisesRegex(PermissionError, "publication denied"):
                self.collect()
        self.assertEqual(self.snapshot(self.work), before)
        self.assertFalse(list(self.work.glob(".runtime-archive-*")))
        self.collect()
        old = self.snapshot(self.work / "archive")
        with self.assertRaises(FileExistsError):
            self.collect()
        self.assertEqual(self.snapshot(self.work / "archive"), old)

    def test_cli_success_attention_rejection_and_help_outside_working_directory(self):
        args = [
            "--candidate",
            str(self.candidate),
            "--logs",
            str(self.logs),
            "--run",
            RUN,
            "--notranslate",
            str(self.policy),
        ]
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(archive.main([*args, "--out", str(self.work / "cli")]), 0)
            self.crash.unlink()
            self.assertEqual(archive.main([*args, "--out", str(self.work / "attention")]), 1)
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(archive.main([*args, "--out", str(self.work / "cli")]), 2)
        result = subprocess.run(
            [sys.executable, str(Path(archive.__file__)), "--help"],
            cwd=self.work,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--deployment", result.stdout)


if __name__ == "__main__":
    unittest.main()
