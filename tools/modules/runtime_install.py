"""Inspect, deploy and recover one current runtime pair through sealed records."""

import argparse
import base64
import contextlib
import hashlib
import json
import os
import queue
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import time
import zlib
from datetime import datetime, timezone
from pathlib import Path

import runtime_candidate

KIND = "AC1_RUNTIME_INSTALL_INSPECTION"
PROJECT_FILES = (runtime_candidate.ASI, runtime_candidate.PACKAGE)
RECORD_VERSION = 3
LEGACY_DATA = {"ac1_dict.txt", "ac1_cjk_glyphs.bin", "ac1_cjk_forge.txt", "ac1_cjk_runtime.bin"}
DISABLED_SUFFIXES = (".off", ".disabled", ".bak")


def linked(path):
    info = path.lstat()
    reparse = getattr(info, "st_file_attributes", 0) & getattr(
        stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0
    )
    return stat.S_ISLNK(info.st_mode) or bool(reparse)


def checked_path(path):
    # Check before resolve: resolving first hides a junction or symlink supplied
    # as an input root or output parent.
    path = Path(os.path.abspath(path))
    for entry in (path, *path.parents):
        try:
            is_linked = linked(entry)
        except FileNotFoundError:
            continue
        if is_linked:
            raise ValueError(f"Linked path is not inspected: {entry}")
    return path


def plugin_state(name):
    name = name.casefold()
    if name.endswith(".asi"):
        return "ENABLED"
    if any(name.endswith(".asi" + suffix) for suffix in DISABLED_SUFFIXES):
        return "DISABLED"
    return None


def file_observation(path):
    digest, crc, count = hashlib.sha256(), 0, 0
    marker, tail, runtime = runtime_candidate.MARKER, b"", False
    is_plugin = plugin_state(path.name) is not None
    with path.open("rb") as stream:
        before = os.fstat(stream.fileno())
        if not stat.S_ISREG(before.st_mode):
            raise ValueError(f"Not a regular file: {path}")
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
            crc = zlib.crc32(block, crc)
            count += len(block)
            if is_plugin:
                window = tail + block
                runtime |= marker in window
                tail = window[-len(marker) :]
        after = os.fstat(stream.fileno())
    current = path.stat()

    def identity(info):
        return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns

    if identity(before) != identity(after) or identity(after) != identity(current):
        raise ValueError(f"File changed during inspection: {path}")
    if count != before.st_size or linked(path):
        raise ValueError(f"File changed during inspection: {path}")
    return dict(
        fingerprint=dict(bytes=count, sha256=digest.hexdigest()),
        crc32=f"{crc:08X}",
        runtime_marker=runtime,
    )


def installation_files(game, host_names):
    selected, skipped_links = {}, []
    root_names = {name.casefold() for name in host_names} | {
        "datapc.forge",
        "dinput8.dll",
        *LEGACY_DATA,
    }
    for path in sorted(game.iterdir()):
        wanted = path.name.casefold() in root_names or plugin_state(path.name)
        if not wanted:
            continue
        if linked(path):
            skipped_links.append(path.relative_to(game).as_posix())
            continue
        if not path.is_file():
            raise ValueError(f"Expected a file: {path}")
        selected[path.relative_to(game).as_posix()] = file_observation(path)

    scripts = checked_path(game / "scripts")
    if not scripts.exists():
        return selected, skipped_links
    if linked(scripts) or not scripts.is_dir():
        raise ValueError("scripts must be a real directory")

    # Loader-visible ASIs can be nested. Observe those, plus root configuration
    # files; nested logs, dumps and assets are not copied or hashed here.
    def scan_error(error):
        raise error

    for folder, directories, names in os.walk(scripts, followlinks=False, onerror=scan_error):
        for name in sorted(directories[:]):
            path = Path(folder) / name
            if linked(path):
                skipped_links.append(path.relative_to(game).as_posix())
                directories.remove(name)
        for name in sorted(names):
            path = Path(folder) / name
            wanted = Path(folder) == scripts or plugin_state(name)
            if not wanted:
                continue
            relative = path.relative_to(game).as_posix()
            if linked(path):
                skipped_links.append(relative)
                continue
            selected[relative] = file_observation(path)
    return selected, sorted(skipped_links)


def project_plugin(relative, observation, expected_asi):
    name = Path(relative).name.casefold()
    named = name.startswith(("ac1_cjk", "ac1_rt_", "ac1_runtime_", "ac1_load_diag"))
    same_bytes = observation["fingerprint"] == expected_asi
    return named or same_bytes or observation["runtime_marker"]


