"""Delivery integrity and failures of the single supported build mode."""

import argparse
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

NATIVE_BUILD_LOG = """==== 通过 205 / 失败 0 ====
==== 通过 114 / 失败 0 ====
==== 通过 77 / 失败 0 ====
==== 通过 25 / 失败 0 ====
Startup replay: checks=8 failures=0
Startup replay: checks=5 failures=0
Startup replay: checks=8 failures=0
Startup replay: checks=5 failures=0
Startup replay: checks=6 failures=0
Startup replay: checks=6 failures=0
Startup replay: checks=8 failures=0
Startup replay: checks=8 failures=0
"""


class CandidateTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ac1-candidate-test-")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)

    def fixture(self):
        return fixtures.language_candidate(self.work / "candidate")

    def seal(self, directory, manifest=None):
        fixtures.seal_candidate(directory, manifest)

    def test_verify_without_build_dependencies(self):
        directory = self.fixture()
        result = subprocess.run(
            [sys.executable, str(TOOLS / "runtime_candidate.py"), "verify", str(directory)],
            cwd=self.work,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("NOT_TESTED_BY_TOOL", result.stdout)

    def test_candidate_sources_include_changes_to_shared_toolchain(self):
        root = self.work / "source tree"
        for name in (
            "build.ps1",
            "LICENSE",
            "third_party/minhook/LICENSE.txt",
            "tools/modules/native-toolchain.ps1",
        ):
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(name.encode())
        with patch.object(candidate, "ROOT", root):
            before = candidate.source_fingerprints()
            (root / "tools/modules/native-toolchain.ps1").write_bytes(b"changed SDK configuration")
            after = candidate.source_fingerprints()
        self.assertIn("tools/modules/native-toolchain.ps1", before)
        self.assertNotEqual(
            before["tools/modules/native-toolchain.ps1"],
            after["tools/modules/native-toolchain.ps1"],
        )

    def test_retired_candidate_versions_are_rejected_before_interpreting_their_fields(self):
        directory = self.fixture()
        for version in (2, 3, True, 5):
            manifest = candidate.read_json(directory / "manifest.json")
            manifest["format_version"] = version
            self.seal(directory, manifest)
            with (
                self.subTest(version=version),
                self.assertRaisesRegex(ValueError, "Unsupported candidate"),
            ):
                candidate.verify(directory)

    def test_current_candidate_rejects_retired_mode_and_restore_evidence(self):
        directory = self.fixture()
        manifest = candidate.read_json(directory / "manifest.json")
        manifest["mode"] = "AC1_RUNTIME_PACKAGE=1 / TEXT_REPLACE=1"
        self.seal(directory, manifest)
        with self.assertRaisesRegex(ValueError, "validation state"):
            candidate.verify(directory)
        manifest["mode"] = candidate.MODE
        manifest["default_restore"] = dict(status="PASS")
        self.seal(directory, manifest)
        with self.assertRaisesRegex(ValueError, "validation state"):
            candidate.verify(directory)
        manifest.pop("default_restore")
        self.seal(directory, manifest)
        (directory / "evidence/default-restore.log").write_text("retired build step")
        with self.assertRaisesRegex(ValueError, "inventory mismatch"):
            candidate.verify(directory)

    def test_changed_runtime_bytes_are_rejected(self):
        directory = self.fixture()
        path = directory / "install/scripts/AC1_Language.bin"
        path.write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "digest mismatch"):
            candidate.verify(directory)

    def test_linked_manifest_is_rejected_before_reading_its_target(self):
        directory = self.fixture()
        external = self.work / "outside-manifest.json"
        external.write_text("not JSON; the verifier must not read this target")
        manifest = directory / "manifest.json"
        manifest.unlink()
        try:
            manifest.symlink_to(external)
        except OSError as error:
            self.skipTest(f"File symlink creation is unavailable: {error}")
        with self.assertRaisesRegex(ValueError, "Linked candidate entry"):
            candidate.verify(directory)

    def test_extra_game_file_and_missing_file_are_rejected(self):
        directory = self.fixture()
        extra = directory / "install/DataPC.forge"
        extra.write_bytes(b"must not enter the delivery")
        with self.assertRaisesRegex(ValueError, "inventory mismatch"):
            candidate.verify(directory)
        extra.unlink()
        (directory / "install/LICENSE").unlink()
        with self.assertRaisesRegex(ValueError, "inventory mismatch"):
            candidate.verify(directory)

    def test_manifest_cannot_add_path_outside_candidate(self):
        directory = self.fixture()
        path = directory / "manifest.json"
        manifest = json.loads(path.read_text(encoding="utf-8"))
        manifest["files"]["../unrelated"] = dict(bytes=0, sha256="0" * 64)
        path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Manifest inventory"):
            candidate.verify(directory)

    def test_native_acceptance_cannot_be_inherited_by_new_build(self):
        directory = self.fixture()
        path = directory / "manifest.json"
        manifest = json.loads(path.read_text(encoding="utf-8"))
        manifest["native"] = "PASS"
        self.seal(directory, manifest)
        with self.assertRaisesRegex(ValueError, "validation state"):
            candidate.verify(directory)

    def test_dx10_requires_the_resource_adapter_marker(self):
        directory = self.fixture()
        path = directory / f"install/scripts/{candidate.ASI}"
        binary = path.read_bytes()
        path.write_bytes(binary.replace(candidate.HOST_ADAPTER_MARKER, b""))
        self.seal(directory)
        with self.assertRaisesRegex(ValueError, "Dx10 resource adapter"):
            candidate.verify(directory)
        path.write_bytes(binary)
        self.seal(directory)
        self.assertEqual(candidate.verify(directory)["hosts"], list(candidate.HOSTS.values()))

    def test_wrong_host_crc_is_rejected(self):
        directory = self.fixture()
        manifest = candidate.read_json(directory / "manifest.json")
        manifest["hosts"] = [dict(exe="AssassinsCreed_Dx10.exe", crc32="E8936C99")]
        self.seal(directory, manifest)
        with self.assertRaisesRegex(ValueError, "host or validation"):
            candidate.verify(directory)

    def test_resealed_old_package_receipt_is_rejected(self):
        directory = self.fixture()
        (directory / "install/scripts/AC1_Language.bin").write_bytes(b"new package")
        self.seal(directory)
        with self.assertRaisesRegex(ValueError, "receipt"):
            candidate.verify(directory)

    def test_wrong_asi_mode_is_rejected_even_when_resealed(self):
        directory = self.fixture()
        (directory / f"install/scripts/{candidate.ASI}").write_bytes(b"default forge mode")
        self.seal(directory)
        with self.assertRaisesRegex(ValueError, "runtime-package entry"):
            candidate.verify(directory)

    def test_checksum_manifest_changes_are_rejected(self):
        directory = self.fixture()
        (directory / "SHA256SUMS").write_text("", encoding="ascii")
        with self.assertRaisesRegex(ValueError, "SHA256SUMS"):
            candidate.verify(directory)

    def test_old_background_startup_is_rejected_even_when_resealed(self):
        directory = self.fixture()
        (directory / f"install/scripts/{candidate.ASI}").write_bytes(candidate.MARKER)
        self.seal(directory)
        with self.assertRaisesRegex(ValueError, "synchronous loader startup"):
            candidate.verify(directory)

    def test_existing_destination_is_untouched(self):
        directory = self.fixture()
        before = (directory / "manifest.json").read_bytes()
        with self.assertRaises(FileExistsError):
            candidate.build(argparse.Namespace(out=directory))
        self.assertEqual((directory / "manifest.json").read_bytes(), before)

    def test_native_failure_does_not_build_a_second_mode(self):
        calls = []

        def fail(command, log):
            calls.append((command, log.name))
            if log.name == "runtime-build.log":
                raise ValueError("runtime build failed")

        with patch.object(candidate, "run", side_effect=fail):
            with self.assertRaisesRegex(ValueError, "runtime build failed"):
                candidate.native_build("pwsh", self.work, self.work)
        self.assertEqual([name for _, name in calls], ["runtime-build.log"])
        self.assertIn("-RunTests", calls[0][0])
        self.assertNotIn("-Defines", calls[0][0])
        self.assertNotIn("-AsiName", calls[0][0])

    def test_native_build_accepts_all_eight_startup_replays(self):
        (self.work / "runtime-build.log").write_text(NATIVE_BUILD_LOG, encoding="utf-8")
        pe = dict(machine="x86", imports=["KERNEL32.DLL"], initialize_entry="InitializeASI")
        with (
            patch.object(candidate, "run"),
            patch.object(candidate, "inspect_asi", return_value=pe),
        ):
            asi, tests, actual_pe = candidate.native_build("pwsh", self.work, self.work)
        self.assertEqual(asi, self.work / "runtime" / candidate.ASI)
        self.assertEqual(tests, dict(core=205, dict=114, text=77, glyph=25, startup=54))
        self.assertEqual(actual_pe, pe)

    def test_native_build_rejects_incomplete_or_failed_results(self):
        lines = NATIVE_BUILD_LOG.splitlines(keepends=True)
        invalid = {
            "historical four startup replays": "".join(lines[:8]),
            "missing startup replay": "".join(lines[:-1]),
            "extra startup replay": NATIVE_BUILD_LOG + lines[-1],
            "failed startup replay": NATIVE_BUILD_LOG.replace("failures=0", "failures=1", 1),
            "missing module": "".join(lines[1:]),
            "failed module": NATIVE_BUILD_LOG.replace("失败 0", "失败 1", 1),
        }
        for name, log in invalid.items():
            with self.subTest(case=name):
                (self.work / "runtime-build.log").write_text(log, encoding="utf-8")
                with (
                    patch.object(candidate, "run"),
                    patch.object(candidate, "inspect_asi") as inspect,
                ):
                    with self.assertRaisesRegex(ValueError, "Missing successful"):
                        candidate.native_build("pwsh", self.work, self.work)
                    inspect.assert_not_called()

    def test_failed_workflow_leaves_no_candidate_and_releases_lock(self):
        root = self.work / "source"
        root.mkdir()
        source = root / "input"
        source.write_bytes(b"fixture")
        args = argparse.Namespace(
            out=self.work / "delivery",
            version="0.1.0-rc.1",
            dictionary=source,
            font=source,
            profile=source,
            input_root=root,
        )
        with patch.object(candidate, "ROOT", root), patch.object(candidate, "VC", root):
            compiler = root / "bin/Hostx64/x86/dumpbin.exe"
            compiler.parent.mkdir(parents=True)
            compiler.touch()
            with (
                patch.object(candidate, "native_sources", return_value={}),
                patch.object(candidate.shutil, "which", return_value="pwsh"),
                patch.object(candidate, "run", side_effect=ValueError("oracle failed")),
            ):
                with self.assertRaisesRegex(ValueError, "oracle failed"):
                    candidate.build_runtime(args)
        self.assertFalse(args.out.exists())
        self.assertFalse((root / "build/runtime-candidate.lock").exists())
        self.assertEqual(list(self.work.glob(".runtime-native-*")), [])


if __name__ == "__main__":
    unittest.main()
