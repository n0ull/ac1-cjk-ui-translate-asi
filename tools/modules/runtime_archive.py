"""Archive one explicit runtime run, its provenance and text audit without changing inputs."""

import argparse
import hashlib
import json
import os
import re
import stat
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

import audit_runtime_text as text_audit
import runtime_candidate as candidate
import runtime_install as installation

ROOT = Path(__file__).resolve().parents[2]
LINE = re.compile(r"\[([^]]+)\]\[(INFO|DEBUG|WARN|ERR|FATAL)\] (.*)")
START = re.compile(r"=== ac1_cjk 已注入 pid=(\d+) run=(\d{8}_\d{6}) ===")
HOST = re.compile(r"\[启动\] 宿主 ✓ exe=(\S+).* CRC32=([0-9A-Fa-f]{8})")
RTP_FIELDS = (
    "gate",
    "atlas_mask",
    "uploaded_mask",
    "expanded",
    "uploads",
    "fonts",
    "glyphs",
    "retained",
    "rejects",
    "faults",
    "font_failed",
)
FONT_TRACK_FIELDS = ("font_active", "font_peak", "font_limit", "font_full")
FUNNEL = re.compile(
    r"\[状态\] FUNNEL 触发=(\d+) 替换=(\d+) 未命中=(\d+)\(ASCII (\d+)/非ASCII (\d+)\) "
    r"跳过=空串 (\d+)/同值 (\d+)/词典未就绪 (\d+)/结构不可信 (\d+)/其它 (\d+) "
    r"容量不足=(\d+) 异常=(\d+) 样本=(\d+)\+(\d+)/(\d+)"
)
TEXT_FIELDS = (
    "hits",
    "replaced",
    "miss",
    "miss_ascii",
    "miss_nonascii",
    "skip_empty",
    "skip_unchanged",
    "skip_dictionary_not_ready",
    "skip_untrusted_view",
    "skip_other",
    "capacity_failures",
    "exceptions",
    "sample_hit",
    "sample_miss",
    "sample_limit",
)
LIMITS = [
    "STABLE is a bounded read window, not proof that the game exited or that all logging completed.",
    "New runtime identities are self-reported startup observations, not memory attestation. ASI and package digests are required.",
    "Log span is not measured active gameplay time. Missing or malformed counters are unknown, never zero.",
    "Upload counters describe native callbacks, not independent GPU frame capture; rebuilds and loaded third-party modules are not certified.",
    "User feedback is preserved verbatim with its stated source and selected run; the tool does not certify visuals or compatibility.",
    "Text inventories are entry observations, not confirmed on-screen untranslated text; no dictionary changes are made.",
    "Earlier crashes and other runs remain separate evidence. Clean counters do not resolve an earlier failure's cause.",
]


def file_state(path):
    path = installation.checked_path(path)
    try:
        info = path.stat()
    except FileNotFoundError:
        return None
    if not stat.S_ISREG(info.st_mode):
        raise ValueError(f"Expected a regular evidence file: {path}")
    return dict(
        device=info.st_dev, inode=info.st_ino, bytes=info.st_size, mtime_ns=info.st_mtime_ns
    )


def copy_snapshot(source, target):
    before = file_state(source)
    if before is None:
        return dict(state="MISSING", source=str(source), captured=None)
    digest, count = hashlib.sha256(), 0
    try:
        with source.open("rb") as reader, target.open("wb") as writer:
            for block in iter(lambda: reader.read(1024 * 1024), b""):
                writer.write(block)
                digest.update(block)
                count += len(block)
        after = file_state(source)
        stable = before == after and count == before["bytes"]
    except FileNotFoundError:
        after, stable = None, False
    captured = dict(bytes=count, sha256=digest.hexdigest()) if target.exists() else None
    if captured and installation.file_observation(target)["fingerprint"] != captured:
        raise ValueError(f"Archived copy digest mismatch: {target}")
    return dict(
        state="STABLE" if stable else "CHANGED_DURING_READ",
        source=str(source),
        before=before,
        after=after,
        captured=captured,
    )