def classify_files(observed, hosts, expected_pair):
    managed = {("scripts/" + name).casefold() for name in expected_pair}
    special = {host["exe"].casefold() for host in hosts} | {"datapc.forge", "dinput8.dll"}
    plugins, third_party, legacy, conflicts = [], {}, [], []
    for relative, observation in observed.items():
        if relative.casefold() in managed or relative.casefold() in special:
            continue
        state = plugin_state(relative)
        own_plugin = state and project_plugin(
            relative, observation, expected_pair[runtime_candidate.ASI]
        )
        old_data = Path(relative).name.casefold() in LEGACY_DATA
        retired_package = Path(relative).name.casefold() == "ac1_cjk_runtime.bin"
        record = dict(path=relative, fingerprint=observation["fingerprint"])
        if state:
            record["activation"] = state
        if own_plugin or old_data:
            legacy.append(record)
            if retired_package or (own_plugin and state == "ENABLED"):
                conflicts.append(record)
            continue
        third_party[relative] = observation["fingerprint"]
        if state:
            plugins.append(record)
    return third_party, plugins, legacy, conflicts


def compare_third_party(baseline_path, game, current):
    if baseline_path is None:
        return None
    previous = runtime_candidate.read_json(baseline_path)
    valid = (
        isinstance(previous, dict)
        and previous.get("kind") == KIND
        and type(previous.get("format_version")) is int
        and previous["format_version"] == RECORD_VERSION
        and os.path.normcase(previous.get("game", "")) == os.path.normcase(str(game))
    )
    if not valid:
        raise ValueError("Baseline must be an inspection of the same game directory")
    files = previous.get("third_party", {}).get("files")
    if not isinstance(files, dict):
        raise ValueError("Baseline has no third-party file snapshot")
    for name, value in files.items():
        valid_path = (
            isinstance(name, str) and not Path(name).is_absolute() and ".." not in Path(name).parts
        )
        valid_fingerprint = (
            isinstance(value, dict)
            and type(value.get("bytes")) is int
            and value["bytes"] >= 0
            and isinstance(value.get("sha256"), str)
            and re.fullmatch(r"[0-9a-f]{64}", value["sha256"]) is not None
            and set(value) == {"bytes", "sha256"}
        )
        if not valid_path or not valid_fingerprint:
            raise ValueError("Invalid baseline third-party snapshot")
    removed = {name: value for name, value in files.items() if name not in current}
    added = {name: value for name, value in current.items() if name not in files}
    renamed = []
    for old_name, value in list(removed.items()):
        matches = [name for name, actual in added.items() if actual == value]
        old_copies = [name for name, actual in removed.items() if actual == value]
        if len(matches) != 1 or len(old_copies) != 1:
            continue
        new_name = matches[0]
        before, after = plugin_state(old_name), plugin_state(new_name)
        changed = before is not None and after is not None and before != after
        renamed.append(
            dict(before=old_name, after=new_name, fingerprint=value, activation_changed=changed)
        )
        del removed[old_name]
        del added[new_name]
    modified = [
        dict(path=name, before=files[name], after=current[name])
        for name in sorted(files.keys() & current.keys())
        if files[name] != current[name]
    ]
    return dict(
        baseline=dict(path=str(baseline_path), **runtime_candidate.fingerprint(baseline_path)),
        renamed=renamed,
        added=added,
        removed=removed,
        modified=modified,
    )


