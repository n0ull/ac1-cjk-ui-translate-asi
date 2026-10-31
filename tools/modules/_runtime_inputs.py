"""Offline source adapters. Binary parsing and serialization live elsewhere."""

import hashlib
import json
import re
import struct
import subprocess
from pathlib import Path

import _dictionary as dictionary_text

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
DEFAULT_LANGUAGE = ROOT / "data/languages/zh/language.json"
BASELINES = HERE / "runtime/hosts"
DEFAULT_ORACLE = ROOT / "build/runtime-oracle/dictionary_oracle.exe"

ORACLE_SOURCES = (
    "tools/modules/runtime/dictionary_oracle.cpp",
    "tools/modules/build_runtime_oracle.ps1",
    "tools/modules/native-toolchain.ps1",
    "modules/dict/src/dict.cpp",
    "modules/core/src/file.cpp",
    "modules/core/src/str.cpp",
    "modules/dict/include/ac1/dict/dict.h",
    "modules/core/include/ac1/core/file.h",
    "modules/core/include/ac1/core/str.h",
)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def write_json(path, value):
    with path.open("x", encoding="utf-8") as stream:
        stream.write(json.dumps(value, ensure_ascii=False, indent=2) + "\n")


def source_path(name):
    path = Path(name)
    return path if path.is_absolute() else ROOT / path


def checked_source(name, expected):
    raw = source_path(name).read_bytes()
    if sha(raw) != expected:
        raise ValueError(f"Input fingerprint mismatch: {name}")
    return raw


def language_profile(path, language):
    expected = {
        "language_format_version",
        "language_id",
        "baseline",
        "dictionary",
        "font_sha256",
        "extra_characters",
        "fonts",
    }
    if (
        set(language) != expected
        or type(language["language_format_version"]) is not int
        or language["language_format_version"] != 1
    ):
        raise ValueError("Unknown language configuration fields/version")
    label = language["language_id"]
    if (
        not isinstance(label, str)
        or len(label) > 31
        or not re.fullmatch(r"[a-z][a-z0-9]*(?:-[a-z0-9]+)*", label)
    ):
        raise ValueError("Invalid language identifier")
    baseline_id = language["baseline"]
    if not isinstance(baseline_id, str) or not re.fullmatch(r"[a-z][a-z0-9-]*", baseline_id):
        raise ValueError("Invalid shared baseline identifier")
    baseline_bytes = (BASELINES / (baseline_id + ".json")).read_bytes()
    baseline = json.loads(baseline_bytes.decode("utf-8"))
    if (
        not isinstance(baseline, dict)
        or set(baseline)
        != {
            "baseline_format_version",
            "id",
            "hosts",
            "max_package_bytes",
            "max_total_records_per_font",
            "fonts",
        }
        or type(baseline["baseline_format_version"]) is not int
        or baseline["baseline_format_version"] != 1
        or baseline["id"] != baseline_id
    ):
        raise ValueError("Unknown shared baseline fields/version")
    hosts = baseline["hosts"]
    if not isinstance(hosts, dict) or not hosts or not set(hosts) <= {"dx9", "dx10"}:
        raise ValueError("Invalid baseline hosts")
    for name, host in hosts.items():
        if (
            not isinstance(host, dict)
            or set(host) != {"exe", "crc32"}
            or host["exe"] != f"AssassinsCreed_{name.capitalize()}.exe"
            or not isinstance(host["crc32"], str)
            or not re.fullmatch(r"[0-9A-F]{8}", host["crc32"])
        ):
            raise ValueError("Invalid baseline host identity")
    dictionary = language["dictionary"]
    if (
        not isinstance(dictionary, str)
        or not dictionary
        or Path(dictionary).name != dictionary
        or dictionary in (".", "..")
        or ":" in dictionary
        or "\\" in dictionary
    ):
        raise ValueError("Language dictionary must be an adjacent filename")
    original_fonts = baseline["fonts"]
    recipes = language["fonts"]
    original_fields = {
        "name",
        "mft",
        "mft_sha256",
        "original_texture",
        "original_texture_sha256",
        "original_records_sha256",
        "original_count",
        "asset_id",
        "class_hash",
        "original_dimensions",
    }
    if not isinstance(original_fonts, list) or len(original_fonts) != 4:
        raise ValueError("Expected four original font identities")
    for font in original_fonts:
        if not isinstance(font, dict) or set(font) != original_fields:
            raise ValueError("Unknown original font identity fields")
        if not isinstance(font["name"], str):
            raise ValueError("Invalid original font name")
    originals = {font["name"]: font for font in original_fonts}
    if not isinstance(recipes, list) or len(recipes) != 4 or len(originals) != 4:
        raise ValueError("Expected four original font identities and recipes")
    recipe_fields = {
        "name",
        "target_dimensions",
        "font_px",
        "advance",
        "font_baseline",
        "native_baseline",
        "density",
        "bake",
        "axes",
    }
    fonts = []
    for recipe in recipes:
        if (
            not isinstance(recipe, dict)
            or set(recipe) != recipe_fields
            or not isinstance(recipe["name"], str)
            or recipe["name"] not in originals
        ):
            raise ValueError("Unknown language font recipe fields")
        fonts.append(
            dict(originals[recipe["name"]], **{k: v for k, v in recipe.items() if k != "name"})
        )
    return dict(
        profile_version=3,
        runtime_package_version=2,
        dictionary_rules_version=2,
        id=label,
        language_id=label,
        baseline_id=baseline_id,
        hosts=hosts,
        dictionary_path=str((path.parent / dictionary).resolve()),
        font_sha256=language["font_sha256"],
        extra_characters=language["extra_characters"],
        fonts=fonts,
        max_package_bytes=baseline["max_package_bytes"],
        max_total_records_per_font=baseline["max_total_records_per_font"],
        _baseline_bytes=baseline_bytes,
    )


