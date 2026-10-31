"""Freeze one runtime, build language candidates and verify delivery integrity."""

import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VC = Path(
    "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207"
)
ASI = "ac1_runtime_package.asi"
PACKAGE = "AC1_Language.bin"
FORMAT_VERSION = 4
MODE = "runtime-package / TEXT_REPLACE=1"
MARKER = b"[RTP] single package active dictionary_keys=%d text_replace=%d"
STARTUP_MARKER = b"[RTP] synchronous InitializeASI startup thread=%lu"
HOST_ADAPTER_MARKER = b"[RTP] Dx9/Dx10 resource adapters; selected=%s"
LANGUAGE_MARKER = b"[RTP] language=%s embedded_digest=%s package_version=2 dictionary_rules=2"
HOSTS = {
    "dx9": dict(exe="AssassinsCreed_Dx9.exe", crc32="E8936C99"),
    "dx10": dict(exe="AssassinsCreed_Dx10.exe", crc32="3AF8F9D0"),
}
INSTALL_FILES = {
    f"install/scripts/{ASI}",
    f"install/scripts/{PACKAGE}",
    "install/README.md",
    "install/LICENSE",
    "install/THIRD_PARTY_NOTICES.md",
}
PACKAGE_REPORTS = {
    "build-report.json",
    "build-report.md",
    "verify-report.json",
    "profile.json",
    "dictionary-audit.json",
    "dictionary-effective.json",
    *(f"coverage-{name}.json" for name in ("bold", "techno", "textpc", "title")),
}
RUNTIME_CONTENT_FILES = {
    ASI,
    "dictionary_oracle.exe",
    "dictionary_oracle.json",
    "runtime-build.log",
    "oracle.log",
    "pe.txt",
}
RUNTIME_FILES = RUNTIME_CONTENT_FILES | {"runtime-build.json"}
CONTENT_FILES = (
    INSTALL_FILES
    | {"evidence/package.log"}
    | {f"evidence/package/{name}" for name in PACKAGE_REPORTS | {"baseline.json"}}
    | {f"evidence/runtime/{name}" for name in RUNTIME_FILES}
)


def fingerprint(path):
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return dict(bytes=path.stat().st_size, sha256=digest)