def snapshot(inputs, stage):
    """Retry the selected set together so a late crash write invalidates the run copy."""
    history = []
    for attempt in range(1, 4):
        files = {}
        for relative, source in inputs.items():
            target = stage / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.exists():
                target.unlink()
            files[relative] = copy_snapshot(source, target)
        stable = True
        for relative, source in inputs.items():
            record = files[relative]
            unchanged = file_state(source) == record.get("after")
            if record["state"] == "MISSING":
                unchanged = file_state(source) is None
            elif record["state"] == "STABLE":
                try:
                    observed = (
                        installation.file_observation(source)["fingerprint"] if unchanged else None
                    )
                except FileNotFoundError:
                    observed = None
                except ValueError as error:
                    if not str(error).startswith("File changed during inspection:"):
                        raise
                    observed = None
                unchanged = unchanged and observed == record["captured"]
            if not unchanged or record["state"] == "CHANGED_DURING_READ":
                record["state"] = "CHANGED_DURING_READ"
                stable = False
        history.append(
            dict(attempt=attempt, states={name: row["state"] for name, row in files.items()})
        )
        if stable:
            return dict(state="STABLE", attempts=history, files=files)
        if attempt != 3:
            time.sleep(0.05)
    return dict(state="INCOMPLETE", attempts=history, files=files)