def read_profile(path, input_root, oracle_path=DEFAULT_ORACLE):
    raw = path.read_bytes()
    profile = json.loads(raw.decode("utf-8"))
    if not isinstance(profile, dict):
        raise ValueError("Profile must be an object")
    profile = language_profile(path, profile)

    def integer(value, low, high, label):
        if type(value) is not int or not low <= value <= high:
            raise ValueError(f"Invalid {label}: expected integer in {low}..{high}")

    def digest(value):
        if (
            not isinstance(value, str)
            or len(value) != 64
            or any(ch not in "0123456789abcdef" for ch in value)
        ):
            raise ValueError("Expected lowercase SHA-256 fingerprint")

    integer(profile["max_package_bytes"], 1024, 64 * 1024 * 1024, "package budget")
    integer(profile["max_total_records_per_font"], 1, 4096, "native record budget")
    digest(profile["font_sha256"])
    fonts = profile["fonts"]
    if not isinstance(fonts, list) or any(not isinstance(item, dict) for item in fonts):
        raise ValueError("Profile fonts must be a list of objects")
    if any(not isinstance(item["name"], str) for item in fonts) or sorted(
        item["name"] for item in fonts
    ) != ["bold", "techno", "textpc", "title"]:
        raise ValueError("Profile must identify all four distinct fonts")
    input_root = input_root.resolve()
    if not input_root.is_dir():
        raise FileNotFoundError(f"Missing explicit input root: {input_root}")
    identities = set()
    for item in fonts:
        for field in ("original_dimensions", "target_dimensions"):
            dimensions = item[field]
            if not isinstance(dimensions, list) or len(dimensions) != 2:
                raise ValueError("Dimensions must contain width and height")
            for value in dimensions:
                integer(value, 4, 8192, "dimension")
                if value & (value - 1):
                    raise ValueError("Dimensions must be powers of two")
        old_w, old_h = item["original_dimensions"]
        width, height = item["target_dimensions"]
        integer(item["density"], 2, 2, "raster density")
        if width < old_w or height < old_h:
            raise ValueError("Profile cannot preserve the original atlas band at density 2")
        for field in ("asset_id", "class_hash"):
            integer(item[field], 0, 0xFFFFFFFF, field)
        identity = (item["asset_id"], item["class_hash"])
        if identity in identities:
            raise ValueError("Duplicate profile resource identity")
        identities.add(identity)
        integer(
            item["original_count"],
            1,
            profile["max_total_records_per_font"],
            "original record count",
        )
        integer(item["font_px"], 1, 256, "font pixel size")
        integer(item["advance"], 1, 512, "advance")
        integer(item["font_baseline"], -32768, 32767, "font baseline")
        integer(item["native_baseline"], -32768, 32767, "native baseline")
        axes = item["axes"]
        if not isinstance(axes, dict) or any(
            not isinstance(tag, str)
            or len(tag) != 4
            or type(value) not in (int, float)
            or not float("-inf") < value < float("inf")
            for tag, value in axes.items()
        ):
            raise ValueError("Invalid font variation axes")
        if item["bake"] not in ("bold_cell", "padded_ink"):
            raise ValueError("Unknown raster strategy")
        for field in ("mft_sha256", "original_texture_sha256", "original_records_sha256"):
            digest(item[field])
        for field in ("mft", "original_texture"):
            if not isinstance(item[field], str) or not item[field]:
                raise ValueError(f"Missing input path: {field}")
            source = Path(item[field])
            escapes_root = source.is_absolute() or source.drive or ".." in source.parts
            if escapes_root:
                raise ValueError("Shared input paths must stay relative to the input root")
            item[field] = str((input_root / source).resolve())
            if not Path(item[field]).is_relative_to(input_root):
                raise ValueError("Shared input path resolves outside the input root")

    if (
        sum(item["target_dimensions"][0] * item["target_dimensions"][1] for item in fonts) + 1024
        > profile["max_package_bytes"]
    ):
        raise ValueError("Texture plans exceed the configured package budget")
    extra = profile["extra_characters"]
    if not isinstance(extra, dict) or extra.get("source_kind") != "original_ui_text":
        raise ValueError("Supplement must identify original UI text")
    if not isinstance(extra.get("evidence"), str) or not extra["evidence"].strip():
        raise ValueError("Supplement must document its evidence")
    if not isinstance(extra.get("source_text"), str):
        raise ValueError("Supplement must contain source text")
    validate_text(extra["source_text"], "extra UI text")
    codepoints = extra["codepoints"]
    if not isinstance(codepoints, list) or any(
        not isinstance(cp, str) or len(cp) != 6 or not cp.startswith("U+") for cp in codepoints
    ):
        raise ValueError("Supplement codepoints must use U+NNNN")
    try:
        cps = {int(cp[2:], 16) for cp in codepoints}
    except ValueError as error:
        raise ValueError("Invalid supplement codepoint") from error
    if (
        len(cps) != len(codepoints)
        or cps != {ord(ch) for ch in extra["source_text"]}
        or any(cp < 32 for cp in cps)
    ):
        raise ValueError("Supplement codepoints do not match source text")
    profile["oracle_path"] = str(oracle_path.resolve())
    profile["_source_bytes"] = raw
    return profile


