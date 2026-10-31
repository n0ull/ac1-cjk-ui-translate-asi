"""Compile isolated copies through the actual oracle recipe and provenance reader."""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import _runtime_inputs as inputs  # noqa: E402

COMPILER = Path(
    "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/"
    "14.44.35207/bin/Hostx64/x86/cl.exe"
)


@unittest.skipUnless(
    os.name == "nt" and COMPILER.is_file() and shutil.which("pwsh"),
    "Requires the configured Windows compiler and PowerShell",
)
class OracleBuildTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ac1-oracle-build-")
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.root = self.work / "source tree"
        self.output = self.work / "custom output"
        for name in inputs.ORACLE_SOURCES:
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(inputs.ROOT / name, target)
        self.source = self.root / "modules/dict/src/dict.cpp"
        self.executable = self.output / "dictionary_oracle.exe"
        self.receipt = self.executable.with_suffix(".json")

    def build(self, launcher=None, changed_file="modules/dict/src/dict.cpp"):
        script = launcher or self.root / "tools/modules/build_runtime_oracle.ps1"
        command = ["pwsh", "-NoProfile", "-File", str(script)]
        if launcher:
            command += ["-Root", str(self.root), "-ChangedFile", changed_file]
        # Resolve this relative output from the caller's cwd, not the source tree.
        command += ["-OutDir", "custom output"]
        return subprocess.run(
            command,
            cwd=self.work,
            capture_output=True,
            timeout=60,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )

    def test_valid_build_from_another_directory_has_verifiable_receipt(self):
        result = self.build()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        with patch.object(inputs, "ROOT", self.root):
            self.assertEqual(inputs.oracle_provenance(self.executable)["target"], "x86")

    def test_source_or_toolchain_change_during_build_is_not_signed_as_input(self):
        launcher = self.work / "change-after-compile.ps1"
        launcher.write_text(
            "param([string]$Root, [string]$OutDir, [string]$ChangedFile)\n"
            "$ErrorActionPreference = 'Stop'\n"
            f"$compiler = '{COMPILER.as_posix()}'\n"
            "$source = Join-Path $Root $ChangedFile\n"
            "function cl.exe {\n"
            "    & $compiler @args\n"
            "    if ($LASTEXITCODE -eq 0) {\n"
            "        Add-Content -LiteralPath $source -Value '#error CHANGED_AFTER_COMPILE'\n"
            "    }\n"
            "}\n"
            "& (Join-Path $Root 'tools/modules/build_runtime_oracle.ps1') -OutDir $OutDir\n",
            encoding="utf-8",
        )
        for name in ("modules/dict/src/dict.cpp", "tools/modules/native-toolchain.ps1"):
            with self.subTest(changed=name):
                path = self.root / name
                original = path.read_bytes()
                result = self.build(launcher, changed_file=name)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(
                    ("Oracle source changed during build: " + name).encode(),
                    result.stdout + result.stderr,
                )
                self.assertFalse(self.receipt.exists())
                path.write_bytes(original)

    def test_failed_rebuild_invalidates_the_previous_receipt(self):
        result = self.build()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(self.receipt.is_file())
        with self.source.open("ab") as stream:
            stream.write(b"\n#error EXPECTED_COMPILER_FAILURE\n")
        result = self.build()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"EXPECTED_COMPILER_FAILURE", result.stdout + result.stderr)
        self.assertFalse(self.receipt.exists())

    def test_missing_input_invalidates_receipt_before_compilation(self):
        for name in ("modules/dict/include/ac1/dict/dict.h", "tools/modules/native-toolchain.ps1"):
            with self.subTest(missing=name):
                result = self.build()
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                path = self.root / name
                original = path.read_bytes()
                path.unlink()
                result = self.build()
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.receipt.exists())
                path.write_bytes(original)


if __name__ == "__main__":
    unittest.main(verbosity=2)
