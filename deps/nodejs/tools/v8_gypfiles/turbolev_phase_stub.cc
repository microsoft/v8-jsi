// v8jsisb.dll (jitless/lite sandbox variant) build support.
//
// When maglev is disabled we drop turbolev-graph-builder.cc (the Maglev->Turboshaft
// frontend) from the compiler sources because it pulls in the whole maglev compiler.
// But pipeline.cc still references turboshaft::MaglevGraphBuildingPhase::Run
// unconditionally. That phase only runs when *optimizing JS functions*; it is never
// invoked while mksnapshot generates the CSA builtins. This stub satisfies the linker.
// Only compiled into mksnapshot for the jitless variant (v8_enable_turbofan==0),
// where turbolev-graph-builder.cc is dropped from the compiler sources (see v8.gyp).
#include "src/compiler/turboshaft/turbolev-graph-builder.h"

namespace v8::internal::compiler::turboshaft {

std::optional<BailoutReason> MaglevGraphBuildingPhase::Run(PipelineData*, Zone*,
                                                           Linkage*) {
  return BailoutReason::kGraphBuildingFailed;
}

}  // namespace v8::internal::compiler::turboshaft
