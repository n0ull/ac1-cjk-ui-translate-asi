"""Audit captured text against a verified runtime candidate without editing game data."""

import argparse
import hashlib
import json
import re
import sys
import tempfile
from collections import Counter
from pathlib import Path

import _dictionary as dictionary_text
import runtime_candidate

ROOT = Path(__file__).resolve().parents[2]
INVENTORY_UNITS = 48
ENTRY = re.compile(
    r"\[×([0-9]+)\]\s+(替换|同值|命中|未命中·非ASCII|未命中|未知)\s+caller=([0-9A-Fa-f]{8}) (.+)"
)
CATEGORIES = {
    "replaced": "完整短键：记录过替换",
    "in_dictionary": "完整短键：词典已有，未记录替换",
    "unchanged": "完整短键：命中同值，未写入",
    "excluded": "完整短键：已有不译裁定",
    "no_letters": "完整短键：没有字母（数字、符号等）",
    "review": "完整短键：待核实用途",
    "prefix_only": "48 码元：可能截断，不能按完整键判断",
}
LIMITS = [
    "清单记录的是字符串处理入口，不是上屏证据；待核实不等于玩家可见的漏译。",
    "48 个 UTF-16 码元可能是完整键，也可能是截断前缀；相同前缀在原生表中会合并。",
    "count_snapshot 是首次刷盘时的快照，不是本轮最终频次；状态可能在刷盘后变化。",
    "caller 是每条原生记录的首次调用者；跨文件汇总不代表全部调用点。",
    "清单未携带包摘要；候选是调用者指定的比较基准，工具不能证明日志与包配对。",
    "译文长于规范键只表示容量候选；清单没有原串长度/Myres，不能据此判定容量不足。",
    "同包前缀候选和大小写候选仅供审阅；运行时仍执行完整键的精确匹配。",
]


def utf16_units(text):
    return len(text.encode("utf-16-le")) // 2


def read_inventory(path, index):
    raw = path.read_bytes()
    headers, rows = [], []
    for number, line in enumerate(raw.decode("utf-8-sig").splitlines(), 1):
        if not line.strip():
            continue
        if line.startswith("#"):
            if line.startswith("# === run ") and line.endswith(" ==="):
                headers.append(line)
            continue
        match = ENTRY.fullmatch(line)
        if not match or not headers:
            raise ValueError(
                "%s:%d: malformed inventory entry or missing run header" % (path, number)
            )
        count, status, caller, text = match.groups()
        valid_count = 1 <= int(count) <= 65535
        valid_length = utf16_units(text) <= INVENTORY_UNITS
        if not valid_count or not valid_length:
            raise ValueError("%s:%d: entry exceeds the legacy inventory limits" % (path, number))
        rows.append(
            (
                text,
                dict(
                    inventory=index,
                    line=number,
                    run_header=headers[-1],
                    count_snapshot=int(count),
                    status=status,
                    caller=caller.upper(),
                ),
            )
        )
    if not rows:
        raise ValueError("%s: inventory has no entries" % path)
    source = dict(
        path=str(path),
        bytes=len(raw),
        sha256=hashlib.sha256(raw).hexdigest(),
        run_headers=headers,
        rows=len(rows),
    )
    return source, rows


def exclusion(text, rules):
    for pattern, reason in rules:
        is_prefix = pattern.endswith("*") and text.startswith(pattern[:-1])
        is_exact = text == dictionary_text.canon(pattern)
        if is_prefix or is_exact:
            return dict(pattern=pattern, reason=reason)
    return None


def build_report(candidate, inventories, policy):
    manifest = runtime_candidate.verify(candidate)
    effective = candidate / "evidence/package/dictionary-effective.json"
    dictionary = runtime_candidate.read_json(effective)
    if not isinstance(dictionary, dict) or not dictionary:
        raise ValueError("Effective dictionary must be a nonempty object")
    for key, value in dictionary.items():
        if not isinstance(key, str) or not isinstance(value, str) or not key or not value:
            raise ValueError("Effective dictionary must contain nonempty string pairs")
    build = runtime_candidate.read_json(candidate / "evidence/package/build-report.json")
    if build.get("dictionary", {}).get("stats", {}).get("keys") != len(dictionary):
        raise ValueError("Effective dictionary count differs from the production receipt")

    # Reject broken UTF-8 before the legacy policy loader can replace bad bytes.
    policy.read_bytes().decode("utf-8-sig")
    rules = dictionary_text.load_notranslate(policy)
    sources, entries = [], {}
    for index, path in enumerate(inventories):
        source, rows = read_inventory(path, index)
        sources.append(source)
        for text, observation in rows:
            entry = entries.setdefault(
                text, dict(text=text, utf16_units=utf16_units(text), observations=[])
            )
            entry["observations"].append(observation)

    case_variants = {}
    prefixes = {}
    for key in sorted(dictionary):
        case_variants.setdefault(key.casefold(), []).append(key)
        # The native inventory truncates UTF-16, rather than Python code points.
        prefix = key.encode("utf-16-le")[: INVENTORY_UNITS * 2]
        prefixes.setdefault(prefix, []).append(key)

    for text, entry in entries.items():
        entry["statuses"] = sorted({row["status"] for row in entry["observations"]})
        entry["callers"] = sorted({row["caller"] for row in entry["observations"]})
        if entry["utf16_units"] == INVENTORY_UNITS:
            entry["category"] = "prefix_only"
            entry["dictionary_prefix_candidates"] = prefixes.get(text.encode("utf-16-le"), [])
            continue
        if text in dictionary:
            replaced = "替换" in entry["statuses"]
            if replaced:
                entry["category"] = "replaced"
            elif "同值" in entry["statuses"]:
                entry["category"] = "unchanged"
            else:
                entry["category"] = "in_dictionary"
            entry["translation"] = dictionary[text]
            continue
        rule = exclusion(text, rules)
        if rule:
            entry["category"] = "excluded"
            entry["exclusion"] = rule
            continue
        entry["category"] = "review" if any(c.isalpha() for c in text) else "no_letters"
        entry["case_candidates"] = case_variants.get(text.casefold(), [])

    capacity = []
    for key, value in sorted(dictionary.items()):
        key_units, value_units = utf16_units(key), utf16_units(value)
        if value_units <= key_units:
            continue
        entry = entries.get(key, {})
        complete = entry.get("utf16_units", INVENTORY_UNITS) < INVENTORY_UNITS
        capacity.append(
            dict(
                key=key,
                translation=value,
                key_units=key_units,
                value_units=value_units,
                capacity="UNKNOWN",
                complete_key_statuses=entry.get("statuses", []) if complete else [],
            )
        )

    counts = Counter(entry["category"] for entry in entries.values())
    return dict(
        format_version=2,
        dictionary_rules_version=2,
        status="AUDIT_COMPLETE",
        native="NOT_ASSESSED_BY_TOOL",
        candidate=dict(
            path=str(candidate),
            version=manifest["version"],
            manifest=runtime_candidate.fingerprint(candidate / "manifest.json"),
            package=manifest["files"]["install/scripts/" + runtime_candidate.PACKAGE],
        ),
        inventories=sources,
        notranslate=dict(path=str(policy), **runtime_candidate.fingerprint(policy)),
        tool_sources={
            path.name: runtime_candidate.fingerprint(path)
            for path in (
                Path(__file__).resolve(),
                ROOT / "tools/modules/_dictionary.py",
                ROOT / "tools/modules/runtime_candidate.py",
            )
        },
        limits=LIMITS,
        summary=dict(
            dictionary_keys=len(dictionary),
            unique_records=len(entries),
            categories={name: counts[name] for name in CATEGORIES},
            capacity_candidates=len(capacity),
        ),
        capacity_candidates=capacity,
        entries=[entries[text] for text in sorted(entries)],
    )


