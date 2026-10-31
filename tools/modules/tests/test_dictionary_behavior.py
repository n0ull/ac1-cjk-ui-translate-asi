"""Compare the native byte loader and offline oracle with a frozen behavior baseline."""

import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
ROOT = TOOLS.parent.parent
sys.path.insert(0, str(TOOLS))
import _dictionary as dictionary_text  # noqa: E402
from _runtime_inputs import DEFAULT_ORACLE, effective_dictionary, oracle_provenance  # noqa: E402

FIXTURE = Path(__file__).with_name("fixtures") / "dictionary_behavior_v1.json"
DIAGNOSTICS = FIXTURE.with_name("dictionary_diagnostics_v1.json")
PROBE = ROOT / "modules/dict/tests/out/dictionary_probe.exe"
REVIEWED_DICTIONARY = ROOT / "data/languages/zh/dictionary.tsv"
PROBE_SOURCES = (
    "modules/dict/tests/dictionary_probe.cpp",
    "modules/dict/src/dict.cpp",
    "modules/core/src/file.cpp",
    "modules/core/src/str.cpp",
    "modules/dict/tests/build.ps1",
    "tools/modules/native-toolchain.ps1",
    "modules/dict/include/ac1/dict/dict.h",
    "modules/core/include/ac1/core/file.h",
    "modules/core/include/ac1/core/str.h",
)
FIELDS = (
    "accepted ready keys key_pool value_pool lines new duplicates comments empty bad "
    "key_bad value_bad pool_full key_full key_long value_long fallback_full error_logs"
).split()
ORACLE_FIELDS = (
    "keys key_pool value_pool lines new duplicates bad key_bad value_bad "
    "pool_full key_full key_long value_long fallback_full"
).split()
RETAINED_COUNTERS = set(
    "lines new overrides identical comments empty bad key_bad value_bad "
    "pool_full key_full key_long value_long fallback_full".split()
)

# Deliberate semantic changes. The v1 fixtures remain immutable reference data.
V2_REJECTIONS = {
    "last_duplicate_wins": ("duplicate_key", 2),
    "normalized_duplicate": ("duplicate_key", 2),
    "same_duplicate": ("duplicate_key", 2),
    "fallback_same_duplicate": ("duplicate_key", 2),
    "missing_tab": ("malformed_row", 2),
    "invalid_utf8_key_first": ("invalid_utf8_key", 1),
    "invalid_utf8_key_last": ("invalid_utf8_key", 2),
    "invalid_utf8_value_first": ("invalid_utf8_value", 1),
    "invalid_utf8_value_last": ("invalid_utf8_value", 2),
    "escaped_empty_value": ("empty_value", 2),
    "empty_duplicate_keeps_old": ("duplicate_key", 2),
    "empty_duplicate_before_valid": ("empty_value", 1),
    "identical_duplicate_pool_rollback": ("duplicate_key", 2),
    "override_value_pool_exhaustion": ("duplicate_key", 2),
}


def diagnostic_expectations():
    value = json.loads(DIAGNOSTICS.read_text(encoding="utf-8"))
    if (
        value["format_version"] != 1
        or digest(FIXTURE.read_bytes()) != value["frozen_behavior_sha256"]
    ):
        raise ValueError("Frozen dictionary behavior changed")
    return value


def expected_behavior(baseline, diagnostics, name):
    expected = baseline["production"] if name == "production" else baseline["cases"][name]
    counters = diagnostics["retained_counters"].get(name, {})
    if counters and (expected["state"]["accepted"] or not counters.keys() <= RETAINED_COUNTERS):
        raise ValueError(
            "Only rejected-attempt parsing counters may differ from the frozen behavior"
        )
    state = dict(expected["state"], **counters)
    del state["overrides"], state["identical"]
    state["duplicates"] = 0
    if not state["accepted"]:
        state["error_logs"] = (
            0 if name in ("empty_input", "reinit_empty_clears_old", "source_over_limit") else 1
        )
    return dict(
        **{key: value for key, value in expected.items() if key != "state"},
        state=state,
        diagnostics=diagnostics["production"]
        if name == "production"
        else diagnostics["cases"][name],
    )