def build_inspection(candidate, game, baseline=None):
    manifest = runtime_candidate.verify(candidate)
    target_hosts = manifest["hosts"]
    observed, skipped_links = installation_files(game, [host["exe"] for host in target_hosts])
    by_name = {}
    for name in observed:
        key = name.casefold()
        if key in by_name:
            raise ValueError(f"Ambiguous case-insensitive installation path: {name}")
        by_name[key] = name

    def find(relative):
        name = by_name.get(relative.casefold())
        return observed.get(name) if name is not None else None

    issues = []
    hosts = []
    for expected_host in target_hosts:
        actual_host = find(expected_host["exe"])
        state = (
            "MISSING"
            if actual_host is None
            else "MATCHES"
            if actual_host["crc32"] == expected_host["crc32"]
            else "CRC_MISMATCH"
        )
        hosts.append(dict(expected=expected_host, state=state, actual=actual_host))
        if state == "CRC_MISMATCH":
            issues.append(dict(code="HOST_CRC_MISMATCH", path=expected_host["exe"]))
    if not any(row["actual"] is not None for row in hosts):
        issues.append(dict(code="HOST_MISSING", paths=[host["exe"] for host in target_hosts]))
    resource = find("DataPC.forge")
    if resource is None:
        issues.append(dict(code="RESOURCE_ARCHIVE_MISSING", path="DataPC.forge"))
    if skipped_links:
        issues.append(dict(code="LINKED_ENTRIES_NOT_INSPECTED", paths=skipped_links))

    project = []
    expected_pair = {}
    for name in PROJECT_FILES:
        relative = "scripts/" + name
        expected = manifest["files"]["install/" + relative]
        expected_pair[name] = expected
        actual = find(relative)
        fingerprint = actual["fingerprint"] if actual else None
        state = (
            "MATCHES" if fingerprint == expected else "ABSENT" if actual is None else "DIFFERENT"
        )
        project.append(dict(path=relative, expected=expected, actual=fingerprint, state=state))
    states = {entry["state"] for entry in project}
    pair_state = (
        "MATCHES"
        if states == {"MATCHES"}
        else "ABSENT"
        if states == {"ABSENT"}
        else "PARTIAL"
        if "ABSENT" in states
        else "DIFFERENT"
    )
    if pair_state in {"PARTIAL", "DIFFERENT"}:
        issues.append(
            dict(code="PROJECT_PAIR_" + pair_state, paths=[entry["path"] for entry in project])
        )

    third_party, plugins, legacy, conflicts = classify_files(observed, target_hosts, expected_pair)
    if conflicts:
        issues.append(
            dict(code="OTHER_PROJECT_ASI_ENABLED", paths=[row["path"] for row in conflicts])
        )
    loader = find("dinput8.dll")
    target = dict(
        path=str(candidate),
        version=manifest["version"],
        manifest=runtime_candidate.fingerprint(candidate / "manifest.json"),
    )
    target.update(
        format_version=runtime_candidate.FORMAT_VERSION,
        hosts=target_hosts,
        language_id=manifest["language_id"],
    )
    report = dict(
        format_version=RECORD_VERSION,
        hosts=hosts,
        kind=KIND,
        status="INSPECTED",
        at_utc=datetime.now(timezone.utc).isoformat(),
        game=str(game),
        candidate=target,
        resources=dict(path="DataPC.forge", actual=resource, identity="RUNTIME_GUARDS_REQUIRED"),
        loader=dict(
            path="dinput8.dll",
            state="PRESENT" if loader else "MISSING",
            actual=loader,
            synchronous_initializeasi="NOT_VERIFIED_BY_INSPECTOR",
        ),
        project=dict(
            state=pair_state, files=project, other_project_files=legacy, conflicts=conflicts
        ),
        third_party=dict(
            files=third_party,
            plugins=plugins,
            changes=compare_third_party(baseline, game, third_party),
        ),
        issues=issues,
        limits=[
            "This is a per-file read-only snapshot, not an install authorization or an atomic directory snapshot.",
            "Processes and native behavior are not assessed. Install must recheck inputs and reliably confirm the game is closed.",
            "Host CRC match does not prove resource identities; the native font/texture guards remain required.",
            "dinput8.dll is fingerprinted when present; its loader version/InitializeASI behavior is not inferred from its filename.",
            "Third-party activation refers to filenames only, not observed loaded modules or compatibility.",
            "Scope: selected host/DataPC.forge/dinput8.dll/root ASIs and legacy data; scripts root files and nested ASIs. Nested logs/assets/configs are not hashed.",
            "Different project bytes mean different from this candidate, not necessarily corrupt. No files are deleted, disabled or restored.",
        ],
    )
    return report


def markdown(report):
    lines = [
        "# 运行时安装检查",
        "",
        f"候选：`{report['candidate']['version']}`。",
        f"项目配对：`{report['project']['state']}`。",
        "",
        "本命令只读游戏目录；未检查进程、原生运行或加载器同步调用能力，不能直接据此开始部署。",
        "",
        "## 宿主",
        "",
        "| 文件 | 状态 |",
        "| --- | --- |",
    ]
    lines.extend(f"| {row['expected']['exe']} | {row['state']} |" for row in report["hosts"])
    lines.extend(["", "## 项目文件", "", "| 文件 | 状态 |", "| --- | --- |"])
    lines.extend(f"| {row['path']} | {row['state']} |" for row in report["project"]["files"])
    lines.extend(["", "## 其它项目文件（仅列出）", ""])
    for row in report["project"]["other_project_files"]:
        lines.append(f"- `{row['path']}`：{row.get('activation', '旧数据')}。")
    lines.extend(["", "## 第三方插件", "", "| 文件 | 文件名启用状态 |", "| --- | --- |"])
    lines.extend(
        f"| {row['path'].replace('|', '&#124;')} | {row['activation']} |"
        for row in report["third_party"]["plugins"]
    )
    lines.extend(["", "## 需核对的安装状态", ""])
    for row in report["issues"]:
        paths = row.get("paths", [row.get("path", "")])
        lines.append("- `" + row["code"] + "`：" + ", ".join(paths))
    if not report["issues"]:
        lines.append("本次范围内没有宿主、项目配对或旧项目ASI冲突。")
    changes = report["third_party"]["changes"]
    if changes:
        lines.extend(["", "## 与前次检查比较", ""])
        lines.append(
            f"同内容改名 {len(changes['renamed'])}，修改 {len(changes['modified'])}，新增 {len(changes['added'])}，缺失 {len(changes['removed'])}。"
        )
        for row in changes["renamed"]:
            lines.append(
                f"- `{row['before']}` → `{row['after']}`；启停文件名变化：{row['activation_changed']}。"
            )
    lines.extend(["", "完整摘要、其它项目文件、比较明细及检查范围见 `inspection.json`。", ""])
    return "\n".join(lines)


