#pragma once

namespace ac1 {
namespace core { class RuntimePackage; }
namespace glyph {

// Startup-only runtime resource interface. Package storage must
// outlive every hook. bind does not mutate game memory or install hooks.
bool runtime_bind(const core::RuntimePackage& package);
bool runtime_install(unsigned host_base);
// Coherent CPU atlases permit pre-layout materialization. Native GPU upload is
// observed separately: it may be lazy and cannot gate the text that triggers it.
bool runtime_ready();
void runtime_status(char* output, int capacity);

} // namespace glyph
} // namespace ac1