def reviewed_production(baseline):
    source = REVIEWED_DICTIONARY
    raw = source.read_bytes()
    if digest(raw) != "c62d99b056c17b6f8a2807111827864d18ceda4bdb4e7dc28e25b7d217451c89":
        raise ValueError("Frozen production source changed")
    values = {
        key: dictionary_text.trim_value(value).replace("\u2026", "...")
        for key, value in dictionary_text.load_supplement(source).items()
    }
    if values_digest(values) != baseline["production"]["values_sha256"]:
        raise ValueError("Current production values differ from the reviewed baseline")
    return raw, values


def read_diagnostics(stdout):
    lines = [line for line in stdout.decode("utf-8").splitlines() if line.startswith("result=")]
    if len(lines) != 1:
        raise ValueError("Expected one dictionary result line")
    fields = dict(part.split("=", 1) for part in lines[0].split())
    return dict(
        result=fields["result"],
        reason=fields["reason"],
        rejected=int(fields["rejected"]),
        first_rejected_line=int(fields["first_rejected_line"]),
        failure_line=int(fields["failure_line"]),
    )


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def values_digest(values):
    canonical = json.dumps(values, ensure_ascii=True, sort_keys=True, separators=(",", ":"))
    return digest(canonical.encode("ascii"))


def probe_provenance(executable):
    receipt = json.loads(executable.with_suffix(".json").read_text(encoding="utf-8-sig"))
    if receipt.get("format_version") != 1 or receipt.get("exe_sha256") != digest(
        executable.read_bytes()
    ):
        raise ValueError("Dictionary probe executable changed; rebuild dict tests")
    sources = receipt["sources"]
    if set(sources) != set(PROBE_SOURCES):
        raise ValueError("Dictionary probe source set changed; rebuild dict tests")
    for name in PROBE_SOURCES:
        if digest((ROOT / name).read_bytes()) != sources[name]:
            raise ValueError("Stale dictionary probe source %s; rebuild dict tests" % name)


def corpus():
    good = b"Good\tok\n"
    cases = [
        ("basic", b"Key\tvalue\n", ["Key", "key", "Absent"]),
        ("bom", b"\xef\xbb\xbfKey\tvalue\r\n", ["Key"]),
        ("comment_and_hash_key", b"# comment\n# Key\tvalue\n", ["# Key"]),
        ("canonical_whitespace", b"  A  B \t value  here \r\n", ["A B", "A  B"]),
        ("escapes", b"Key\tA\\nB\\tC\\\\n\\x4E2D\n", ["Key"]),
        ("last_duplicate_wins", b"Key\told\nKey\tnew\n", ["Key"]),
        ("normalized_duplicate", b" A  B\told\nA B\tnew\n", ["A B"]),
        ("same_duplicate", b"Key\tsame\nKey\tsame\n", ["Key"]),
        ("fallback", "Key\t前…後\n".encode(), ["Key"]),
        ("fallback_same_duplicate", "Key\t…\nKey\t...\n".encode(), ["Key"]),
        ("empty_input", b"", ["Good"]),
        ("only_comments", b"# comment\r\n\n", ["Good"]),
        ("missing_tab", good + b"not a row\n", ["Good"]),
        ("invalid_utf8_key_first", b"\xff\tbad\n" + good, ["Good"]),
        ("invalid_utf8_key_last", good + b"\xff\tbad\n", ["Good"]),
        ("invalid_utf8_value_first", b"Bad\t\xff\n" + good, ["Good", "Bad"]),
        ("invalid_utf8_value_last", good + b"Bad\t\xff\n", ["Good", "Bad"]),
        ("escaped_empty_value", good + b"Key\t\\n\n", ["Good", "Key"]),
        ("empty_duplicate_keeps_old", b"Key\told\nKey\t\\n\n", ["Key"]),
        ("empty_duplicate_before_valid", b"Key\t\\n\nKey\tnew\n", ["Key"]),
        ("escaped_nul", good + b"Bad\t\\x0000\n", ["Good", "Bad"]),
        ("raw_nul", good + b"Bad\ta\0b\n", ["Good", "Bad"]),
        ("escaped_surrogate", good + b"Bad\t\\xD800\n", ["Good", "Bad"]),
        ("astral_scalar", good + "Bad\t😀\n".encode(), ["Good", "Bad"]),
        ("unsupported_control", good + b"Bad\t\x1f\n", ["Good", "Bad"]),
        ("unsupported_overridden_value", b"Key\t\\xD800\nKey\tnew\n", ["Key"]),
        ("cjk_key_skipped", good + "中文\tvalue\n".encode(), ["Good", "中文"]),
        ("maximum_key", ("K" * 4096 + "\tv\n").encode(), ["K" * 4096]),
        ("key_over_limit", good + ("K" * 4097 + "\tv\n").encode(), ["Good"]),
        ("maximum_value", b"Key\t" + b"v" * 8192 + b"\n", ["Key"]),
        ("value_over_limit", good + b"Key\t" + b"v" * 8193 + b"\n", ["Good", "Key"]),
        ("fallback_at_limit", ("Key\t" + "v" * 8189 + "…\n").encode(), ["Key"]),
        ("fallback_over_limit", good + ("Key\t" + "v" * 8190 + "…\n").encode(), ["Good", "Key"]),
        ("identical_duplicate_pool_rollback", (b"Key\t" + b"v" * 8192 + b"\n") * 20, ["Key"]),
        (
            "override_value_pool_exhaustion",
            b"".join(("Key\t" + str(i) + "v" * 8190 + "\n").encode() for i in range(20)),
            ["Key"],
        ),
        (
            "value_pool_at_limit",
            b"".join(("K%d\t" % i).encode() + b"v" * 8192 + b"\n" for i in range(16)),
            ["K0", "K15"],
        ),
        (
            "value_pool_exhaustion",
            b"".join(("K%d\t" % i).encode() + b"v" * 8192 + b"\n" for i in range(17)),
            ["K0", "K16"],
        ),
        (
            "key_pool_at_limit",
            b"".join((str(i).zfill(4) + "K" * 4092 + "\tv\n").encode() for i in range(48)),
            ["0000" + "K" * 4092],
        ),
        (
            "key_pool_exhaustion",
            b"".join((str(i).zfill(4) + "K" * 4092 + "\tv\n").encode() for i in range(49)),
            ["0000" + "K" * 4092],
        ),
        (
            "key_count_at_limit",
            b"".join(("K%d\tv\n" % i).encode() for i in range(6144)),
            ["K0", "K6143"],
        ),
        (
            "key_count_exhaustion",
            b"".join(("K%d\tv\n" % i).encode() for i in range(6145)),
            ["K0", "K6144"],
        ),
        ("source_over_limit", b"x" * (16 * 1024 * 1024 + 1), ["Good"]),
    ]
    return [(name, raw, queries, False) for name, raw, queries in cases] + [
        ("reinit_replaces_old", b"New\tnew\n", ["Prior", "New"], True),
        ("reinit_empty_clears_old", b"", ["Prior"], True),
        ("reinit_invalid_clears_old", b"Bad\t\\xD800\n", ["Prior", "Bad"], True),
    ]