def inspect_installation(candidate, game, out, baseline=None):
    candidate, game, out = (checked_path(path) for path in (candidate, game, out))
    baseline = checked_path(baseline) if baseline is not None else None
    if not game.is_dir():
        raise FileNotFoundError(f"Game directory does not exist: {game}")
    if out.exists():
        raise FileExistsError(f"Inspection output already exists: {out}")
    protected_roots = (candidate, game, *([baseline.parent] if baseline else []))
    if any(out == root or root in out.parents for root in protected_roots):
        raise ValueError(
            "Inspection output must be outside game, candidate and baseline report directories"
        )
    report = build_inspection(candidate, game, baseline)
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".runtime-inspect-", dir=out.parent) as temporary:
        stage = Path(temporary) / "report"
        stage.mkdir()
        runtime_candidate.write_json(stage / "inspection.json", report)
        (stage / "inspection.md").write_text(markdown(report), encoding="utf-8", newline="\n")
        # Windows can transiently deny the rename of newly written report
        # files. Retry only that denial, never a destination collision.
        for attempt in range(4):
            try:
                stage.rename(out)
                break
            except PermissionError as error:
                transient = os.name == "nt" and error.winerror in {5, 32}
                if not transient or out.exists() or attempt == 3:
                    raise
                time.sleep(0.05)
    return report


ORDER = (runtime_candidate.PACKAGE, runtime_candidate.ASI)
SUCCESS = {"PLAN_READY", "DEPLOYED", "VERIFIED", "ROLLED_BACK"}

# FileShare.None holds both host files closed to other open requests throughout
# the write, in addition to CIM process inspection. A sandbox-filtered empty
# process list alone is not sufficient. FileMode.Open never creates/truncates.
# https://learn.microsoft.com/dotnet/api/system.io.fileshare
GATE_SCRIPT = r"""
$ErrorActionPreference = 'Stop'
[Console]::InputEncoding = [Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$handles = @()
try {
    $request = [Console]::ReadLine() | ConvertFrom-Json
    $all = @(Get-CimInstance Win32_Process -ErrorAction Stop)
    if (-not ($all.ProcessId -contains $PID) -or -not ($all.ProcessId -contains $request.parent)) {
        throw 'Process view does not contain the guard and its parent'
    }
    $running = @($all | Where-Object { $_.Name -in $request.names })
    if ($running.Count -ne 0) { throw 'An Assassin Creed host process is running' }
    $files = @{}
    foreach ($path in $request.paths) {
        $file = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        $handles += $file
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $hash = [BitConverter]::ToString($sha.ComputeHash($file)).Replace('-', '').ToLowerInvariant() }
        finally { $sha.Dispose() }
        $files[[IO.Path]::GetFileName($path)] = @{ bytes = $file.Length; sha256 = $hash }
    }
    @{ status = 'LOCKED'; files = $files; process_view = 'CIM'; sharing = 'NONE' } | ConvertTo-Json -Depth 5 -Compress
    $null = [Console]::ReadLine()
} catch {
    @{ status = 'REJECTED'; error = $_.Exception.Message } | ConvertTo-Json -Compress
    exit 2
} finally {
    foreach ($file in $handles) { $file.Dispose() }
}
"""