def summarize_run(path, run, expected_hosts, expected_package):
    issues, times = [], []
    try:
        raw = path.read_bytes()
        content = raw.decode("utf-8-sig")
    except UnicodeError:
        return dict(state="UNREADABLE_UTF8"), [dict(code="RUN_INVALID_UTF8")]
    if raw and not raw.endswith(b"\n"):
        issues.append(dict(code="RUN_TRAILING_PARTIAL_LINE"))
    starts, hosts, resource, text, warnings, unknown = [], [], [], [], [], []
    atlas, fonts, hooks, active, inventory = [], [], [], [], []
    package_identities, asi_identities = [], []
    resource_last_valid, text_last_valid = False, False
    for number, line in enumerate(content.splitlines(), 1):
        if not line.strip():
            continue
        match = LINE.fullmatch(line)
        if not match:
            unknown.append(number)
            continue
        timestamp, level, message = match.groups()
        try:
            times.append(datetime.strptime(timestamp, "%Y-%m-%d %H:%M:%S"))
        except ValueError:
            unknown.append(number)
        row = dict(line=number, timestamp=timestamp, message=message)
        if level in {"WARN", "ERR", "FATAL"}:
            warnings.append(dict(**row, level=level))
        if start := START.fullmatch(message):
            starts.append(dict(**row, pid=int(start[1]), run=start[2]))
        if host := HOST.match(message):
            hosts.append(dict(**row, exe=host[1], crc32=host[2].upper()))
        if message.startswith("[RTP] language="):
            match = re.fullmatch(
                r"\[RTP\] language=([a-z][a-z0-9]*(?:-[a-z0-9]+)*) embedded_digest=([0-9a-f]{64}) package_version=2 dictionary_rules=2",
                message,
            )
            if match:
                package_identities.append(dict(language_id=match[1], embedded_digest=match[2]))
            else:
                issues.append(dict(code="PACKAGE_IDENTITY_UNRECOGNIZED", line=number))
        if message.startswith("[RTP] asi_sha256="):
            match = re.fullmatch(r"\[RTP\] asi_sha256=([0-9a-f]{64})", message)
            if match:
                asi_identities.append(match[1])
            else:
                issues.append(dict(code="ASI_IDENTITY_UNRECOGNIZED", line=number))
        if message.startswith("[状态] RTP "):
            fields = message.removeprefix("[状态] RTP ").split()
            valid = all(re.fullmatch(r"[a-z_]+=[0-9]+", item) for item in fields)
            values = dict(item.split("=") for item in fields) if valid else {}
            valid = (
                valid
                and len(values) == len(fields)
                and set(values) == set(RTP_FIELDS + FONT_TRACK_FIELDS)
            )
            if valid:
                values = {key: int(value) for key, value in values.items()}
                valid = (
                    values["font_active"] <= values["font_peak"] <= values["font_limit"]
                    and values["font_limit"] > 0
                )
            resource_last_valid = valid
            if valid:
                resource.append(dict(**row, fields=values))
            else:
                issues.append(dict(code="RESOURCE_STATUS_UNRECOGNIZED", line=number))
        if message.startswith("[状态] FUNNEL "):
            text_last_valid = False
            if counters := FUNNEL.fullmatch(message):
                text_last_valid = True
                fields = dict(zip(TEXT_FIELDS, map(int, counters.groups())))
                text.append(dict(**row, fields=fields))
            else:
                issues.append(dict(code="TEXT_STATUS_UNRECOGNIZED", line=number))
        if message.startswith("[RTP] atlas="):
            atlas.append(row)
        if message.startswith("[RTP] font="):
            fonts.append(row)
        if "[钩] RTP_" in message and "已装 ✓" in message:
            hooks.append(row)
        if message.startswith("[RTP] single package active "):
            active.append(row)
            if not re.fullmatch(
                r"\[RTP\] single package active dictionary_keys=\d+ text_replace=1", message
            ):
                issues.append(
                    dict(
                        code="PACKAGE_ACTIVATION_UNRECOGNIZED_OR_REPLACEMENT_DISABLED", line=number
                    )
                )
        if message.startswith("[INV] 字符串全记录："):
            values = re.search(r"唯一 (\d+)（丢弃 (\d+)）", message)
            if values:
                inventory.append(dict(**row, unique=int(values[1]), dropped=int(values[2])))
            else:
                issues.append(dict(code="INVENTORY_STATUS_UNRECOGNIZED", line=number))
    if len(starts) != 1 or starts[0]["run"] != run:
        issues.append(dict(code="RUN_IDENTITY_MISSING_OR_MIXED"))
    observed_hosts = {(row["exe"], row["crc32"]) for row in hosts}
    host_matches = len(observed_hosts) == 1 and observed_hosts <= {
        (host["exe"], host["crc32"]) for host in expected_hosts
    }
    if not host_matches:
        issues.append(dict(code="LOG_HOST_MISSING_OR_MISMATCH"))
    if not active:
        issues.append(dict(code="PACKAGE_ACTIVATION_NOT_OBSERVED"))
    if unknown:
        issues.append(dict(code="UNPARSED_LOG_LINES", lines=unknown))
    if warnings:
        issues.append(dict(code="WARNING_OR_ERROR_LINES", count=len(warnings)))
    errors = []
    for kind, samples, keys in (
        ("resource", resource, ("rejects", "faults", "font_failed", "font_full")),
        ("text", text, ("capacity_failures", "exceptions")),
    ):
        if not samples:
            issues.append(dict(code=kind.upper() + "_STATUS_MISSING"))
        for row in samples:
            nonzero = {key: row["fields"][key] for key in keys if row["fields"].get(key)}
            if nonzero:
                errors.append(dict(kind=kind, line=row["line"], counters=nonzero))
    if errors:
        issues.append(dict(code="NONZERO_ERROR_COUNTERS", samples=len(errors)))
    if any(row["dropped"] for row in inventory):
        issues.append(dict(code="INVENTORY_RECORDS_DROPPED"))
    backwards = any(after < before for before, after in zip(times, times[1:]))
    if backwards:
        issues.append(dict(code="LOG_CLOCK_MOVED_BACKWARD"))
    expected_identity = {key: expected_package[key] for key in ("language_id", "embedded_digest")}
    package_matches = package_identities == [expected_identity]
    asi_matches = asi_identities == [expected_package["asi_sha256"]]
    if not package_matches:
        issues.append(dict(code="LOG_PACKAGE_IDENTITY_MISSING_OR_MISMATCH"))
    if not asi_matches:
        issues.append(dict(code="LOG_ASI_IDENTITY_MISSING_OR_MISMATCH"))
    identity_state = (
        "REPORTED_MATCHES" if package_matches and asi_matches else "MISSING_OR_MISMATCH"
    )
    return dict(
        state="PARSED",
        first_log_at=str(min(times)) if times else None,
        last_log_at=str(max(times)) if times else None,
        observed_log_span_seconds=int((max(times) - min(times)).total_seconds()) if times else None,
        active_play_seconds="NOT_MEASURED",
        run_completion="NOT_PROVEN",
        starts=starts,
        hosts=hosts,
        declared_host_matches=host_matches,
        resources=dict(samples=len(resource), last=resource[-1] if resource_last_valid else None),
        text=dict(samples=len(text), last=text[-1] if text_last_valid else None),
        warnings=warnings,
        nonzero_error_samples=errors,
        inventory_status=inventory,
        atlas_publications=atlas,
        font_appends=fonts,
        installed_resource_hooks=hooks,
        package_activation=active,
        binary_pair_identity=identity_state,
        package_identities=package_identities,
        asi_identities=asi_identities,
        selected_host=next(iter(observed_hosts)) if host_matches else None,
        loaded_modules="NOT_OBSERVED",
        resource_rebuild="NOT_CERTIFIED",
        gpu_frame="NOT_CAPTURED",
        visuals="NOT_ASSESSED_BY_TOOL",
    ), issues


