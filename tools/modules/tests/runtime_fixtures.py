"""Valid current candidates shared by delivery, deployment and archive tests."""

import json

import _runtime_inputs as inputs
import runtime_candidate as candidate
import test_language_package as packages


def language_candidate(folder, language="zh", hosts=None):
    folder.mkdir(parents=True, exist_ok=True)
    hosts = hosts or list(candidate.HOSTS.values())
    baseline_hosts = {name: host for name, host in candidate.HOSTS.items() if host in hosts}
    for name in candidate.CONTENT_FILES:
        file = folder / name
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text("{}", encoding="utf-8")
    runtime = folder / "evidence/runtime"
    binary = b"\n".join(
        (
            candidate.MARKER,
            candidate.STARTUP_MARKER,
            candidate.HOST_ADAPTER_MARKER,
            candidate.LANGUAGE_MARKER,
        )
    )
    for file in (runtime / candidate.ASI, folder / "install/scripts" / candidate.ASI):
        file.write_bytes(binary)
    sources = candidate.native_sources()
    oracle = dict(
        format_version=1,
        target="x86",
        exe_sha256=candidate.fingerprint(runtime / "dictionary_oracle.exe")["sha256"],
        sources={name: sources[name]["sha256"] for name in inputs.ORACLE_SOURCES},
    )
    (runtime / "dictionary_oracle.json").write_text(json.dumps(oracle), encoding="utf-8")
    receipt = dict(
        format_version=1,
        version="0.2.0-rc.1",
        status="RUNTIME_OFFLINE_PASS",
        native="NOT_TESTED_BY_TOOL",
        package_version=2,
        dictionary_rules_version=2,
        hosts=candidate.HOSTS,
        sources=sources,
        native_tests={},
        pe={},
        files={
            name: candidate.fingerprint(runtime / name) for name in candidate.RUNTIME_CONTENT_FILES
        },
    )
    (runtime / "runtime-build.json").write_text(json.dumps(receipt), encoding="utf-8")
    raw, _ = packages.synthetic_package(language)
    (folder / "install/scripts" / candidate.PACKAGE).write_bytes(raw)
    evidence = folder / "evidence/package"
    (evidence / "baseline.json").write_text(
        json.dumps(dict(id="fixture", hosts=baseline_hosts)), encoding="utf-8"
    )
    (evidence / "profile.json").write_text(
        json.dumps(dict(language_id=language, baseline="fixture")), encoding="utf-8"
    )
    (evidence / "dictionary-effective.json").write_text(
        json.dumps({"BACK": "戻る", "START": "始める"}), encoding="utf-8"
    )
    report = dict(
        status="OFFLINE_PASS",
        package_version=2,
        language_id=language,
        package_bytes=len(raw),
        package_sha256=inputs.sha(raw),
        embedded_digest=raw[64:96].hex(),
        font_sha256="0" * 64,
        oracle_sha256=oracle["exe_sha256"],
        baseline_id="fixture",
        baseline_sha256=candidate.fingerprint(evidence / "baseline.json")["sha256"],
        profile_sha256=candidate.fingerprint(evidence / "profile.json")["sha256"],
        dictionary=dict(rules_version=2, stats=dict(keys=2)),
    )
    for name in ("build-report.json", "verify-report.json"):
        (evidence / name).write_text(json.dumps(report), encoding="utf-8")
    manifest = dict(
        format_version=4,
        version="0.2.0-rc.1",
        status="CANDIDATE_OFFLINE_PASS",
        mode=candidate.MODE,
        native="NOT_TESTED_BY_TOOL",
        language_id=language,
        hosts=hosts,
        runtime_version=receipt["version"],
        native_tests={},
        pe={},
    )
    seal_candidate(folder, manifest)
    candidate.verify(folder)
    return folder


def seal_candidate(folder, manifest=None):
    manifest = manifest or candidate.read_json(folder / "manifest.json")
    manifest["files"] = {
        name: candidate.fingerprint(folder / name) for name in candidate.CONTENT_FILES
    }
    (folder / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    names = sorted(candidate.CONTENT_FILES | {"manifest.json"})
    (folder / "SHA256SUMS").write_text(
        "".join(f"{candidate.fingerprint(folder / name)['sha256']}  {name}\n" for name in names),
        encoding="ascii",
    )