class ClosedGame:
    """Windows adapter; no production skip/assume-closed option exists."""

    def __init__(self, game):
        self.game, self.process = game, None

    def __enter__(self):
        if os.name != "nt":
            raise RuntimeError("Write operations require the Windows process/file guard")
        names = [host["exe"] for host in runtime_candidate.HOSTS.values()]
        paths = [checked_path(self.game / name) for name in names]
        paths = [path for path in paths if path.exists()]
        if not paths:
            raise ValueError("No host executable to lock")
        encoded = base64.b64encode(GATE_SCRIPT.encode("utf-16-le")).decode("ascii")
        powershell = (
            Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"
        )
        self.process = subprocess.Popen(
            [str(powershell), "-NoProfile", "-NonInteractive", "-EncodedCommand", encoded],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            encoding="utf-8",
        )
        try:
            request = dict(parent=os.getpid(), names=names, paths=[str(path) for path in paths])
            self.process.stdin.write(json.dumps(request) + "\n")
            self.process.stdin.flush()
            answer = queue.Queue()
            reader = threading.Thread(
                target=lambda: answer.put(self.process.stdout.readline()), daemon=True
            )
            reader.start()
            try:
                self.evidence = json.loads(answer.get(timeout=30))
            except (queue.Empty, json.JSONDecodeError) as error:
                raise RuntimeError("Game guard did not produce a reliable result") from error
            if self.evidence.get("status") != "LOCKED":
                raise RuntimeError(self.evidence.get("error", "Game guard rejected the operation"))
            self.check()
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def check(self):
        if self.process.poll() is not None:
            raise RuntimeError("Game guard exited; write refused")

    def __exit__(self, *unused):
        if self.process is None:
            return
        with contextlib.suppress(OSError):
            self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        self.process.stdout.close()


def identity(path):
    path = checked_path(path)
    if not path.exists():
        return None
    return file_observation(path)["fingerprint"]


def pair(game):
    return {name: identity(game / "scripts" / name) for name in PROJECT_FILES}


def output_directory(out, *protected):
    out = checked_path(out)
    if out.exists():
        raise FileExistsError(f"Output already exists: {out}")
    for root in protected:
        root = checked_path(root)
        if out == root or root in out.parents or out in root.parents:
            raise ValueError("Output must be outside input directories and cannot contain them")
    return out


def write_record(path, value):
    # Flush the pre-commit record and backups before touching installed targets.
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())


def seal_record(folder, name, value):
    target, seal = folder / name, folder / (name + ".sha256")
    if target.exists() or seal.exists():
        raise FileExistsError(f"Record already exists: {target}")
    with tempfile.TemporaryDirectory(prefix=".record-", dir=folder) as temporary:
        staged = Path(temporary) / name
        write_record(staged, value)
        digest = identity(staged)["sha256"]
        staged_seal = Path(temporary) / "sha256"
        with staged_seal.open("x", encoding="ascii") as stream:
            stream.write(digest + "\n")
            stream.flush()
            os.fsync(stream.fileno())
        published_seal = False
        try:
            staged_seal.rename(seal)
            published_seal = True
            # Publishing JSON last makes it the record's commit point.
            staged.rename(target)
        except BaseException:
            if published_seal and not target.exists():
                seal.unlink()
            raise


def read_record(folder, name):
    path = checked_path(folder / name)
    digest = checked_path(folder / (name + ".sha256")).read_text(encoding="ascii").strip()
    if identity(path)["sha256"] != digest:
        raise ValueError(f"Record digest mismatch: {path}")
    value = runtime_candidate.read_json(path)
    if (
        not isinstance(value, dict)
        or type(value.get("format_version")) is not int
        or value["format_version"] != RECORD_VERSION
    ):
        raise ValueError(f"Unsupported record: {path}")
    return value


def stamp(kind, **fields):
    return dict(
        format_version=RECORD_VERSION,
        kind=kind,
        at_utc=datetime.now(timezone.utc).isoformat(),
        **fields,
    )


def binding(report):
    return dict(
        candidate=report["candidate"],
        resource=report["resources"]["actual"]["fingerprint"],
        loader=report["loader"]["actual"]["fingerprint"],
        before={Path(row["path"]).name: row["actual"] for row in report["project"]["files"]},
        after={Path(row["path"]).name: row["expected"] for row in report["project"]["files"]},
        hosts={
            row["expected"]["exe"]: row["actual"]["fingerprint"]
            for row in report["hosts"]
            if row["actual"] is not None
        },
    )


def deployable(report):
    blocking = [row for row in report["issues"] if row["code"] != "PROJECT_PAIR_DIFFERENT"]
    if blocking or report["loader"]["state"] != "PRESENT":
        raise ValueError(
            f"Installation cannot be planned: {blocking}; loader={report['loader']['state']}"
        )


def plan_installation(source, game, out):
    source, game = (checked_path(path) for path in (source, game))
    if source == game or source in game.parents or game in source.parents:
        raise ValueError("Candidate and game directories must be separate")
    out = output_directory(out, source, game)
    report = build_inspection(source, game)
    deployable(report)
    record = stamp(
        "AC1_RUNTIME_INSTALL_PLAN",
        status="PLAN_READY",
        game=str(game),
        binding=binding(report),
    )
    out.mkdir(parents=True)
    # A plan is usable only once both its report and seal exist.
    write_record(out / "inspection.json", report)
    record["inspection"] = identity(out / "inspection.json")
    seal_record(out, "plan.json", record)
    return record


