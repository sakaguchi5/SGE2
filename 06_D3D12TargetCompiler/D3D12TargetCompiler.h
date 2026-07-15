#pragma once

#include "../00_Base/Result.h"

// Windows SDK headers define `interface` as a preprocessor macro.
// SemanticModel intentionally uses `Program::interface` as a member name,
// so protect that declaration when this header is included after Windows headers.
#ifdef interface
#pragma push_macro("interface")
#undef interface
#define SGE_D3D12_TARGET_COMPILER_RESTORE_INTERFACE_MACRO
#endif

#include "../02_SemanticModel/SemanticModel.h"
#include "../05_TargetModel/TargetModel.h"

#ifdef SGE_D3D12_TARGET_COMPILER_RESTORE_INTERFACE_MACRO
#pragma pop_macro("interface")
#undef SGE_D3D12_TARGET_COMPILER_RESTORE_INTERFACE_MACRO
#endif

#include <cstddef>
#include <string>
#include <vector>

namespace sge::compiler::d3d12
{
struct CompileError final
{
    std::string stage;
    std::string message;
};

struct CompileOutput final
{
    std::vector<std::byte> packageBytes;
    std::string executionDigestHex;
};

[[nodiscard]] base::Result<CompileOutput, CompileError> Compile(
    const semantic::SemanticGraph& graph,
    const target::D3D12TargetProfile& targetProfile);
}
