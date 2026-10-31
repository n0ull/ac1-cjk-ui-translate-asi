"""Deployment behavior with real temporary files and a controlled game-closure adapter."""

import contextlib
import io
import json
import os
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import runtime_candidate as candidate  # noqa: E402
import runtime_install as deploy  # noqa: E402
import runtime_install as install  # noqa: E402
import test_runtime_install as fixtures  # noqa: E402


class DeploymentTests(unittest.TestCase):
    make_candidate = fixtures.InstallationTests.make_candidate
    seal_candidate = fixtures.InstallationTests.seal_candidate
    snapshot = fixtures.InstallationTests.snapshot

    def setUp(self):
        fixtures.InstallationTests.setUp(self)
        self.plan = self.work / "plan"
        self.receipt = self.work / "deployment"
        self.rollback = self.work / "rollback"
        host = self.game / "AssassinsCreed_Dx10.exe"

        class Guard:
            evidence = dict(status="TEST_ADAPTER", files={host.name: candidate.fingerprint(host)})

            def check(self):
                pass

        self.guard = Guard()
        gate = patch.object(deploy, "ClosedGame", lambda game: contextlib.nullcontext(self.guard))
        gate.start()
        self.addCleanup(gate.stop)

    def prepare(self):
        return deploy.plan_installation(self.candidate, self.game, self.plan)

    def install(self):
        return deploy.install_plan(self.plan, self.receipt)

    def old_pair(self):
        for name in install.PROJECT_FILES:
            (self.scripts / name).write_bytes(("previous " + name).encode())

    def fail_second_commit(self, fail_recovery=False):
        original = deploy.replace_file

        def fail(source, target):
            is_commit = Path(source).parent.name.startswith(".ac1-runtime-")
            if is_commit and target.name == candidate.ASI:
                raise PermissionError("second commit denied")
            if (
                fail_recovery
                and target.name == candidate.PACKAGE
                and deploy.identity(source)
                != candidate.fingerprint(self.candidate / "install/scripts" / candidate.PACKAGE)
            ):
                raise PermissionError("recovery denied")
            original(source, target)

        return patch.object(deploy, "replace_file", fail)

    def test_first_install_and_rollback_restore_absence_preserving_other_files(self):
        for name in install.PROJECT_FILES:
            (self.scripts / name).unlink()
        before = self.snapshot(self.game)
        self.prepare()
        self.assertEqual(self.install()["status"], "DEPLOYED")
        self.assertEqual(
            deploy.verify_deployment(self.receipt, self.work / "verify")["status"], "VERIFIED"
        )
        self.assertEqual(
            deploy.rollback_deployment(self.receipt, self.rollback)["status"], "ROLLED_BACK"
        )
        self.assertEqual(self.snapshot(self.game), before)

    def test_upgrade_backs_up_complete_pair_and_restores_original_bytes_and_mtimes(self):
        self.old_pair()
        before = self.snapshot(self.game)
        source_before = self.snapshot(self.candidate)
        self.prepare()
        self.assertEqual(self.install()["status"], "DEPLOYED")
        for name in install.PROJECT_FILES:
            self.assertEqual(
                (self.receipt / "backup" / name).read_bytes(), ("previous " + name).encode()
            )
        self.assertEqual(self.snapshot(self.candidate), source_before)
        deploy.rollback_deployment(self.receipt, self.rollback)
        self.assertEqual(self.snapshot(self.game), before)

    def test_matching_pair_is_not_replaced_but_receipt_still_verifies(self):
        before = self.snapshot(self.game)
        self.prepare()
        with patch.object(
            deploy, "replace_file", side_effect=AssertionError("unneeded replacement")
        ):
            self.assertEqual(self.install()["status"], "DEPLOYED")
        self.assertEqual(self.snapshot(self.game), before)

    def test_plan_rejects_partial_pair_missing_loader_and_extra_own_plugin(self):
        for mode in ("partial", "loader", "conflict"):
            with self.subTest(mode=mode):
                path = {
                    "partial": self.scripts / candidate.PACKAGE,
                    "loader": self.game / "dinput8.dll",
                    "conflict": self.scripts / "ac1_rt_probe.asi",
                }[mode]
                if mode == "conflict":
                    path.write_bytes(b"legacy observer")
                    previous = None
                else:
                    previous = path.read_bytes()
                    path.unlink()
                before = self.snapshot(self.game)
                with self.assertRaisesRegex(ValueError, "cannot be planned"):
                    self.prepare()
                self.assertFalse(self.plan.exists())
                self.assertEqual(before, self.snapshot(self.game))
                if previous is None:
                    path.unlink()
                else:
                    path.write_bytes(previous)

    def test_stale_project_candidate_host_and_resource_are_refused_without_writes(self):
        for mode in ("project", "candidate", "host", "resource", "loader"):
            with self.subTest(mode=mode):
                plan = self.work / ("plan-" + mode)
                deploy.plan_installation(self.candidate, self.game, plan)
                path = {
                    "project": self.scripts / candidate.ASI,
                    "candidate": self.candidate / "install/LICENSE",
                    "host": self.game / "AssassinsCreed_Dx10.exe",
                    "resource": self.game / "DataPC.forge",
                    "loader": self.game / "dinput8.dll",
                }[mode]
                previous = path.read_bytes()
                path.write_bytes(previous + b"user changed")
                before = self.snapshot(self.game)
                with self.assertRaises(ValueError):
                    deploy.install_plan(plan, self.receipt)
                self.assertFalse(self.receipt.exists())
                self.assertEqual(self.snapshot(self.game), before)
                path.write_bytes(previous)

    def test_process_guard_failure_and_lost_guard_never_write_targets(self):
        self.prepare()
        before = self.snapshot(self.game)
        with patch.object(deploy, "ClosedGame", side_effect=RuntimeError("running or unknown")):
            with self.assertRaisesRegex(RuntimeError, "running or unknown"):
                self.install()
        with patch.object(self.guard, "check", side_effect=RuntimeError("guard exited")):
            with self.assertRaisesRegex(RuntimeError, "guard exited"):
                self.install()
        self.assertFalse(self.receipt.exists())
        self.assertEqual(self.snapshot(self.game), before)

    def test_second_commit_failure_restores_both_old_files(self):
        self.old_pair()
        before = self.snapshot(self.game)
        self.prepare()
        with self.fail_second_commit():
            result = self.install()
        self.assertEqual(result["status"], "FAILED_RESTORED")
        self.assertEqual(self.snapshot(self.game), before)
        self.assertFalse(list(self.scripts.glob(".ac1-runtime-*")))
        self.assertEqual(
            deploy.verify_deployment(self.receipt, self.work / "verify")["status"], "ATTENTION"
        )

    def test_first_install_partial_failure_restores_absence(self):
        for name in install.PROJECT_FILES:
            (self.scripts / name).unlink()
        before = self.snapshot(self.game)
        self.prepare()
        with self.fail_second_commit():
            result = self.install()
        self.assertEqual(result["status"], "FAILED_RESTORED")
        self.assertEqual(self.snapshot(self.game), before)

    def test_recovery_failure_is_explicit_and_later_rollback_uses_retained_backup(self):
        self.old_pair()
        before = self.snapshot(self.game)
        self.prepare()
        with self.fail_second_commit(fail_recovery=True):
            result = self.install()
        self.assertEqual(result["status"], "RECOVERY_REQUIRED")
        self.assertIn("recovery denied", result["recovery_error"])
        self.assertTrue((self.receipt / "backup" / candidate.PACKAGE).exists())
        self.assertEqual(
            deploy.rollback_deployment(self.receipt, self.rollback)["status"], "ROLLED_BACK"
        )
        self.assertEqual(self.snapshot(self.game), before)

    def test_interruption_leaves_precommit_record_that_can_be_recovered(self):
        self.old_pair()
        before = self.snapshot(self.game)
        self.prepare()
        original = deploy.replace_file

        def interrupt(source, target):
            if target.name == candidate.ASI:
                raise KeyboardInterrupt("simulated process interruption")
            original(source, target)

        with patch.object(deploy, "replace_file", interrupt):
            with self.assertRaises(KeyboardInterrupt):
                self.install()
        self.assertTrue((self.receipt / "transaction.json").exists())
        self.assertFalse((self.receipt / "result.json").exists())
        self.assertEqual(
            deploy.verify_deployment(self.receipt, self.work / "verify")["recorded_result"],
            "INTERRUPTED",
        )
        deploy.rollback_deployment(self.receipt, self.rollback)
        self.assertEqual(self.snapshot(self.game), before)

    def test_backup_failure_never_starts_replacement(self):
        self.old_pair()
        self.prepare()
        before = self.snapshot(self.game)
        with patch.object(deploy.shutil, "copy2", side_effect=PermissionError("backup denied")):
            with self.assertRaisesRegex(PermissionError, "backup denied"):
                self.install()
        self.assertFalse((self.receipt / "transaction.json").exists())
        self.assertEqual(self.snapshot(self.game), before)

    def test_user_changes_and_corrupt_backup_block_rollback(self):
        self.old_pair()
        self.prepare()
        self.install()
        active = self.scripts / candidate.ASI
        deployed = active.read_bytes()
        active.write_bytes(b"user replacement")
        before = self.snapshot(self.game)
        with self.assertRaisesRegex(ValueError, "changed since deployment"):
            deploy.rollback_deployment(self.receipt, self.rollback)
        self.assertEqual(before, self.snapshot(self.game))
        active.write_bytes(deployed)
        (self.receipt / "backup" / candidate.PACKAGE).write_bytes(b"corrupt")
        with self.assertRaisesRegex(ValueError, "backup changed"):
            deploy.rollback_deployment(self.receipt, self.rollback)
        self.assertFalse(self.rollback.exists())

    def test_failed_rollback_is_retried_from_its_own_event(self):
        self.old_pair()
        before = self.snapshot(self.game)
        self.prepare()
        self.install()
        original = deploy.replace_file

        def fail_package(source, target):
            if target.name == candidate.PACKAGE:
                raise PermissionError("rollback package denied")
            original(source, target)

        with patch.object(deploy, "replace_file", fail_package):
            self.assertEqual(
                deploy.rollback_deployment(self.receipt, self.rollback)["status"],
                "RECOVERY_REQUIRED",
            )
        with self.assertRaisesRegex(ValueError, "changed since deployment"):
            deploy.rollback_deployment(self.receipt, self.work / "wrong-retry")
        retry = deploy.rollback_deployment(self.rollback, self.work / "retry")
        self.assertEqual(retry["status"], "ROLLED_BACK")
        self.assertEqual(self.snapshot(self.game), before)

    def test_interrupted_rollback_can_resume_without_candidate_directory(self):
        self.old_pair()
        before = self.snapshot(self.game)
        self.prepare()
        self.install()
        original = deploy.replace_file

        def interrupt_package(source, target):
            if target.name == candidate.PACKAGE:
                raise KeyboardInterrupt("interrupted rollback")
            original(source, target)

        with patch.object(deploy, "replace_file", interrupt_package):
            with self.assertRaises(KeyboardInterrupt):
                deploy.rollback_deployment(self.receipt, self.rollback)
        self.candidate.rename(self.work / "moved-candidate")
        self.assertFalse((self.rollback / "result.json").exists())
        deploy.rollback_deployment(self.rollback, self.work / "retry")
        self.assertEqual(self.snapshot(self.game), before)

    def test_result_publication_failure_restores_pair_and_leaves_no_partial_success(self):
        self.old_pair()
        before = self.snapshot(self.game)
        self.prepare()
        original = Path.rename
        failed = False

        def deny_result(path, target):
            nonlocal failed
            if Path(target).name == "result.json" and not failed:
                failed = True
                raise PermissionError("result publication denied")
            return original(path, target)

        with patch.object(Path, "rename", deny_result):
            result = self.install()
        self.assertEqual(result["status"], "FAILED_RESTORED")
        self.assertEqual(self.snapshot(self.game), before)
        self.assertEqual(
            deploy.read_record(self.receipt, "result.json")["status"], "FAILED_RESTORED"
        )
        self.assertFalse(list(self.receipt.glob(".record-*")))

    def test_new_conflict_between_plan_and_exclusive_lock_stops_install(self):
        self.prepare()

        @contextlib.contextmanager
        def gate(game):
            (self.scripts / "ac1_rt_probe.asi").write_bytes(b"new observer")
            yield self.guard

        expected = deploy.pair(self.game)
        with patch.object(deploy, "ClosedGame", gate):
            with self.assertRaisesRegex(ValueError, "active project ASI"):
                self.install()
        self.assertEqual(deploy.pair(self.game), expected)
        self.assertFalse(self.receipt.exists())

    def test_verifier_separates_third_party_changes_from_conflicts_and_host_changes(self):
        self.prepare()
        self.install()
        (self.scripts / "AC1_RT_probe.asi.off").write_bytes(b"disabled observer")
        (self.scripts / "new_mod.asi").write_bytes(b"third party")
        report = deploy.verify_deployment(self.receipt, self.work / "new-mod")
        self.assertEqual(report["status"], "VERIFIED")
        self.assertEqual(list(report["third_party"]["added"]), ["scripts/new_mod.asi"])
        (self.scripts / "ac1_rt_active.asi").write_bytes(b"active observer")
        report = deploy.verify_deployment(self.receipt, self.work / "conflict")
        self.assertEqual(report["status"], "ATTENTION")
        self.assertTrue(report["project"]["matches"])
        self.assertEqual(report["conflicts"][0]["path"], "scripts/ac1_rt_active.asi")
        (self.scripts / "ac1_rt_active.asi").unlink()
        (self.game / "AssassinsCreed_Dx10.exe").write_bytes(b"new host")
        report = deploy.verify_deployment(self.receipt, self.work / "changed-host")
        self.assertEqual(report["status"], "ATTENTION")
        self.assertFalse(report["protected"]["AssassinsCreed_Dx10.exe"]["matches"])

    def test_first_install_can_create_scripts_and_rollback_removes_only_managed_files(self):
        game = self.work / "game-without-scripts"
        game.mkdir()
        for name in ("AssassinsCreed_Dx10.exe", "DataPC.forge", "dinput8.dll"):
            (game / name).write_bytes((self.game / name).read_bytes())
        before = self.snapshot(game)
        deploy.plan_installation(self.candidate, game, self.plan)
        self.install()
        deploy.rollback_deployment(self.receipt, self.rollback)
        self.assertEqual(self.snapshot(game), before)
        self.assertTrue((game / "scripts").is_dir())

    def test_third_party_toggles_configuration_and_logs_survive_install_and_rollback(self):
        self.old_pair()
        self.prepare()
        subtitle = self.scripts / "SubtitleSynchAC1.asi"
        subtitle.rename(subtitle.with_name("SubtitleSynchAC1.asi.off"))
        config = self.scripts / "SubtitleSynchAC1.ini"
        config.write_text("new user configuration", encoding="utf-8")
        log = self.scripts / "AC1_CJK/run_old.log"
        log.write_text("new evidence", encoding="utf-8")
        self.install()
        subtitle = self.scripts / "SubtitleSynchAC1.asi.off"
        subtitle.rename(subtitle.with_name("SubtitleSynchAC1.asi"))
        verify = deploy.verify_deployment(self.receipt, self.work / "verify")
        self.assertEqual(verify["status"], "VERIFIED")
        self.assertTrue(verify["third_party"]["renamed"][0]["activation_changed"])
        deploy.rollback_deployment(self.receipt, self.rollback)
        self.assertEqual(config.read_text(encoding="utf-8"), "new user configuration")
        self.assertEqual(log.read_text(encoding="utf-8"), "new evidence")
        self.assertTrue((self.scripts / "SubtitleSynchAC1.asi").exists())

    def test_tampered_plan_and_receipt_do_not_select_unmanaged_paths(self):
        self.prepare()
        plan = self.plan / "plan.json"
        original = plan.read_bytes()
        plan.write_bytes(original + b" ")
        with self.assertRaisesRegex(ValueError, "digest mismatch"):
            self.install()
        plan.write_bytes(original)
        self.install()
        record = self.receipt / "transaction.json"
        record.write_bytes(record.read_bytes() + b" ")
        before = self.snapshot(self.game)
        with self.assertRaisesRegex(ValueError, "digest mismatch"):
            deploy.rollback_deployment(self.receipt, self.rollback)
        self.assertEqual(before, self.snapshot(self.game))

    def test_output_collisions_and_input_containment_are_rejected(self):
        for out in (self.game / "output", self.candidate / "output", self.work):
            with self.assertRaises((ValueError, FileExistsError)):
                deploy.plan_installation(self.candidate, self.game, out)
        self.prepare()
        with self.assertRaises(FileExistsError):
            self.prepare()
        with self.assertRaisesRegex(ValueError, "outside"):
            deploy.install_plan(self.plan, self.plan / "receipt")
        self.install()
        with self.assertRaises(FileExistsError):
            self.install()

    def test_cli_plan_install_verify_rollback_and_failure_exit_codes(self):
        for command, args in (
            ("plan", ["--candidate", str(self.candidate), "--game", str(self.game)]),
            ("install", ["--plan", str(self.plan)]),
            ("verify", ["--receipt", str(self.receipt)]),
            ("rollback", ["--receipt", str(self.receipt)]),
        ):
            out = {
                "plan": self.plan,
                "install": self.receipt,
                "verify": self.work / "verify",
                "rollback": self.rollback,
            }[command]
            with contextlib.redirect_stdout(io.StringIO()) as output:
                code = install.main([command, *args, "--out", str(out)])
            self.assertEqual(code, 0, output.getvalue())
            self.assertIn(json.loads(output.getvalue())["status"], deploy.SUCCESS)
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(
                install.main(["install", "--plan", str(self.plan), "--out", str(self.receipt)]), 2
            )