def raster_font(path, item):
    from fontTools.ttLib import TTFont
    from PIL import ImageFont

    requested = item["axes"]
    with TTFont(path) as source:
        axes = list(source["fvar"].axes) if "fvar" in source else []
    by_tag = {axis.axisTag: axis for axis in axes}
    if not set(requested) <= set(by_tag):
        raise ValueError("Source font does not contain the requested variation axes")
    values = []
    for axis in axes:
        value = requested.get(axis.axisTag, axis.defaultValue)
        if not axis.minValue <= value <= axis.maxValue:
            raise ValueError(f"Font variation axis outside its range: {axis.axisTag}")
        values.append(value)
    font = ImageFont.truetype(str(path), item["font_px"] * 2)
    if values:
        font.set_variation_by_axes(values)
    return font


def oracle_provenance(path):
    manifest_path = path.with_suffix(".json")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    if manifest.get("format_version") != 1 or manifest.get("target") != "x86":
        raise ValueError("Unknown production dictionary oracle manifest")
    if sha(path.read_bytes()) != manifest["exe_sha256"]:
        raise ValueError("Dictionary oracle executable fingerprint mismatch; rebuild it")
    sources = manifest["sources"]
    if set(sources) != set(ORACLE_SOURCES):
        raise ValueError("Dictionary oracle source set mismatch; rebuild it")
    for name in ORACLE_SOURCES:
        if sha((ROOT / name).read_bytes()) != sources[name]:
            raise ValueError(
                f"Stale dictionary oracle source {name}; run tools/modules/build_runtime_oracle.ps1"
            )
    return manifest


