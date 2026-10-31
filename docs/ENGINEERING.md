# Runtime implementation

The runtime loads one language package, prepares four fonts and replaces text before native measurement.
Build instructions are in [Build and test](development.md); package creation is in [the translator guide](translate.md).

## Modules

```text
core → dict → text
core → glyph
core + dict + text + glyph → app → ac1_runtime_package.asi
```

| Module | Responsibility |
| --- | --- |
| `core` | Host identity, hooks, memory/file guards, logging and package validation |
| `dict` | Production UTF-8 dictionary decoding, normalization and exact lookup |
| `text` | Replace materialized UI strings at confirmed callers before measurement |
| `glyph` | Host-specific font tables, UV adaptation, textures and resource state |
| `app` | Own the package and initialize modules in dependency order |

Cross-module includes use public headers under `include/ac1/<module>/`.
Text receives a resource-readiness callback rather than depending directly on glyph internals.
Offline tools have no runtime Python dependency.

## Startup and ownership

`DllMain` disables thread notifications. The loader calls exported `InitializeASI` synchronously outside loader lock.
`InitOnceExecuteOnce` prevents duplicate initialization.

Startup checks the host, package, dictionary and resource plans before installing text and resource hooks.
A missing or invalid package prevents activation. The app owns its package for the process lifetime;
font and texture views borrow that immutable storage. There is no hot reload or unload interface.

Both host CRC32 and hook entry signatures must match. Supported executable identities and language limits are
listed in [Font presets](fonts.md#support-limits). Resource helpers and hooks are defined per host in
`modules/glyph/src/runtime.cpp`; text callers are in `modules/text/src/path_funnel.cpp`.

## Font preparation

Each resource must match its asset ID, class, name, dimensions, original record count and original-record fingerprint.
The engine owns font tables and GPU resources. The runtime uses its allocation, construction and reserve callbacks,
copies package pixels, and keeps borrowed storage out of engine destructors.

Each font uses page 0. Original BC2 blocks occupy the lower-left protected band of an expanded atlas.
Original UVs scale with atlas dimensions. Appended glyphs retain their baked advances, shared baseline and density.
A zero-append plan keeps the original texture, table and UVs.

CPU publication uses a resource lock and generation checks. Stale operations cannot publish into a newer generation.
Native callbacks run outside that lock to allow reentry. GPU uploads are observed separately from CPU readiness;
first-use uploads must not block the text that triggers them.

The font gate requires all four CPU plans and no font failure before text replacement.
Failure disables later writes; it does not restore translations already materialized in UI strings.
Font-instance lifetime, device/resource reconstruction and cached text need representative in-game verification.

## Text and dictionaries

The text hook accepts confirmed UI callers and validates the game's string layout and buffer capacity.
It performs normalized exact lookup, writes only when the translation fits, and reads back the result.
It does not grow buffers or truncate translations. Draw-time hooks adapt font data and UVs; translation occurs earlier.

`dict::init_bytes` is the sole dictionary initialization entry. The offline checker calls that same implementation;
Python handles source adaptation and compares results. Invalid encodings, unsupported code points, duplicate normalized keys,
capacity failures and unstable translation chains are rejected. See [Dictionary format](../data/README.md#text-dictionary-format).

Original control labels may contain `按钮` without translation. Explicit `extra_characters` gives those characters
font coverage even when they are absent from translated values. Additions must reflect observed UI text.

## Diagnostics and verification

Logs and text inventories are written beside the ASI in `AC1_Runtime/` with run-specific filenames.
Inventory entries record the first saved state and a maximum 48-code-unit prefix; an entry does not prove display.

The package writer is checked by an independent pixel/metric reader. Native tests exercise dictionary behavior,
font publication and rollback, text-buffer guards, inventory I/O and startup rejection paths.
Offline checks and installation hashes do not replace actual menu, control-label and scene-loading tests.

See [Package layout](RUNTIME_PACKAGE.md), [Reader interface](RUNTIME_READER.md) and
[Internal tools](../tools/modules/runtime/README.md) for binary, ownership and command references.