def validate_binding(value):
    for key in ("before", "after"):
        files = value[key]
        if not isinstance(files, dict) or set(files) != set(PROJECT_FILES):
            raise ValueError("Invalid managed file inventory")
        absent = all(item is None for item in files.values())
        if key == "before" and absent:
            continue
        for fingerprint in files.values():
            valid = (
                isinstance(fingerprint, dict)
                and set(fingerprint) == {"bytes", "sha256"}
                and type(fingerprint["bytes"]) is int
                and fingerprint["bytes"] >= 0
                and isinstance(fingerprint["sha256"], str)
                and re.fullmatch(r"[0-9a-f]{64}", fingerprint["sha256"]) is not None
            )
            if not valid:
                raise ValueError("Invalid managed file fingerprint or partial pair")
    if value["candidate"].get("format_version") != runtime_candidate.FORMAT_VERSION:
        raise ValueError("Invalid candidate format in deployment record")
    hosts = value["candidate"]["hosts"]
    if not hosts or any(host not in runtime_candidate.HOSTS.values() for host in hosts):
        raise ValueError("Unsupported hosts in deployment record")
    host_names = {host["exe"] for host in hosts}
    if len(host_names) != len(hosts) or not value["hosts"] or not set(value["hosts"]) <= host_names:
        raise ValueError("Invalid installed host inventory")


def validated_plan(folder):
    folder = checked_path(folder)
    record = read_record(folder, "plan.json")
    if record.get("kind") != "AC1_RUNTIME_INSTALL_PLAN" or record.get("status") != "PLAN_READY":
        raise ValueError("Expected a formal installation plan")
    validate_binding(record["binding"])
    if identity(folder / "inspection.json") != record["inspection"]:
        raise ValueError("Plan inspection digest mismatch")
    return folder, record


def copy_verified(source, target, expected):
    checked_path(source)
    checked_path(target)
    if identity(source) != expected:
        raise ValueError(f"Source changed: {source}")
    shutil.copy2(source, target)
    with target.open("r+b") as stream:
        os.fsync(stream.fileno())
    shutil.copystat(source, target)
    if identity(target) != expected:
        raise ValueError(f"Copy digest mismatch: {target}")


def check_guard(guard, bound):
    guard.check()
    supported = {host["exe"] for host in bound["candidate"]["hosts"]}
    observed = {name: value for name, value in guard.evidence["files"].items() if name in supported}
    if observed != bound["hosts"]:
        raise ValueError("Host changed before the exclusive game lock")


def check_targets(game, expected):
    if pair(game) != expected:
        raise ValueError("Managed files changed; plan or rollback is stale")


def scan_conflicts(game, bound):
    # Hosts are already exclusively held by the guard and cannot be opened a
    # second time here. The remaining scan still covers all loader-visible ASIs.
    observed, links = installation_files(game, "")
    hosts = bound["candidate"]["hosts"]
    sections = classify_files(observed, hosts, bound["after"])
    if links or sections[3]:
        raise ValueError("Linked installation entries or another active project ASI")
    return observed


def replace_file(source, target):
    # Both paths are on the game volume. This atomic step covers one file only.
    checked_path(source)
    checked_path(target)
    os.replace(source, target)


def restore(game, backups, bound, guard, stage):
    for name in reversed(ORDER):
        check_guard(guard, bound)
        target, previous = game / "scripts" / name, bound["before"][name]
        current = identity(target)
        if current == previous:
            continue
        if current != bound["after"][name]:
            raise ValueError(f"User-modified target cannot be restored: {target}")
        if previous is None:
            if identity(target) != current:
                raise ValueError("Target changed during recovery")
            target.unlink()
        else:
            staged = stage / (name + ".restore")
            copy_verified(backups / name, staged, previous)
            if identity(target) != current:
                raise ValueError("Target changed during recovery")
            replace_file(staged, target)
    check_targets(game, bound["before"])


def finish(folder, transaction, status, **extra):
    result = stamp(
        "AC1_RUNTIME_INSTALL_RESULT",
        transaction=identity(folder / "transaction.json"),
        operation=transaction["operation"],
        status=status,
        native="NOT_ASSESSED",
        **extra,
    )
    seal_record(folder, "result.json", result)
    return result