def original_font(item):
    raw = checked_source(item["mft"], item["mft_sha256"])
    marker = raw.index(b"PixmapFont")
    count = struct.unpack_from("<H", raw, marker + 10)[0]
    if marker + 12 + count * 30 > len(raw):
        raise ValueError("Truncated original MFT records")
    canonical = raw[marker + 12 : marker + 12 + count * 30]
    if count != item["original_count"] or sha(canonical) != item["original_records_sha256"]:
        raise ValueError(f"{item['name']}: original MFT record identity mismatch")
    records = [part[0] + b"\0\0" for part in struct.iter_unpack("<30s", canonical)]
    rows = [struct.unpack("<HHHhhh4fHH", record) for record in records]
    if len({row[0] for row in rows}) != count:
        raise ValueError("Original font contains duplicate codepoints")
    texture = checked_source(item["original_texture"], item["original_texture_sha256"])
    if len(texture) != item["original_dimensions"][0] * item["original_dimensions"][1]:
        raise ValueError("Original BC2 payload size mismatch")
    return rows, texture


def validate_text(text, label):
    for ch in text:
        cp = ord(ch)
        unsupported_scalar = cp > 0xFFFF or 0xD800 <= cp <= 0xDFFF
        unsupported_control = cp < 32 and ch not in "\n\r\t"
        if unsupported_scalar or unsupported_control or cp == 127:
            raise ValueError(f"{label}: unsupported U+{cp:04X}")


def dictionary_audit(raw):
    """Explain rejected/overridden lines without replacing the dictionary helper's loader."""
    if not raw or len(raw) > 16 * 1024 * 1024:
        raise ValueError("Dictionary must contain 1..16 MiB of source bytes")
    seen, rejected, duplicates = {}, [], []
    for line_number, line in enumerate(raw.split(b"\n"), 1):
        if line_number == 1 and line.startswith(b"\xef\xbb\xbf"):
            line = line[3:]
        try:
            text = line.decode("utf-8").rstrip("\r").strip(" \t")
        except UnicodeDecodeError:
            rejected.append(dict(line=line_number, reason="invalid_utf8"))
            continue
        if not text or (text.startswith("#") and "\t" not in text):
            continue
        if "\t" not in text:
            rejected.append(dict(line=line_number, reason="missing_tab"))
            continue
        key, value = text.split("\t", 1)
        key = dictionary_text.canon(dictionary_text.unescape_value(key.strip(" \t")))
        value = dictionary_text.unescape_value(value.strip(" \t\r\n"))
        # Unsupported native code units must fail even if a later duplicate
        # would otherwise hide that line from the effective dictionary.
        validate_text(key, f"dictionary key line {line_number}")
        validate_text(value, f"dictionary value line {line_number}")
        reason = None
        if not key or not dictionary_text.trim_value(value):
            reason = "empty_key_or_value"
        elif len(key) > 4096 or len(value) > 8192:
            raise ValueError(f"Dictionary capacity limit at line {line_number}")
        if reason:
            rejected.append(dict(line=line_number, key=key, reason=reason))
            continue
        if key in seen:
            previous = seen[key]
            duplicates.append(
                dict(
                    line=line_number,
                    previous_line=previous[0],
                    key=key,
                    same_value=previous[1] == value,
                )
            )
        seen[key] = (line_number, value)
    return dict(rejected=rejected, duplicates=duplicates)