def feedback_record(path, run):
    value = candidate.read_json(path)
    valid = (
        isinstance(value, dict)
        and value.get("format_version") == 1
        and value.get("kind") == "AC1_RUNTIME_USER_FEEDBACK"
        and isinstance(value.get("observations"), list)
    )
    if not valid:
        raise ValueError("Feedback must be a format-1 AC1_RUNTIME_USER_FEEDBACK record")
    selected = []
    for row in value["observations"]:
        if not isinstance(row, dict):
            raise ValueError("Invalid feedback observation")
        if any(
            not isinstance(row.get(key), str) or not row[key].strip()
            for key in ("run", "text", "source")
        ):
            raise ValueError("Feedback observations need run, text and stated source")
        if row["run"] == run:
            selected.append({key: row[key] for key in ("run", "text", "source")})
    if not selected:
        raise ValueError("Feedback contains no observation for the selected run")
    return selected


def deployment_reference(stage, formal, target):
    if not formal:
        return dict(kind="NOT_PROVIDED", verification="NOT_PROVEN"), []
    _, transaction, result = installation.deployment_record(stage / "references/deployment")
    bound = transaction["binding"]
    matches = (
        bound["candidate"]["manifest"] == target["manifest"]
        and bound["candidate"]["hosts"] == target["hosts"]
        and {name: value for name, value in bound["after"].items() if value is not None}
        == target["files"]
    )
    if not matches:
        raise ValueError("Deployment receipt does not target the selected candidate")
    status = result["status"] if result else "INTERRUPTED"
    issues = [] if status == "DEPLOYED" else [dict(code="DEPLOYMENT_NOT_COMPLETED", status=status)]
    return dict(
        kind="FORMAL_RECEIPT", verification="DECLARED_TARGET_MATCHES", recorded_result=status
    ), issues


def make_audit(source, stage, out, run, summary):
    inventory = stage / "logs" / f"strings_{run}.txt"
    if not inventory.exists():
        return dict(status="MISSING_INVENTORY"), []
    try:
        observed, _ = text_audit.read_inventory(inventory, 0)
        pids = {
            int(match[1])
            for header in observed["run_headers"]
            if (match := re.search(r" pid=(\d+) ", header))
        }
        expected = {row["pid"] for row in summary.get("starts", [])}
        header_time = re.match(
            r"# === run (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d) ", observed["run_headers"][0]
        )
        first, last = summary.get("first_log_at"), summary.get("last_log_at")
        time_matches = header_time and first and last and first <= header_time[1] <= last
        correct_run = len(summary.get("starts", [])) == 1 and summary["starts"][0]["run"] == run
        if (
            len(observed["run_headers"]) != 1
            or not correct_run
            or pids != expected
            or not time_matches
        ):
            return dict(status="UNAVAILABLE"), [dict(code="INVENTORY_RUN_IDENTITY_MISMATCH")]
        report = text_audit.build_report(source, [inventory], stage / "references/notranslate.txt")
    except (ValueError, UnicodeError) as error:
        return dict(status="UNAVAILABLE", error=str(error).replace(str(stage), str(out))), [
            dict(code="INVENTORY_AUDIT_UNAVAILABLE")
        ]
    report["inventories"][0]["path"] = str(out / "logs" / inventory.name)
    report["notranslate"]["path"] = str(out / "references/notranslate.txt")
    folder = stage / "text-audit"
    folder.mkdir()
    candidate.write_json(folder / "audit.json", report)
    (folder / "audit.md").write_text(text_audit.markdown(report), encoding="utf-8", newline="\n")
    return dict(
        status=report["status"], path="text-audit/audit.json", summary=report["summary"]
    ), []