def markdown(report):
    def cell(value):
        return (
            str(value)
            .replace("&", "&amp;")
            .replace("<", "&lt;")
            .replace(">", "&gt;")
            .replace("|", "&#124;")
            .replace("\n", "<br>")
        )

    summary = report["summary"]
    lines = [
        "# 运行时文本离线审计",
        "",
        "候选 `%s`；%d 份清单、%d 条去重记录、生产有效键 %d。"
        % (
            report["candidate"]["version"],
            len(report["inventories"]),
            summary["unique_records"],
            summary["dictionary_keys"],
        ),
        "本报告不修改词典或安装，也不作完整 UI/容量/稳定性验收。",
        "",
        "| 记录分类 | 条数 |",
        "| --- | ---: |",
    ]
    for name, title in CATEGORIES.items():
        lines.append("| %s | %d |" % (title, summary["categories"][name]))
    lines.extend(["", "## 证据边界", ""])
    lines.extend("- " + limit for limit in report["limits"])
    lines.extend(
        [
            "",
            "## 完整短键待核实",
            "",
            "包含按键、占位文和内部菜单；先确认上屏页面，再决定是否补译。",
            "",
            "| 原文 | 首次调用者（各清单） | 大小写候选（不自动使用） |",
            "| --- | --- | --- |",
        ]
    )
    for entry in report["entries"]:
        if entry["category"] != "review":
            continue
        callers = ", ".join(entry["callers"])
        candidates = cell(", ".join(entry["case_candidates"]))
        lines.append("| %s | %s | %s |" % (cell(entry["text"]), callers, candidates))
    lines.extend(
        [
            "",
            "## 译文长于规范键的容量候选",
            "",
            "Myres 未记录，不能从长度差推断失败。短键的替换状态仅证明曾成功提交。",
            "",
            "| 原键 | 译文 | UTF-16 长度（键→值） | 完整短键记录状态 |",
            "| --- | --- | --- | --- |",
        ]
    )
    for entry in report["capacity_candidates"]:
        statuses = cell(", ".join(entry["complete_key_statuses"]))
        lines.append(
            "| %s | %s | %d→%d | %s |"
            % (
                cell(entry["key"]),
                cell(entry["translation"]),
                entry["key_units"],
                entry["value_units"],
                statuses,
            )
        )
    lines.extend(["", "全部记录、前缀候选、逐文件行号与输入/工具摘要见 `audit.json`。", ""])
    return "\n".join(lines)


def audit(candidate, inventories, policy, out):
    candidate, policy, out = candidate.resolve(), policy.resolve(), out.resolve()
    inventories = [path.resolve() for path in inventories]
    if out == candidate or candidate in out.parents:
        raise ValueError("Audit output must be outside the immutable candidate")
    if out.exists():
        raise FileExistsError("Audit output already exists: %s" % out)
    if len(set(inventories)) != len(inventories):
        raise ValueError("Do not count the same inventory file twice")
    report = build_report(candidate, inventories, policy)
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".text-audit-", dir=out.parent) as temporary:
        staged = Path(temporary) / "report"
        staged.mkdir()
        runtime_candidate.write_json(staged / "audit.json", report)
        (staged / "audit.md").write_text(markdown(report), encoding="utf-8", newline="\n")
        staged.rename(out)
    return report["summary"]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--inventory", type=Path, nargs="+", required=True)
    parser.add_argument("--notranslate", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        summary = audit(args.candidate, args.inventory, args.notranslate, args.out)
    except (OSError, UnicodeError, ValueError) as error:
        print("Text audit rejected: %s" % error, file=sys.stderr)
        return 2
    print(json.dumps(summary, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