def effective_dictionary(path, oracle, out):
    if dictionary_text.looks_like_lg_dict(str(path)):
        raise ValueError(
            "--dict expects a reviewed UTF-8 TAB dictionary; export LG input explicitly first"
        )
    oracle_provenance(oracle)
    raw = path.read_bytes()
    audit = dictionary_audit(raw)
    queried = dictionary_text.load_supplement(str(path))
    queries = out / "dictionary-queries.bin"
    with queries.open("wb") as stream:
        for key in sorted(queried):
            encoded = key.encode("utf-16-le")
            stream.write(struct.pack("<I", len(encoded) // 2) + encoded)
    native_file = out / "dictionary-native.bin"
    try:
        result = subprocess.run(
            [str(oracle), str(path), str(queries), str(native_file)],
            capture_output=True,
            check=False,
            timeout=30,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
    except subprocess.TimeoutExpired as error:
        raise ValueError("Production dictionary oracle timed out") from error
    if result.returncode:
        detail = (result.stdout or result.stderr).decode("utf-8", errors="replace").strip()[:512]
        raise ValueError(f"Production dictionary oracle failed: exit {result.returncode}; {detail}")
    native = native_file.read_bytes()
    header = struct.unpack_from("<17I", native)
    if header[:3] != (0x4F444341, 2, 2):
        raise ValueError("Unknown production oracle output")
    names = "keys key_pool value_pool lines new duplicates bad key_bad value_bad pool_full key_full key_long value_long fallback_full".split()
    stats = dict(zip(names, header[3:]))
    values, offset = {}, 68
    while offset < len(native):
        key_len, value_len = struct.unpack_from("<II", native, offset)
        offset += 8
        end_key, end_value = offset + key_len * 2, offset + (key_len + value_len) * 2
        if not value_len or end_value > len(native):
            raise ValueError("Incomplete production lookup")
        key = native[offset:end_key].decode("utf-16-le")
        values[key] = native[end_key:end_value].decode("utf-16-le")
        offset = end_value
    if len(values) != stats["keys"] or set(values) != set(queried):
        raise ValueError("Python query set does not cover the production dictionary")
    if not values:
        raise ValueError("Production dictionary has no effective values")
    if any(
        stats[name] for name in ("pool_full", "key_full", "key_long", "value_long", "fallback_full")
    ):
        raise ValueError("Production dictionary rejected entries for capacity")
    for key, value in values.items():
        expected = dictionary_text.trim_value(queried[key]).replace("\u2026", "...")
        if value != expected:
            raise ValueError(f"Production normalization changed outside rules v2: {key!r}")
        validate_text(value, "effective value")
    audit.update(
        rules_version=2,
        stats=stats,
        fallback_changes=[
            dict(key=k, before=queried[k], after=v) for k, v in values.items() if queried[k] != v
        ],
        max_key_units=max(map(len, values)),
        max_value_units=max(map(len, values.values())),
    )
    return values, audit


def corpus(values, profile):
    dictionary = {ord(ch) for value in values.values() for ch in value if ord(ch) >= 32}
    supplement = profile["extra_characters"]
    if supplement["source_kind"] != "original_ui_text" or not supplement["evidence"]:
        raise ValueError("Supplement must identify its actual source")
    validate_text(supplement["source_text"], "extra UI text")
    cps = {int(cp[2:], 16) for cp in supplement["codepoints"]}
    if cps != {ord(ch) for ch in supplement["source_text"]}:
        raise ValueError("Supplement codepoints do not match its source text")
    return dictionary, cps