def read_lookups(result, offset, queries):
    values = {}
    for key in queries:
        if offset + 8 > len(result):
            raise ValueError("Incomplete native lookup lengths")
        key_len, value_len = struct.unpack_from("<II", result, offset)
        offset += 8
        end_key, end_value = offset + key_len * 2, offset + (key_len + value_len) * 2
        if end_value > len(result) or result[offset:end_key].decode("utf-16-le") != key:
            raise ValueError("Incomplete or reordered native lookup")
        values[key] = result[end_key:end_value].decode("utf-16-le") if value_len else None
        offset = end_value
    if offset != len(result):
        raise ValueError("Trailing native lookup data")
    return values


def write_input(folder, raw, queries):
    source, query_file, output = (
        folder / name for name in ("input.bin", "queries.bin", "observed.bin")
    )
    source.write_bytes(raw)
    with query_file.open("wb") as stream:
        for key in queries:
            encoded = key.encode("utf-16-le")
            stream.write(struct.pack("<I", len(encoded) // 2) + encoded)
    return source, query_file, output


def observe(probe, raw, queries, folder, seed=False):
    source, query_file, output = write_input(folder, raw, queries)
    command = [str(probe), str(source), str(query_file), str(output)]
    if seed:
        command.append("--seed")
    process = subprocess.run(command, check=True, capture_output=True, timeout=30)
    result = output.read_bytes()
    header_bytes = (len(FIELDS) + 3) * 4
    if len(result) < header_bytes:
        raise ValueError("Incomplete dictionary probe header")
    header = struct.unpack_from("<%dI" % (len(FIELDS) + 3), result)
    if header[:3] != (0x31424441, 2, 2):
        raise ValueError("Unknown dictionary probe protocol")
    values = read_lookups(result, header_bytes, queries)
    return dict(
        input_sha256=digest(raw),
        state=dict(zip(FIELDS, header[3:])),
        queries=len(queries),
        found=sum(value is not None for value in values.values()),
        values_sha256=values_digest(values),
        diagnostics=read_diagnostics(process.stdout),
    ), values


@unittest.skipUnless(
    os.name == "nt" and PROBE.is_file(), "Build modules/dict/tests first for native behavior checks"
)
class DictionaryBehaviorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        probe_provenance(PROBE)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ac1-dictionary-behavior-")
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.baseline = json.loads(FIXTURE.read_text(encoding="utf-8"))
        self.diagnostics = diagnostic_expectations()
        self.assertEqual(self.baseline["format_version"], 1)

    def test_byte_loader_matches_frozen_edge_cases(self):
        cases = corpus()
        self.assertEqual({name for name, *_ in cases}, set(self.baseline["cases"]))
        self.assertEqual(set(self.diagnostics["cases"]), set(self.baseline["cases"]))
        self.assertTrue(
            self.diagnostics["retained_counters"].keys() <= self.baseline["cases"].keys()
        )
        for name, raw, queries, seed in cases:
            with self.subTest(case=name):
                observed, _ = observe(PROBE, raw, queries, self.folder, seed)
                self.assertEqual(digest(raw), self.baseline["cases"][name]["input_sha256"])
                if name in V2_REJECTIONS:
                    reason, line = V2_REJECTIONS[name]
                    self.assertEqual(
                        {
                            k: observed["state"][k]
                            for k in ("accepted", "ready", "keys", "key_pool", "value_pool")
                        },
                        dict(accepted=0, ready=0, keys=0, key_pool=0, value_pool=0),
                    )
                    self.assertEqual(observed["found"], 0)
                    self.assertEqual(observed["diagnostics"]["reason"], reason)
                    self.assertEqual(observed["diagnostics"]["failure_line"], line)
                    continue
                if name == "cjk_key_skipped":
                    self.assertEqual(observed["state"]["keys"], 2)
                    self.assertEqual(
                        observed["values_sha256"], values_digest({"Good": "ok", "中文": "value"})
                    )
                    self.assertEqual(observed["diagnostics"]["result"], "clean")
                    continue
                self.assertEqual(observed, expected_behavior(self.baseline, self.diagnostics, name))

    @unittest.skipUnless(
        REVIEWED_DICTIONARY.is_file(), "Private reviewed Chinese dictionary is not supplied"
    )
    def test_reviewed_production_dictionary_matches_full_frozen_values(self):
        raw, values = reviewed_production(self.baseline)
        observed, actual = observe(PROBE, raw, sorted(values), self.folder)
        self.assertEqual(observed["state"]["accepted"], 1)
        self.assertEqual(actual, values)
        self.assertEqual(observed["values_sha256"], self.baseline["production"]["values_sha256"])
        self.assertEqual(observed["state"]["keys"], 1466)


@unittest.skipUnless(
    os.name == "nt" and DEFAULT_ORACLE.is_file(),
    "Build tools/modules/build_runtime_oracle.ps1 first",
)
class DictionaryOracleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        oracle_provenance(DEFAULT_ORACLE)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ac1-dictionary-oracle-")
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.baseline = json.loads(FIXTURE.read_text(encoding="utf-8"))
        self.diagnostics = diagnostic_expectations()

    def test_oracle_matches_frozen_edge_cases(self):
        for name, raw, queries, _seed in corpus():
            with self.subTest(case=name):
                expected = expected_behavior(self.baseline, self.diagnostics, name)
                self.assertEqual(digest(raw), expected["input_sha256"])
                folder = self.folder / name
                folder.mkdir()
                paths = write_input(folder, raw, queries)
                result = subprocess.run(
                    [str(DEFAULT_ORACLE), *map(str, paths)], capture_output=True, timeout=30
                )
                # Empty/oversized files fail in core's bounded reader, before dict runs.
                if name in V2_REJECTIONS:
                    self.assertEqual(result.returncode, 3)
                    self.assertFalse(paths[2].exists())
                    self.assertEqual(
                        read_diagnostics(result.stdout)["reason"], V2_REJECTIONS[name][0]
                    )
                    continue
                if raw and len(raw) <= 16 * 1024 * 1024:
                    if name != "cjk_key_skipped":
                        self.assertEqual(
                            read_diagnostics(result.stdout), self.diagnostics["cases"][name]
                        )
                else:
                    self.assertIn(b"dictionary read failed", result.stderr)
                if not expected["state"]["accepted"]:
                    self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
                    self.assertFalse(paths[2].exists())
                    continue

                if name == "cjk_key_skipped":
                    self.assertEqual(result.returncode, 0)
                    self.assertEqual(
                        read_lookups(paths[2].read_bytes(), 68, queries),
                        {"Good": "ok", "中文": "value"},
                    )
                    continue

                # Exit 1 also reports an incomplete query set; these fixtures intentionally
                # include misses and sample only a few keys in the capacity cases.
                complete = expected["found"] == len(queries) == expected["state"]["keys"]
                self.assertEqual(result.returncode, 0 if complete else 1)
                native = paths[2].read_bytes()
                header = struct.unpack_from("<17I", native)
                self.assertEqual(header[:3], (0x4F444341, 2, 2))
                self.assertEqual(
                    dict(zip(ORACLE_FIELDS, header[3:])),
                    {field: expected["state"][field] for field in ORACLE_FIELDS},
                )
                values = read_lookups(native, 68, queries)
                self.assertEqual(values_digest(values), expected["values_sha256"])

    @unittest.skipUnless(
        REVIEWED_DICTIONARY.is_file(), "Private reviewed Chinese dictionary is not supplied"
    )
    def test_oracle_exports_full_frozen_production_values(self):
        raw, frozen = reviewed_production(self.baseline)
        source = self.folder / "reviewed-production.tsv"
        source.write_bytes(raw)
        values, audit = effective_dictionary(source, DEFAULT_ORACLE, self.folder)
        expected = self.baseline["production"]
        self.assertEqual(values_digest(values), expected["values_sha256"])
        self.assertEqual(values, frozen)
        self.assertEqual(audit["stats"]["keys"], 1466)
        self.assertEqual(audit["rules_version"], 2)

    def test_unreadable_input_preserves_rejection_exit_code(self):
        paths = write_input(self.folder, b"Key\tvalue\n", ["Key"])
        paths[0].unlink()
        result = subprocess.run(
            [str(DEFAULT_ORACLE), *map(str, paths)], capture_output=True, timeout=30
        )
        self.assertEqual(result.returncode, 3)
        self.assertIn(b"dictionary read failed", result.stderr)
        self.assertFalse(paths[2].exists())


class ProbeReceiptTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ac1-probe-receipt-")
        self.addCleanup(self.temp.cleanup)
        self.executable = Path(self.temp.name) / "probe.exe"
        self.executable.write_bytes(b"probe receipt fixture; never executed")
        self.receipt = dict(
            format_version=1,
            exe_sha256=digest(self.executable.read_bytes()),
            sources={name: digest((ROOT / name).read_bytes()) for name in PROBE_SOURCES},
        )

    def save(self):
        self.executable.with_suffix(".json").write_text(json.dumps(self.receipt), encoding="utf-8")

    def test_changed_probe_executable_is_rejected(self):
        self.save()
        probe_provenance(self.executable)
        self.executable.write_bytes(b"different executable")
        with self.assertRaisesRegex(ValueError, "executable changed"):
            probe_provenance(self.executable)

    def test_changed_production_source_receipt_is_rejected(self):
        self.receipt["sources"]["modules/dict/src/dict.cpp"] = "0" * 64
        self.save()
        with self.assertRaisesRegex(ValueError, "Stale dictionary probe source"):
            probe_provenance(self.executable)

    def test_missing_toolchain_source_is_rejected(self):
        del self.receipt["sources"]["tools/modules/native-toolchain.ps1"]
        self.save()
        with self.assertRaisesRegex(ValueError, "source set changed"):
            probe_provenance(self.executable)

    def test_changed_toolchain_source_receipt_is_rejected(self):
        self.receipt["sources"]["tools/modules/native-toolchain.ps1"] = "0" * 64
        self.save()
        with self.assertRaisesRegex(ValueError, "Stale dictionary probe source"):
            probe_provenance(self.executable)


if __name__ == "__main__":
    if os.name != "nt" or not PROBE.is_file() or not DEFAULT_ORACLE.is_file():
        raise SystemExit(
            "Native behavior gate requires Windows, dict tests and runtime oracle builds"
        )
    unittest.main(verbosity=2)