def write_json(path, value):
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2)
        stream.write("\n")


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def directory_files(root):
    # Junctions can escape a candidate just as symlinks can. Inspect directories
    # before os.walk descends, including on Windows where is_symlink is false.
    files = set()
    for folder, directories, names in os.walk(root, followlinks=False):
        for name in directories + names:
            path = Path(folder) / name
            info = path.lstat()
            reparse = getattr(info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT
            if path.is_symlink() or reparse:
                raise ValueError(f"Linked candidate entry: {path}")
            if not path.is_dir():
                files.add(path.relative_to(root).as_posix())
    return files


def verify(candidate):
    if candidate.is_symlink() or not candidate.is_dir():
        raise ValueError("Candidate must be a real directory")
    actual = directory_files(candidate)
    manifest = read_json(candidate / "manifest.json")
    if not isinstance(manifest, dict):
        raise ValueError("Unsupported candidate manifest")
    version = manifest.get("format_version")
    if type(version) is not int or version != FORMAT_VERSION:
        raise ValueError("Unsupported candidate manifest; expected format 4")
    content_files = CONTENT_FILES
    expected = content_files | {"manifest.json", "SHA256SUMS"}
    if actual != expected:
        raise ValueError(
            f"Candidate inventory mismatch: missing={expected - actual}; extra={actual - expected}"
        )
    hosts = manifest.get("hosts")
    supported_hosts = (
        isinstance(hosts, list)
        and bool(hosts)
        and all(host in HOSTS.values() for host in hosts)
        and len({host["exe"] for host in hosts}) == len(hosts)
    )
    checks = (
        manifest.get("mode") == MODE,
        manifest.get("status") == "CANDIDATE_OFFLINE_PASS",
        manifest.get("native") == "NOT_TESTED_BY_TOOL",
        "default_restore" not in manifest,
        supported_hosts,
    )
    if not all(checks):
        raise ValueError("Candidate mode, host or validation state mismatch")
    declared = manifest.get("files")
    if not isinstance(declared, dict) or set(declared) != content_files:
        raise ValueError("Manifest inventory mismatch")
    for name in sorted(content_files):
        if declared[name] != fingerprint(candidate / name):
            raise ValueError(f"Candidate digest mismatch: {name}")
    checksum_names = sorted(content_files | {"manifest.json"})
    checksums = "".join(
        f"{fingerprint(candidate / name)['sha256']}  {name}\n" for name in checksum_names
    )
    if (candidate / "SHA256SUMS").read_text(encoding="ascii") != checksums:
        raise ValueError("SHA256SUMS mismatch")
    build = read_json(candidate / "evidence/package/build-report.json")
    reader = read_json(candidate / "evidence/package/verify-report.json")
    package = declared[f"install/scripts/{PACKAGE}"]
    for report in (build, reader):
        valid = (
            report.get("status") == "OFFLINE_PASS"
            and report.get("package_sha256") == package["sha256"]
            and report.get("package_bytes") == package["bytes"]
        )
        if not valid:
            raise ValueError("Package receipt does not describe delivered package")
    if MARKER not in (candidate / f"install/scripts/{ASI}").read_bytes():
        raise ValueError("ASI does not contain runtime-package entry")
    if STARTUP_MARKER not in (candidate / f"install/scripts/{ASI}").read_bytes():
        raise ValueError("ASI does not contain synchronous loader startup")
    dx10 = HOSTS["dx10"] in manifest["hosts"]
    if dx10 and HOST_ADAPTER_MARKER not in (candidate / f"install/scripts/{ASI}").read_bytes():
        raise ValueError("ASI does not contain the Dx10 resource adapter")
    runtime = verify_runtime(candidate / "evidence/runtime")
    if declared[f"install/scripts/{ASI}"] != runtime["files"][ASI]:
        raise ValueError("Delivered ASI differs from the frozen runtime")
    if LANGUAGE_MARKER not in (candidate / f"install/scripts/{ASI}").read_bytes():
        raise ValueError("ASI does not support language packages v2")
    label = manifest.get("language_id")
    if (
        not isinstance(label, str)
        or not re.fullmatch(r"[a-z][a-z0-9]*(?:-[a-z0-9]+)*", label)
        or len(label) > 31
    ):
        raise ValueError("Invalid candidate language")
    raw = (candidate / f"install/scripts/{PACKAGE}").read_bytes()
    valid_header = (
        raw[:8] == b"AC1LANG\0"
        and raw[8:16] == struct.pack("<II", 2, 128)
        and raw[60:64] == struct.pack("<I", 2)
        and raw[96:128] == label.encode("ascii").ljust(32, b"\0")
    )
    if not valid_header:
        raise ValueError("Delivered package language mismatch")
    if any(
        report.get("language_id") != label or report.get("package_version") != 2
        for report in (build, reader)
    ):
        raise ValueError("Language package receipt mismatch")
    if build.get("oracle_sha256") != runtime["files"]["dictionary_oracle.exe"]["sha256"]:
        raise ValueError("Package was not checked by the frozen runtime oracle")
    baseline = read_json(candidate / "evidence/package/baseline.json")
    language_config = read_json(candidate / "evidence/package/profile.json")
    same_inputs = (
        build.get("baseline_sha256") == declared["evidence/package/baseline.json"]["sha256"]
        and build.get("profile_sha256") == declared["evidence/package/profile.json"]["sha256"]
        and language_config.get("language_id") == label
        and build.get("baseline_id") == baseline.get("id") == language_config.get("baseline")
    )
    same_runtime = (
        manifest.get("runtime_version") == runtime["version"]
        and manifest.get("native_tests") == runtime["native_tests"]
        and manifest.get("pe") == runtime["pe"]
    )
    if not same_inputs or not same_runtime:
        raise ValueError("Frozen language/runtime inputs do not match candidate receipts")
    baseline_hosts = baseline.get("hosts", {})
    supported_hosts = [
        host for name, host in baseline_hosts.items() if runtime["hosts"].get(name) == host
    ]
    if len(supported_hosts) != len(baseline_hosts) or manifest["hosts"] != supported_hosts:
        raise ValueError("Candidate hosts differ from its frozen resource baseline")
    return manifest


def source_fingerprints():
    files = {ROOT / "build.ps1", ROOT / "LICENSE"}
    for folder in ("app", "modules", "third_party/minhook", "tools"):
        for path in (ROOT / folder).rglob("*"):
            relative = path.relative_to(ROOT)
            excluded = {"out", "input", ".git", "__pycache__"} & set(relative.parts)
            source = path.suffix in {".cpp", ".c", ".h", ".ps1", ".py", ".json", ".in", ".md"}
            if path.is_file() and source and not excluded:
                files.add(path)
    files.add(ROOT / "third_party/minhook/LICENSE.txt")
    return {path.relative_to(ROOT).as_posix(): fingerprint(path) for path in sorted(files)}


def run(command, log):
    print(f"==> {log.stem}", flush=True)
    with log.open("xb") as stream:
        result = subprocess.run(
            command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT, check=False
        )
    if result.returncode:
        tail = log.read_text(encoding="utf-8-sig", errors="replace")[-5000:]
        raise ValueError(f"{log.stem} failed ({result.returncode}):\n{tail}")


def module_results(log):
    text = log.read_text(encoding="utf-8-sig", errors="replace")
    results = re.findall(r"==== 通过 (\d+) / 失败 (\d+) ====", text)
    if len(results) != 4 or any(int(failed) for _, failed in results):
        raise ValueError("Missing successful four-module test results")
    return dict(zip(("core", "dict", "text", "glyph"), (int(n) for n, _ in results)))


def inspect_asi(asi, log):
    run(
        [str(VC / "bin/Hostx64/x86/dumpbin.exe"), "/headers", "/imports", "/exports", str(asi)], log
    )
    text = log.read_text(encoding="utf-8-sig", errors="replace")
    imports = sorted(set(re.findall(r"^\s+([\w.-]+\.dll)\s*$", text, re.MULTILINE | re.IGNORECASE)))
    if "14C machine (x86)" not in text or "File Type: DLL" not in text:
        raise ValueError("Expected an x86 DLL from the native linker")
    if [name.upper() for name in imports] != ["KERNEL32.DLL"]:
        raise ValueError(f"Unexpected native imports: {imports}")
    if MARKER not in asi.read_bytes():
        raise ValueError("Native build did not select runtime-package mode")
    if not re.search(
        r"^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+InitializeASI(?:\s|$)", text, re.MULTILINE
    ):
        raise ValueError("Native build does not export undecorated InitializeASI")
    return dict(machine="x86", imports=["KERNEL32.DLL"], initialize_entry="InitializeASI")


def native_build(pwsh, work, evidence):
    runtime = work / "runtime"
    run(
        [
            pwsh,
            "-NoProfile",
            "-File",
            str(ROOT / "build.ps1"),
            "-RunTests",
            "-OutDir",
            str(runtime),
        ],
        evidence / "runtime-build.log",
    )
    tests = module_results(evidence / "runtime-build.log")
    startup = re.findall(
        r"Startup replay: checks=(\d+) failures=(\d+)",
        (evidence / "runtime-build.log").read_text(encoding="utf-8-sig"),
    )
    if len(startup) != 8 or any(int(failed) for _, failed in startup):
        raise ValueError("Missing successful synchronous startup regression results")
    tests["startup"] = sum(int(count) for count, _ in startup)
    pe = inspect_asi(runtime / ASI, evidence / "pe.txt")
    return runtime / ASI, tests, pe


def native_sources():
    files = {
        ROOT / "build.ps1",
        ROOT / "tools/modules/native-toolchain.ps1",
        ROOT / "tools/modules/build_runtime_oracle.ps1",
        ROOT / "tools/modules/runtime/dictionary_oracle.cpp",
    }
    for folder in ("app", "modules", "third_party/minhook"):
        for path in (ROOT / folder).rglob("*"):
            if (
                path.is_file()
                and path.suffix in (".c", ".cpp", ".h", ".ps1")
                and not ({"out", ".git"} & set(path.relative_to(ROOT).parts))
            ):
                files.add(path)
    return {path.relative_to(ROOT).as_posix(): fingerprint(path) for path in sorted(files)}


def verify_runtime(directory, *, current=False):
    from _runtime_inputs import ORACLE_SOURCES, oracle_provenance

    if (
        directory.is_symlink()
        or not directory.is_dir()
        or directory_files(directory) != RUNTIME_FILES
    ):
        raise ValueError("Frozen runtime inventory mismatch")
    receipt = read_json(directory / "runtime-build.json")
    valid = (
        receipt.get("format_version") == 1
        and receipt.get("status") == "RUNTIME_OFFLINE_PASS"
        and receipt.get("package_version") == 2
        and receipt.get("dictionary_rules_version") == 2
        and receipt.get("hosts") == HOSTS
    )
    if not valid or set(receipt.get("files", {})) != RUNTIME_CONTENT_FILES:
        raise ValueError("Invalid frozen runtime contract")
    for name in RUNTIME_CONTENT_FILES:
        if receipt["files"][name] != fingerprint(directory / name):
            raise ValueError(f"Frozen runtime digest mismatch: {name}")
    binary = (directory / ASI).read_bytes()
    if any(
        marker not in binary
        for marker in (MARKER, STARTUP_MARKER, HOST_ADAPTER_MARKER, LANGUAGE_MARKER)
    ):
        raise ValueError("Frozen ASI lacks the required runtime entry points")
    oracle = read_json(directory / "dictionary_oracle.json")
    if oracle.get("exe_sha256") != receipt["files"]["dictionary_oracle.exe"]["sha256"] or set(
        oracle.get("sources", {})
    ) != set(ORACLE_SOURCES):
        raise ValueError("Frozen oracle receipt mismatch")
    if any(
        oracle["sources"][name] != receipt.get("sources", {}).get(name, {}).get("sha256")
        for name in ORACLE_SOURCES
    ):
        raise ValueError("ASI and oracle do not share the recorded native sources")
    if current:
        if receipt["sources"] != native_sources():
            raise ValueError("Native sources changed; rebuild the frozen runtime")
        oracle_provenance(directory / "dictionary_oracle.exe")
    return receipt


def build_runtime(args):
    destination = args.out.resolve()
    if destination.exists():
        raise FileExistsError(f"Refusing to overwrite runtime: {destination}")
    if not re.fullmatch(r"\d+\.\d+\.\d+-rc\.\d+", args.version):
        raise ValueError("Runtime version must be MAJOR.MINOR.PATCH-rc.NUMBER")
    pwsh = shutil.which("pwsh")
    if not pwsh:
        raise FileNotFoundError("PowerShell 7 is required")
    sources = native_sources()
    destination.parent.mkdir(parents=True, exist_ok=True)
    lock = ROOT / "build/runtime-candidate.lock"
    lock.parent.mkdir(parents=True, exist_ok=True)
    with lock.open("x", encoding="ascii") as stream:
        stream.write(f"pid={os.getpid()}\n")
    stage = None
    try:
        stage = Path(tempfile.mkdtemp(prefix=".runtime-native-", dir=destination.parent))
        work = stage / "work"
        work.mkdir()
        asi, tests, pe = native_build(pwsh, work, stage)
        shutil.copyfile(asi, stage / ASI)
        oracle = work / "oracle"
        run(
            [
                pwsh,
                "-NoProfile",
                "-File",
                str(ROOT / "tools/modules/build_runtime_oracle.ps1"),
                "-OutDir",
                str(oracle),
            ],
            stage / "oracle.log",
        )
        for name in ("dictionary_oracle.exe", "dictionary_oracle.json"):
            shutil.copyfile(oracle / name, stage / name)
        if native_sources() != sources:
            raise ValueError("Native sources changed during runtime generation")
        if work.resolve().parent != stage.resolve():
            raise ValueError("Unexpected native working directory")
        shutil.rmtree(work)
        receipt = dict(
            format_version=1,
            version=args.version,
            status="RUNTIME_OFFLINE_PASS",
            native="NOT_TESTED_BY_TOOL",
            package_version=2,
            dictionary_rules_version=2,
            hosts=HOSTS,
            native_tests=tests,
            pe=pe,
            sources=sources,
            files={name: fingerprint(stage / name) for name in sorted(RUNTIME_CONTENT_FILES)},
        )
        write_json(stage / "runtime-build.json", receipt)
        verify_runtime(stage, current=True)
        os.rename(stage, destination)
        return receipt
    finally:
        try:
            if stage and stage.exists():
                if (
                    stage.resolve().parent != destination.parent.resolve()
                    or not stage.name.startswith(".runtime-native-")
                ):
                    raise ValueError("Refusing to clean unexpected runtime staging directory")
                shutil.rmtree(stage)
        finally:
            lock.unlink()


def font_notices(font_path, digest):
    from fontTools.ttLib import TTFont

    with TTFont(font_path) as font:
        names = font["name"]
        values = [
            names.getDebugName(index) or "(source metadata not provided)"
            for index in (1, 0, 13, 14)
        ]
    return "\n".join(
        [
            "",
            "## 本次源字体元数据",
            "",
            f"- 字体：{values[0]}",
            f"- 源文件 SHA-256：`{digest}`",
            f"- 版权：{values[1]}",
            f"- 许可声明：{values[2]}",
            f"- 许可地址：{values[3]}",
            "",
        ]
    )


def build(args):
    from _runtime_inputs import read_profile

    destination = args.out.resolve()
    if destination.exists():
        raise FileExistsError(f"Refusing to overwrite candidate: {destination}")
    if not re.fullmatch(r"\d+\.\d+\.\d+-rc\.\d+", args.version):
        raise ValueError("Candidate version must be MAJOR.MINOR.PATCH-rc.NUMBER")
    runtime_directory = args.runtime.resolve()
    runtime = verify_runtime(runtime_directory, current=True)
    profile = read_profile(
        args.language.resolve(),
        args.input_root.resolve(),
        runtime_directory / "dictionary_oracle.exe",
    )
    if profile["runtime_package_version"] != 2:
        raise ValueError("Language candidates require package v2")
    hosts = [
        identity
        for name, identity in profile["hosts"].items()
        if runtime["hosts"].get(name) == identity
    ]
    if len(hosts) != len(profile["hosts"]):
        raise ValueError("Language baseline is not supported by this runtime")
    sources = source_fingerprints()
    destination.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=".runtime-candidate-", dir=destination.parent))
    try:
        evidence = stage / "evidence"
        scripts = stage / "install/scripts"
        evidence.mkdir()
        scripts.mkdir(parents=True)
        frozen = evidence / "runtime"
        shutil.copytree(runtime_directory, frozen)
        runtime = verify_runtime(frozen, current=True)
        package_dir = stage / "package"
        run(
            [
                sys.executable,
                "-X",
                "utf8",
                str(ROOT / "tools/modules/build_runtime.py"),
                "--language",
                str(args.language.resolve()),
                "--font",
                str(args.font.resolve()),
                "--input-root",
                str(args.input_root.resolve()),
                "--oracle",
                str(frozen / "dictionary_oracle.exe"),
                "--out",
                str(package_dir),
            ],
            evidence / "package.log",
        )
        shutil.move(package_dir / PACKAGE, scripts / PACKAGE)
        shutil.move(package_dir, evidence / "package")
        shutil.copyfile(frozen / ASI, scripts / ASI)
        instructions = (
            "# AC1 language package\n\n"
            f"Language: {profile['language_id']}; candidate: {args.version}.\n\n"
            "OFFLINE_PASS; separate Dx9/Dx10 game validation is pending.\n"
            "Close the game and use runtime_install.py plan/install/verify from the source repository.\n"
            "Keep the deployment receipt and backup for rollback; do not overwrite manually.\n\n"
            f"Runtime files: scripts/{ASI}, scripts/{PACKAGE}.\n"
        )
        (stage / "install/README.md").write_text(instructions, encoding="utf-8")
        shutil.copyfile(ROOT / "LICENSE", stage / "install/LICENSE")
        notices = (ROOT / "tools/modules/runtime/CANDIDATE_NOTICES.md").read_text(encoding="utf-8")
        package_report = read_json(evidence / "package/build-report.json")
        notices += font_notices(args.font.resolve(), package_report["font_sha256"])
        notices += "\n\n" + (ROOT / "third_party/minhook/LICENSE.txt").read_text(encoding="utf-8")
        (stage / "install/THIRD_PARTY_NOTICES.md").write_text(notices, encoding="utf-8")
        if source_fingerprints() != sources:
            raise ValueError("Build sources changed during candidate generation")
        manifest = dict(
            format_version=FORMAT_VERSION,
            version=args.version,
            mode=MODE,
            status="CANDIDATE_OFFLINE_PASS",
            native="NOT_TESTED_BY_TOOL",
            distribution="LOCAL_GENERATED_CANDIDATE",
            language_id=profile["language_id"],
            hosts=hosts,
            runtime_version=runtime["version"],
            native_tests=runtime["native_tests"],
            pe=runtime["pe"],
            sources=sources,
            files={name: fingerprint(stage / name) for name in sorted(CONTENT_FILES)},
        )
        write_json(stage / "manifest.json", manifest)
        names = sorted(CONTENT_FILES | {"manifest.json"})
        (stage / "SHA256SUMS").write_text(
            "".join(f"{fingerprint(stage / name)['sha256']}  {name}\n" for name in names),
            encoding="ascii",
            newline="\n",
        )
        verify(stage)
        os.rename(stage, destination)
        return manifest
    finally:
        if stage.exists():
            if stage.resolve().parent != destination.parent.resolve() or not stage.name.startswith(
                ".runtime-candidate-"
            ):
                raise ValueError("Refusing to clean unexpected candidate staging directory")
            shutil.rmtree(stage)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    builder = commands.add_parser(
        "build", help="Build one language candidate using an existing frozen runtime"
    )
    builder.add_argument("--runtime", type=Path, required=True)
    builder.add_argument("--language", type=Path, required=True)
    builder.add_argument("--font", type=Path, required=True)
    builder.add_argument("--input-root", type=Path, required=True)
    builder.add_argument("--version", required=True)
    builder.add_argument("--out", type=Path, required=True)
    native = commands.add_parser(
        "build-runtime", help="Build and freeze ASI and its production dictionary oracle once"
    )
    native.add_argument("--version", required=True)
    native.add_argument("--out", type=Path, required=True)
    runtime_checker = commands.add_parser(
        "verify-runtime", help="Verify a frozen runtime without rebuilding it"
    )
    runtime_checker.add_argument("runtime", type=Path)
    checker = commands.add_parser(
        "verify", help="Check inventory and hashes; no compiler or font required"
    )
    checker.add_argument("candidate", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "build-runtime":
            report = build_runtime(args)
        elif args.command == "verify-runtime":
            report = verify_runtime(args.runtime.resolve())
        elif args.command == "build":
            report = build(args)
        else:
            report = verify(args.candidate.resolve())
        print(f"{report['status']}: {report['version']}; native={report['native']}", flush=True)
        return 0
    except OSError as error:
        print(f"INPUT_ERROR: {error}", file=sys.stderr)
        return 2
    except (ValueError, KeyError, TypeError, AttributeError) as error:
        print(f"CANDIDATE_REJECTED: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