def install_plan(plan, out):
    plan, record = validated_plan(plan)
    game = checked_path(record["game"])
    bound = record["binding"]
    source = checked_path(bound["candidate"]["path"])
    out = output_directory(out, game, source, plan)
    fresh = build_inspection(source, game)
    deployable(fresh)
    if binding(fresh) != bound:
        raise ValueError("Deployment plan is stale")
    with ClosedGame(game) as guard:
        check_guard(guard, bound)
        check_targets(game, bound["before"])
        scan_conflicts(game, bound)
        for name, expected in (
            ("DataPC.forge", bound["resource"]),
            ("dinput8.dll", bound["loader"]),
        ):
            if identity(game / name) != expected:
                raise ValueError(f"Protected input changed: {name}")
        out.mkdir(parents=True)
        backups = out / "backup"
        backups.mkdir()
        write_record(out / "inspection.json", fresh)
        for name, previous in bound["before"].items():
            if previous is not None:
                copy_verified(game / "scripts" / name, backups / name, previous)
        scripts = checked_path(game / "scripts")
        scripts.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".ac1-runtime-", dir=scripts) as temporary:
            stage = checked_path(temporary)
            for name, expected in bound["after"].items():
                copy_verified(
                    source / "install/scripts" / name,
                    stage / (name + ".stage"),
                    expected,
                )
            transaction = stamp(
                "AC1_RUNTIME_INSTALL_TRANSACTION",
                operation="INSTALL",
                game=str(game),
                binding=bound,
                plan=identity(plan / "plan.json"),
                guard=guard.evidence,
                inspection=identity(out / "inspection.json"),
                stage=str(stage),
            )
            seal_record(out, "transaction.json", transaction)
            try:
                check_targets(game, bound["before"])
                scan_conflicts(game, bound)
                for name in ORDER:
                    check_guard(guard, bound)
                    if identity(game / "scripts" / name) != bound["before"][name]:
                        raise ValueError("Managed file changed before replacement")
                    if bound["before"][name] != bound["after"][name]:
                        replace_file(stage / (name + ".stage"), game / "scripts" / name)
                check_targets(game, bound["after"])
                return finish(out, transaction, "DEPLOYED")
            except Exception as error:
                try:
                    restore(game, backups, bound, guard, stage)
                except Exception as recovery:
                    return finish(
                        out,
                        transaction,
                        "RECOVERY_REQUIRED",
                        error=str(error),
                        recovery_error=str(recovery),
                    )
                return finish(out, transaction, "FAILED_RESTORED", error=str(error))


def deployment_record(folder):
    folder = checked_path(folder)
    transaction = read_record(folder, "transaction.json")
    if (
        transaction.get("kind") != "AC1_RUNTIME_INSTALL_TRANSACTION"
        or transaction.get("operation") != "INSTALL"
    ):
        raise ValueError("Expected an installation transaction")
    validate_binding(transaction["binding"])
    if identity(folder / "inspection.json") != transaction["inspection"]:
        raise ValueError("Deployment inspection digest mismatch")
    result = None
    if (folder / "result.json").exists():
        result = read_record(folder, "result.json")
        if (
            result.get("kind") != "AC1_RUNTIME_INSTALL_RESULT"
            or result.get("transaction") != identity(folder / "transaction.json")
            or result["format_version"] != transaction["format_version"]
        ):
            raise ValueError("Result does not bind this installation transaction")
    return folder, transaction, result


def verify_deployment(receipt, out):
    receipt, transaction, result = deployment_record(receipt)
    game = checked_path(transaction["game"])
    bound = transaction["binding"]
    out = output_directory(out, receipt, game, bound["candidate"]["path"])
    actual = pair(game)
    matches = actual == bound["after"]
    committed = result is not None and result["status"] == "DEPLOYED"
    hosts = bound["candidate"]["hosts"]
    observed, links = installation_files(game, [host["exe"] for host in hosts])
    third_party, plugins, legacy, conflicts = classify_files(observed, hosts, bound["after"])
    protected = {}
    protected_hosts = bound["hosts"]
    protected_hosts = {host["exe"]: protected_hosts.get(host["exe"]) for host in hosts}
    for name, expected in (
        *protected_hosts.items(),
        ("DataPC.forge", bound["resource"]),
        ("dinput8.dll", bound["loader"]),
    ):
        actual_input = identity(game / name)
        protected[name] = dict(
            expected=expected, actual=actual_input, matches=actual_input == expected
        )
    inputs_match = all(row["matches"] for row in protected.values())
    valid = matches and committed and inputs_match and not links and not conflicts
    report = stamp(
        "AC1_RUNTIME_INSTALL_VERIFICATION",
        status="VERIFIED" if valid else "ATTENTION",
        transaction=identity(receipt / "transaction.json"),
        recorded_result=result["status"] if result else "INTERRUPTED",
        project=dict(expected=bound["after"], actual=actual, matches=matches),
        third_party=compare_third_party(receipt / "inspection.json", game, third_party),
        plugins=plugins,
        other_project_files=legacy,
        conflicts=conflicts,
        protected=protected,
        linked_entries=links,
        native="NOT_ASSESSED",
    )
    out.mkdir(parents=True)
    seal_record(out, "verification.json", report)
    return report