def markdown(report):
    summary = report["run"]["observations"]
    lines = [
        "# 运行时证据归档",
        "",
        f"运行：`{report['run']['id']}`；归档状态：`{report['status']}`。",
        f"候选：`{report['candidate']['version']}`；观测宿主：`{summary.get('selected_host', '未知')}`。",
        "",
        "归档分别保存日志、部署引用和人工反馈；不自动签发实机通过结论。",
        "",
        f"快照：`{report['snapshot']['state']}`；部署关联：`{report['deployment']['verification']}`。",
        f"日志跨度：{summary.get('observed_log_span_seconds', '未知')} 秒；活跃游玩时长未测量。",
        "",
        "| 观测 | 数值/状态 |",
        "| --- | --- |",
    ]
    for key, label in (("resources", "资源状态样本"), ("text", "文本状态样本")):
        item = summary.get(key, {})
        lines.append(f"| {label} | {item.get('samples', '未知')} |")
    last = summary.get("text", {}).get("last")
    lines.append(f"| 最后替换计数 | {last['fields']['replaced'] if last else '未知'} |")
    lines.extend(["", "## 人工反馈（原文）", ""])
    for row in report["feedback"]["observations"]:
        lines.append("- " + row["text"].replace("\n", "<br>"))
        lines.append("  来源：" + row["source"].replace("\n", "<br>"))
    if not report["feedback"]["observations"]:
        lines.append("本次未提供对应运行的人工反馈。")
    lines.extend(["", "## 需关注的证据状态", ""])
    lines.extend("- `" + row["code"] + "`" for row in report["issues"])
    if not report["issues"]:
        lines.append("本次已解析日志未列出错误；运行完成、页面覆盖和实际二进制配对仍未由工具证明。")
    lines.extend(["", "## 解释范围", ""])
    lines.extend("- " + item for item in report["limits"])
    lines.extend(["", "原始快照、来源摘要、逐行计数及未译观测审计见 `archive.json`。", ""])
    return "\n".join(lines)


