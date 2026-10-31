# Runtime package reader

`core::RuntimePackage` validates `AC1_Language.bin` before exposing immutable dictionary, font and texture views.
Its public header is `modules/core/include/ac1/core/runtime_package.h`.
See [Package layout](RUNTIME_PACKAGE.md) and [Runtime implementation](ENGINEERING.md).

The app owns the package for the lifetime of all hooks and borrowed views. A failed open exposes no usable data;
reopening after a successful open is rejected. Font directory order is `bold`, `techno`, `textpc`, `title`.
Select resources by identity or name rather than assuming an engine load order.

The reader checks segment bounds, alignment, hashes, padding, font budgets, BC2 lengths, original-pixel protection,
appended glyph order, metrics and UVs. SHA-256 uses Windows CNG; these hashes check consistency, not authorship.
The host adapter separately validates executable signatures and actual original font resources.

Dictionary bytes are copied into the dictionary module's pools. Font records and texture bytes remain borrowed package
views; engine-owned objects must use native allocation and copying. Zero-append fonts keep original resources.

Original UI control labels can contain `按钮`. The Chinese preset explicitly includes those code points for rendering;
their presence in the atlas does not imply a translation. Supplements belong to `extra_characters`, with evidence of original text.

Text checks resource readiness before replacing UI strings. This gate does not restore replaced strings or guarantee
recovery across all device lifecycles. Check both renderers in the game after changing resources.
