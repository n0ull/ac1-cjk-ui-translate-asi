"""Language switching and multi-host safeguards through public tools."""

import contextlib
import json
import os
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import runtime_candidate as candidate
import runtime_install as deploy
import test_runtime_install as fixtures
from runtime_fixtures import language_candidate


class LanguageDeploymentTests(unittest.TestCase):
    make_candidate = fixtures.InstallationTests.make_candidate
    seal_candidate = fixtures.InstallationTests.seal_candidate
    snapshot = fixtures.InstallationTests.snapshot

    def setUp(self):
        fixtures.InstallationTests.setUp(self)
        self.old = self.candidate
        self.target = language_candidate(self.work / "language")
        self.plan = self.work / "plan"
        self.receipt = self.work / "receipt"

        game_root = self.game

        class Guard:
            def __init__(self):
                self.evidence = dict(
                    files={
                        host["exe"]: candidate.fingerprint(game_root / host["exe"])
                        for host in candidate.HOSTS.values()
                        if (game_root / host["exe"]).exists()
                    }
                )

            def check(self):
                pass

        gate = patch.object(deploy, "ClosedGame", lambda game: contextlib.nullcontext(Guard()))
        gate.start()
        self.addCleanup(gate.stop)

    def prepare(self):
        return deploy.plan_installation(self.target, self.game, self.plan)

    def test_two_active_bins_and_partial_installations_are_refused(self):
        path = self.scripts / "AC1_CJK_Runtime.bin"
        path.write_bytes(b"retired package")
        with self.assertRaises(ValueError):
            deploy.plan_installation(self.target, self.game, self.plan)
        path.unlink()
        (self.scripts / candidate.ASI).unlink()
        with self.assertRaises(ValueError):
            self.prepare()
        self.assertFalse(self.plan.exists())

    def test_interrupted_language_switch_can_restore_original_installation(self):
        self.target = language_candidate(self.work / "japanese", "ja")
        before = self.snapshot(self.game)
        self.prepare()
        original = deploy.replace_file

        def interrupt(source, target):
            original(source, target)
            if Path(source).suffix == ".stage":
                raise KeyboardInterrupt("power loss after language package commit")

        with patch.object(deploy, "replace_file", interrupt):
            with self.assertRaises(KeyboardInterrupt):
                deploy.install_plan(self.plan, self.receipt)
        self.assertFalse((self.receipt / "result.json").exists())
        deploy.rollback_deployment(self.receipt, self.work / "rollback")
        self.assertEqual(before, self.snapshot(self.game))

    def test_language_switch_uses_same_asi_and_restores_prior_language(self):
        self.prepare()
        deploy.install_plan(self.plan, self.receipt)
        before = self.snapshot(self.game)
        other = language_candidate(self.work / "japanese", "ja")
        deploy.plan_installation(other, self.game, self.work / "switch-plan")
        deploy.install_plan(self.work / "switch-plan", self.work / "switch-receipt")
        self.assertEqual(
            (self.scripts / candidate.PACKAGE).read_bytes(),
            (other / "install/scripts" / candidate.PACKAGE).read_bytes(),
        )
        self.assertEqual(
            (self.scripts / candidate.ASI).read_bytes(),
            (self.target / "install/scripts" / candidate.ASI).read_bytes(),
        )
        deploy.rollback_deployment(self.work / "switch-receipt", self.work / "switch-rollback")
        self.assertEqual(before, self.snapshot(self.game))

    def test_single_present_supported_host_is_allowed_but_wrong_existing_host_is_not(self):
        path = self.game / candidate.HOSTS["dx9"]["exe"]
        path.unlink()
        self.prepare()
        self.assertEqual(deploy.install_plan(self.plan, self.receipt)["status"], "DEPLOYED")
        path.write_bytes(b"wrong executable")
        with self.assertRaisesRegex(ValueError, "HOST_CRC_MISMATCH"):
            deploy.plan_installation(self.target, self.game, self.work / "bad-host")

    def test_retired_receipt_versions_are_rejected(self):
        self.prepare()
        deploy.install_plan(self.plan, self.receipt)
        transaction = candidate.read_json(self.receipt / "transaction.json")
        self.assertEqual(transaction["format_version"], 3)
        for version in (1, 2, True, 4):
            transaction["format_version"] = version
            path = self.receipt / "transaction.json"
            path.write_text(json.dumps(transaction), encoding="utf-8")
            path.with_name(path.name + ".sha256").write_text(
                candidate.fingerprint(path)["sha256"], encoding="ascii"
            )
            with (
                self.subTest(version=version),
                self.assertRaisesRegex(ValueError, "Unsupported record"),
            ):
                deploy.deployment_record(self.receipt)

    def test_user_change_to_installed_file_blocks_recovery(self):
        self.prepare()
        deploy.install_plan(self.plan, self.receipt)
        (self.scripts / candidate.PACKAGE).write_bytes(b"user file after deployment")
        before = self.snapshot(self.game)
        with self.assertRaises(ValueError):
            deploy.rollback_deployment(self.receipt, self.work / "refused")
        self.assertEqual(before, self.snapshot(self.game))

    def test_fresh_language_install_and_rollback_restore_absence(self):
        for name in (candidate.ASI, candidate.PACKAGE):
            (self.scripts / name).unlink()
        before = self.snapshot(self.game)
        deploy.plan_installation(self.target, self.game, self.plan)
        deploy.install_plan(self.plan, self.receipt)
        deploy.rollback_deployment(self.receipt, self.work / "rollback-fresh")
        self.assertEqual(before, self.snapshot(self.game))

    def test_interrupted_rollback_resumes_from_event_without_source_candidates(self):
        self.target = language_candidate(self.work / "japanese", "ja")
        before = self.snapshot(self.game)
        self.prepare()
        deploy.install_plan(self.plan, self.receipt)
        original = deploy.replace_file

        def interrupt(source, target):
            original(source, target)
            if Path(source).suffix == ".restore" and target.name == candidate.PACKAGE:
                raise KeyboardInterrupt("interrupted rollback after restoring prior language")

        event = self.work / "rollback-event"
        with patch.object(deploy, "replace_file", interrupt):
            with self.assertRaises(KeyboardInterrupt):
                deploy.rollback_deployment(self.receipt, event)
        self.assertTrue((self.scripts / candidate.PACKAGE).exists())
        self.old.rename(self.work / "old-moved")
        self.target.rename(self.work / "new-moved")
        deploy.rollback_deployment(event, self.work / "rollback-retry")
        self.assertEqual(before, self.snapshot(self.game))

    def test_host_appearing_between_inspection_and_lock_refuses_writes(self):
        (self.game / candidate.HOSTS["dx9"]["exe"]).unlink()
        self.prepare()
        before = self.snapshot(self.game)

        class Guard:
            evidence = dict(
                files={
                    host["exe"]: dict(bytes=0, sha256="0" * 64) for host in candidate.HOSTS.values()
                }
            )

            def check(self):
                pass

        with patch.object(deploy, "ClosedGame", lambda game: contextlib.nullcontext(Guard())):
            with self.assertRaisesRegex(ValueError, "Host changed"):
                deploy.install_plan(self.plan, self.receipt)
        self.assertFalse(self.receipt.exists())
        self.assertEqual(before, self.snapshot(self.game))


@unittest.skipUnless(
    os.name == "nt" and os.environ.get("AC1_TEST_WINDOWS_GUARD") == "1",
    "Explicit trusted Windows guard integration test",
)
class LanguageWindowsGuardTests(unittest.TestCase):
    def test_language_deployment_uses_production_guard_and_restores_the_pair(self):
        production_guard = deploy.ClosedGame
        fixture = LanguageDeploymentTests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        before = fixture.snapshot(fixture.game)
        with patch.object(deploy, "ClosedGame", production_guard):
            fixture.prepare()
            self.assertEqual(
                deploy.install_plan(fixture.plan, fixture.receipt)["status"], "DEPLOYED"
            )
            self.assertEqual(
                deploy.verify_deployment(fixture.receipt, fixture.work / "verify")["status"],
                "VERIFIED",
            )
            self.assertEqual(
                deploy.rollback_deployment(fixture.receipt, fixture.work / "rollback")["status"],
                "ROLLED_BACK",
            )
        self.assertEqual(before, fixture.snapshot(fixture.game))


if __name__ == "__main__":
    unittest.main()