def archive_run(source, logs, run, out, *, policy, feedback=None, receipt=None):
    if not re.fullmatch(r"\d{8}_\d{6}", run):
        raise ValueError("Run must use YYYYMMDD_HHMMSS")
    datetime.strptime(run, "%Y%m%d_%H%M%S")
    source, logs = (installation.checked_path(path) for path in (source, logs))
    if not logs.is_dir():
        raise ValueError("Logs must be an existing directory")
    manifest = candidate.verify(source)
    if policy is None:
        raise ValueError(
            "Language run archives require an explicit --notranslate policy (an empty file is allowed)"
        )
    policy = installation.checked_path(policy)
    feedback = installation.checked_path(feedback) if feedback is not None else None
    receipt = installation.checked_path(receipt) if receipt is not None else None
    roots = [source, logs, policy.parent]
    roots += [
        path if path.is_dir() else path.parent for path in (feedback, receipt) if path is not None
    ]
    # Protect an actual installation even when --logs points deep inside it.
    for parent in (logs, *logs.parents):
        if any((parent / host["exe"]).exists() for host in candidate.HOSTS.values()):
            roots.append(parent)
    out = installation.output_directory(out, *roots)
    package_name = candidate.PACKAGE
    target = dict(
        path=str(source),
        version=manifest["version"],
        manifest=installation.file_observation(source / "manifest.json")["fingerprint"],
        files={
            name: manifest["files"]["install/scripts/" + name]
            for name in (candidate.ASI, package_name)
        },
    )
    raw = (source / "install/scripts" / package_name).read_bytes()
    target.update(
        hosts=manifest["hosts"],
        language_id=manifest["language_id"],
        package_identity=dict(
            language_id=manifest["language_id"],
            embedded_digest=raw[64:96].hex(),
            asi_sha256=target["files"][candidate.ASI]["sha256"],
        ),
    )
    inputs = {
        f"logs/{kind}_{run}.{extension}": logs / f"{kind}_{run}.{extension}"
        for kind, extension in (("run", "log"), ("strings", "txt"), ("crash", "log"))
    }
    if not any(file_state(path) is not None for path in inputs.values()):
        raise ValueError("No evidence files for the selected run")
    inputs["references/notranslate.txt"] = policy
    if feedback:
        inputs["references/feedback.json"] = feedback
    if receipt:
        if not receipt.is_dir():
            raise ValueError("Formal receipt must be a deployment directory")
        for name in (
            "transaction.json",
            "transaction.json.sha256",
            "inspection.json",
            "result.json",
            "result.json.sha256",
        ):
            inputs["references/deployment/" + name] = receipt / name
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".runtime-archive-", dir=out.parent) as temporary:
        stage = Path(temporary) / "archive"
        stage.mkdir()
        captured = snapshot(inputs, stage)
        issues = []
        for name, row in captured["files"].items():
            if row["state"] == "MISSING" and name.startswith("logs/"):
                issues.append(dict(code="LOG_FILE_MISSING", path=name))
            row["archive_path"] = name if row["captured"] else None
        if captured["state"] == "STABLE":
            # Missing non-log references are invalid input; a missing formal
            # result alone is a valid interrupted transaction.
            for name, row in captured["files"].items():
                optional_result = name in {
                    "references/deployment/result.json",
                    "references/deployment/result.json.sha256",
                }
                if (
                    row["state"] == "MISSING"
                    and name.startswith("references/")
                    and not optional_result
                ):
                    raise ValueError(f"Missing reference: {row['source']}")
            selected_feedback = (
                feedback_record(stage / "references/feedback.json", run) if feedback else []
            )
            linked, extra = deployment_reference(stage, receipt, target)
            issues.extend(extra)
            run_log = stage / "logs" / f"run_{run}.log"
            summary, extra = (
                summarize_run(
                    run_log,
                    run,
                    target["hosts"],
                    target["package_identity"],
                )
                if run_log.exists()
                else (dict(state="MISSING"), [])
            )
            issues.extend(extra)
            crash = captured["files"][f"logs/crash_{run}.log"]
            if crash["captured"] and crash["captured"]["bytes"]:
                issues.append(dict(code="CRASH_LOG_NONEMPTY", bytes=crash["captured"]["bytes"]))
            audited, extra = make_audit(source, stage, out, run, summary)
            issues.extend(extra)
        else:
            issues.append(dict(code="SNAPSHOT_CHANGED_DURING_READ"))
            selected_feedback, linked = (
                [],
                dict(kind="NOT_ASSESSED", verification="INCOMPLETE_SNAPSHOT"),
            )
            summary, audited = dict(state="NOT_ASSESSED"), dict(status="NOT_ASSESSED")
        candidate.verify(source)
        if (
            installation.file_observation(source / "manifest.json")["fingerprint"]
            != target["manifest"]
        ):
            raise ValueError("Candidate changed during archive collection")
        status = "ARCHIVED_WITH_ATTENTION" if issues else "ARCHIVED"
        if captured["state"] != "STABLE":
            status = "INCOMPLETE_SNAPSHOT"
        report = dict(
            format_version=2,
            kind="AC1_RUNTIME_RUN_ARCHIVE",
            status=status,
            collected_at_utc=datetime.now(timezone.utc).isoformat(),
            native="NOT_CERTIFIED_BY_TOOL",
            candidate=target,
            deployment=linked,
            snapshot=captured,
            run=dict(id=run, source_logs=str(logs), observations=summary),
            feedback=dict(observations=selected_feedback),
            text_audit=audited,
            issues=issues,
            limits=LIMITS,
            tool_sources={
                name: candidate.fingerprint(ROOT / "tools/modules" / name)
                for name in (
                    "runtime_archive.py",
                    "audit_runtime_text.py",
                    "_dictionary.py",
                    "runtime_candidate.py",
                    "runtime_install.py",
                )
            },
        )
        candidate.write_json(stage / "archive.json", report)
        (stage / "archive.md").write_text(markdown(report), encoding="utf-8", newline="\n")
        names = sorted(path for path in stage.rglob("*") if path.is_file())
        (stage / "SHA256SUMS").write_text(
            "".join(
                f"{candidate.fingerprint(path)['sha256']}  {path.relative_to(stage).as_posix()}\n"
                for path in names
            ),
            encoding="ascii",
        )
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


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--logs", type=Path, required=True)
    parser.add_argument("--run", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--feedback", type=Path)
    parser.add_argument("--deployment", type=Path, help="Current formal deployment directory")
    parser.add_argument("--notranslate", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        report = archive_run(
            args.candidate,
            args.logs,
            args.run,
            args.out,
            feedback=args.feedback,
            receipt=args.deployment,
            policy=args.notranslate,
        )
    except (OSError, ValueError, UnicodeError, KeyError, TypeError, AttributeError) as error:
        print(f"Run archive rejected: {error}", file=sys.stderr)
        return 2
    print(
        json.dumps(
            dict(
                status=report["status"],
                run=report["run"]["id"],
                snapshot=report["snapshot"]["state"],
                issues=len(report["issues"]),
            ),
            ensure_ascii=False,
        )
    )
    return 0 if report["status"] == "ARCHIVED" else 1


if __name__ == "__main__":
    sys.exit(main())