def rollback_deployment(receipt, out):
    receipt = checked_path(receipt)
    requested = receipt
    selected = read_record(receipt, "transaction.json")
    recovering = selected.get("kind") == "AC1_RUNTIME_ROLLBACK_TRANSACTION"
    if recovering:
        receipt = checked_path(selected["installation_source"])
    receipt, transaction, result = deployment_record(receipt)
    if recovering:
        same_installation = (
            selected.get("operation") == "ROLLBACK"
            and selected.get("installation") == identity(receipt / "transaction.json")
            and selected.get("binding") == transaction["binding"]
            and selected.get("game") == transaction["game"]
        )
        if not same_installation:
            raise ValueError("Rollback event does not bind the original installation")
    game = checked_path(transaction["game"])
    bound = transaction["binding"]
    out = output_directory(out, requested, receipt, game, bound["candidate"]["path"])
    for name, expected in bound["before"].items():
        if expected is not None and identity(receipt / "backup" / name) != expected:
            raise ValueError(f"Rollback backup changed or missing: {name}")
    current = pair(game)
    complete = result is not None and result["status"] == "DEPLOYED"
    if complete and not recovering and current != bound["after"]:
        raise ValueError("Managed files changed since deployment; rollback refused")
    if any(
        current[name] not in (bound["before"][name], bound["after"][name])
        for name in bound["after"]
    ):
        raise ValueError("User-modified target cannot be recovered")
    with ClosedGame(game) as guard:
        check_guard(guard, bound)
        check_targets(game, current)
        scan_conflicts(game, bound)
        out.mkdir(parents=True)
        event = stamp(
            "AC1_RUNTIME_ROLLBACK_TRANSACTION",
            operation="ROLLBACK",
            game=str(game),
            installation=identity(receipt / "transaction.json"),
            installation_source=str(receipt),
            binding=bound,
            guard=guard.evidence,
        )
        seal_record(out, "transaction.json", event)
        scripts = checked_path(game / "scripts")
        scripts.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".ac1-runtime-", dir=scripts) as temporary:
            try:
                restore(game, receipt / "backup", bound, guard, Path(temporary))
            except Exception as error:
                return finish(out, event, "RECOVERY_REQUIRED", error=str(error))
        return finish(out, event, "ROLLED_BACK")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    inspector = commands.add_parser(
        "inspect", help="Read installation files and save a new inspection report"
    )
    inspector.add_argument("--candidate", type=Path, required=True)
    inspector.add_argument("--game", type=Path, required=True)
    inspector.add_argument("--out", type=Path, required=True)
    inspector.add_argument(
        "--baseline", type=Path, help="Previous inspection.json for third-party file comparison"
    )
    planner = commands.add_parser(
        "plan", help="Save a bound deployment plan without changing the game"
    )
    planner.add_argument("--candidate", type=Path, required=True)
    planner.add_argument("--game", type=Path, required=True)
    planner.add_argument("--out", type=Path, required=True)
    for command, flag in (
        ("install", "--plan"),
        ("verify", "--receipt"),
        ("rollback", "--receipt"),
    ):
        operation = commands.add_parser(command)
        operation.add_argument(flag, type=Path, required=True, help="Plan or deployment directory")
        operation.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command != "inspect":
            if args.command == "plan":
                report = plan_installation(args.candidate, args.game, args.out)
            elif args.command == "install":
                report = install_plan(args.plan, args.out)
            elif args.command == "verify":
                report = verify_deployment(args.receipt, args.out)
            else:
                report = rollback_deployment(args.receipt, args.out)
            print(json.dumps(report, ensure_ascii=False))
            return 0 if report["status"] in SUCCESS else 1
        report = inspect_installation(args.candidate, args.game, args.out, args.baseline)
    except (OSError, ValueError, KeyError, TypeError, AttributeError, RuntimeError) as error:
        print(f"{args.command} rejected: {error}", file=sys.stderr)
        return 2
    summary = dict(
        status=report["status"],
        hosts={row["expected"]["exe"]: row["state"] for row in report["hosts"]},
        project=report["project"]["state"],
        issues=len(report["issues"]),
    )
    print(json.dumps(summary, ensure_ascii=False))
    return 1 if report["issues"] else 0


if __name__ == "__main__":
    sys.exit(main())