@unittest.skipUnless(
    os.name == "nt" and os.environ.get("AC1_TEST_WINDOWS_GUARD") == "1",
    "Explicit unsandboxed Windows guard integration test",
)
class WindowsGuardTests(unittest.TestCase):
    def test_real_guard_holds_and_releases_only_temporary_host_files(self):
        import tempfile

        with tempfile.TemporaryDirectory(prefix="ac1-guard-test-") as temporary:
            game = Path(temporary)
            host = game / candidate.HOSTS["dx10"]["exe"]
            host.write_bytes(b"non-executable test fixture")
            before = candidate.fingerprint(host), host.stat().st_mtime_ns
            with deploy.ClosedGame(game) as guard:
                self.assertEqual(guard.evidence["files"][host.name], before[0])
                with self.assertRaises(PermissionError):
                    host.open("rb")
            self.assertEqual((candidate.fingerprint(host), host.stat().st_mtime_ns), before)

    def test_real_guard_rejects_a_host_open_in_another_stream(self):
        import tempfile

        with tempfile.TemporaryDirectory(prefix="ac1-guard-test-") as temporary:
            game = Path(temporary)
            host = game / candidate.HOSTS["dx10"]["exe"]
            host.write_bytes(b"non-executable test fixture")
            with host.open("rb"):
                with self.assertRaises(RuntimeError):
                    with deploy.ClosedGame(game):
                        self.fail("Guard must reject a shared host file")
            self.assertEqual(host.read_bytes(), b"non-executable test fixture")

    def test_full_temporary_deployment_with_production_windows_guard(self):
        fixture = fixtures.InstallationTests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        for name in install.PROJECT_FILES:
            (fixture.scripts / name).write_bytes(("previous " + name).encode())
        before = fixture.snapshot(fixture.game)
        plan, receipt, rollback = (fixture.work / name for name in ("plan", "receipt", "rollback"))
        deploy.plan_installation(fixture.candidate, fixture.game, plan)
        self.assertEqual(deploy.install_plan(plan, receipt)["status"], "DEPLOYED")
        self.assertEqual(
            deploy.verify_deployment(receipt, fixture.work / "verify")["status"], "VERIFIED"
        )
        self.assertEqual(deploy.rollback_deployment(receipt, rollback)["status"], "ROLLED_BACK")
        self.assertEqual(before, fixture.snapshot(fixture.game))


if __name__ == "__main__":
    unittest.main()
