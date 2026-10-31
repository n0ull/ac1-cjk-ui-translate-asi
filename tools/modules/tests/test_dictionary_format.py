"""Exercise the real dictionary build gate with valid and invalid DLOG calls."""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
COMPILER = Path(
    "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/"
    "14.44.35207/bin/Hostx64/x86/cl.exe"
)


@unittest.skipUnless(
    os.name == "nt" and COMPILER.is_file() and shutil.which("pwsh"),
    "Requires the configured Windows compiler and PowerShell",
)
class DictionaryFormatTests(unittest.TestCase):
    def test_production_build_enforces_format_arguments(self):
        with tempfile.TemporaryDirectory(prefix="ac1-dictionary-format-") as temporary:
            work = Path(temporary)
            for name in (
                "modules/dict/build.ps1",
                "tools/modules/native-toolchain.ps1",
                "modules/dict/src/dict.cpp",
                "modules/dict/include/ac1/dict/dict.h",
                "modules/core/include/ac1/core/file.h",
                "modules/core/include/ac1/core/str.h",
            ):
                target = work / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(ROOT / name, target)
            source = work / "modules/dict/src/dict.cpp"
            original = source.read_bytes()
            for name, call, diagnostic in (
                ("valid", 'DLOG("literal %%"); DLOG("%*.*s %d", 4, 2, "text", 7);', None),
                ("missing_value", 'DLOG("%d %d", 1);', b"C4473"),
                ("extra_value", 'DLOG("%d", 1, 2);', b"C4474"),
                ("wrong_type", 'DLOG("%s", 1);', b"C4477"),
                ("missing_width_value", 'DLOG("%*s", 4);', b"C4473"),
                ("wrong_precision", 'DLOG("%.*s", "2", "text");', b"C4477"),
            ):
                with self.subTest(case=name):
                    probe = "\nnamespace ac1 { namespace dict { void format_contract_probe() { %s } }}\n"
                    source.write_bytes(original + (probe % call).encode("ascii"))
                    result = subprocess.run(
                        ["pwsh", "-NoProfile", "-File", str(work / "modules/dict/build.ps1")],
                        capture_output=True,
                        timeout=60,
                        creationflags=subprocess.CREATE_NO_WINDOW,
                    )
                    output = result.stdout + result.stderr
                    self.assertEqual(result.returncode == 0, diagnostic is None, output)
                    if diagnostic is not None:
                        self.assertIn(diagnostic, output)


if __name__ == "__main__":
    unittest.main(verbosity=2)
