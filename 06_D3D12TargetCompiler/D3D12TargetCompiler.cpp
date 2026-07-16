#include "D3D12TargetCompiler.h"

#include "../00_Base/BinaryIO.h"
#include "../00_Base/Sha256.h"
#include "../04_SemanticAnalysis/SemanticAnalysis.h"
#include "../07_FrozenPackageCore/PackageReader.h"
#include "../08_D3D12PackageSchema/D3D12Encoding.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wrl/client.h>

#ifdef interface
#undef interface
#endif
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <algorithm>
#include <bit>
#include <cctype>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxguid.lib")

namespace sge::compiler::d3d12
{
using Microsoft::WRL::ComPtr;
namespace pkg = package::d3d12_v13;

namespace
{
constexpr std::uint64_t DefaultPlacementAlignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
constexpr std::uint64_t ConstantPlacementAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;

CompileError Error(std::string stage, std::string message)
{
    return {std::move(stage), std::move(message)};
}

template<class T>
base::Result<T, CompileError> Failure(std::string stage, std::string message)
{
    return base::Result<T, CompileError>::Failure(Error(std::move(stage), std::move(message)));
}

std::string UpperAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char value) {
        return static_cast<char>(std::toupper(value));
    });
    return value;
}

const char* StageName(semantic::ShaderStage stage) noexcept
{
    switch (stage)
    {
    case semantic::ShaderStage::Vertex: return "Vertex";
    case semantic::ShaderStage::Pixel: return "Pixel";
    case semantic::ShaderStage::Compute: return "Compute";
    default: return "Unknown";
    }
}

const char* ParameterKindName(semantic::ProgramParameterKind kind) noexcept
{
    switch (kind)
    {
    case semantic::ProgramParameterKind::ConstantBuffer: return "ConstantBuffer";
    case semantic::ProgramParameterKind::SampledTexture: return "SampledTexture";
    case semantic::ProgramParameterKind::ReadOnlyBuffer: return "ReadOnlyBuffer";
    case semantic::ProgramParameterKind::UnorderedBuffer: return "UnorderedBuffer";
    default: return "Unknown";
    }
}

base::Result<ReflectedBindingKind, CompileError> ReflectedKind(
    const D3D11_SHADER_INPUT_BIND_DESC& binding,
    semantic::ShaderStage stage)
{
    switch (binding.Type)
    {
    case D3D_SIT_CBUFFER:
        return base::Result<ReflectedBindingKind, CompileError>::Success(
            ReflectedBindingKind::ConstantBuffer);
    case D3D_SIT_TEXTURE:
        if (binding.Dimension == D3D_SRV_DIMENSION_TEXTURE2D)
            return base::Result<ReflectedBindingKind, CompileError>::Success(
                ReflectedBindingKind::SampledTexture);
        if (binding.Dimension == D3D_SRV_DIMENSION_BUFFER ||
            binding.Dimension == D3D_SRV_DIMENSION_BUFFEREX)
            return base::Result<ReflectedBindingKind, CompileError>::Success(
                ReflectedBindingKind::ReadOnlyBuffer);
        break;
    case D3D_SIT_STRUCTURED:
    case D3D_SIT_BYTEADDRESS:
        return base::Result<ReflectedBindingKind, CompileError>::Success(
            ReflectedBindingKind::ReadOnlyBuffer);
    case D3D_SIT_UAV_RWTYPED:
        if (binding.Dimension == D3D_SRV_DIMENSION_BUFFER ||
            binding.Dimension == D3D_SRV_DIMENSION_BUFFEREX)
            return base::Result<ReflectedBindingKind, CompileError>::Success(
                ReflectedBindingKind::UnorderedBuffer);
        break;
    case D3D_SIT_UAV_RWSTRUCTURED:
    case D3D_SIT_UAV_RWBYTEADDRESS:
    case D3D_SIT_UAV_APPEND_STRUCTURED:
    case D3D_SIT_UAV_CONSUME_STRUCTURED:
    case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER:
        return base::Result<ReflectedBindingKind, CompileError>::Success(
            ReflectedBindingKind::UnorderedBuffer);
    case D3D_SIT_SAMPLER:
        return base::Result<ReflectedBindingKind, CompileError>::Success(
            ReflectedBindingKind::StaticSampler);
    default:
        break;
    }
    std::ostringstream message;
    message << StageName(stage) << " shader contains an unsupported reflected resource type at register "
            << binding.BindPoint;
    return Failure<ReflectedBindingKind>("shader-reflection", message.str());
}

base::Result<semantic::VertexInput::Meaning, CompileError> VertexMeaning(
    const D3D11_SIGNATURE_PARAMETER_DESC& input)
{
    const auto semanticName = UpperAscii(input.SemanticName ? input.SemanticName : "");
    if (semanticName == "POSITION")
        return base::Result<semantic::VertexInput::Meaning, CompileError>::Success(
            semantic::VertexInput::Meaning::Position);
    if (semanticName == "COLOR")
        return base::Result<semantic::VertexInput::Meaning, CompileError>::Success(
            semantic::VertexInput::Meaning::Color);
    if (semanticName == "TEXCOORD")
        return base::Result<semantic::VertexInput::Meaning, CompileError>::Success(
            semantic::VertexInput::Meaning::TexCoord);
    return Failure<semantic::VertexInput::Meaning>(
        "shader-reflection", "Vertex shader contains an unsupported input semantic: " + semanticName);
}

std::optional<semantic::ProgramParameterKind> SemanticKind(ReflectedBindingKind kind) noexcept
{
    switch (kind)
    {
    case ReflectedBindingKind::ConstantBuffer:
        return semantic::ProgramParameterKind::ConstantBuffer;
    case ReflectedBindingKind::SampledTexture:
        return semantic::ProgramParameterKind::SampledTexture;
    case ReflectedBindingKind::ReadOnlyBuffer:
        return semantic::ProgramParameterKind::ReadOnlyBuffer;
    case ReflectedBindingKind::UnorderedBuffer:
        return semantic::ProgramParameterKind::UnorderedBuffer;
    default:
        return std::nullopt;
    }
}

base::Result<void, CompileError> ValidateReflectedInterface(
    const semantic::Program& program,
    const CompiledShaderStage& shader)
{
    std::map<std::pair<std::uint16_t, std::uint32_t>, const semantic::ProgramParameter*> expected;
    bool expectsStaticSampler = false;
    for (const auto& parameter : program.interface.parameters)
    {
        if (parameter.stage != shader.stage) continue;
        expected[{static_cast<std::uint16_t>(parameter.kind), parameter.shaderRegister}] = &parameter;
        if (parameter.kind == semantic::ProgramParameterKind::SampledTexture)
            expectsStaticSampler = true;
    }

    std::set<std::pair<std::uint16_t, std::uint32_t>> reflected;
    std::uint32_t samplerCount = 0;
    for (const auto& binding : shader.bindings)
    {
        if (binding.kind == ReflectedBindingKind::StaticSampler)
        {
            ++samplerCount;
            if (shader.stage != semantic::ShaderStage::Pixel || binding.shaderRegister != 0 ||
                binding.bindCount != 1)
                return Failure<void>("shader-reflection",
                    "Level 2 v1 requires the reflected static sampler to be Pixel s0 with bind count 1");
            continue;
        }
        const auto semanticKind = SemanticKind(binding.kind);
        if (!semanticKind)
            return Failure<void>("shader-reflection", "reflected binding cannot map to a ProgramParameter");
        const auto key = std::pair{static_cast<std::uint16_t>(*semanticKind), binding.shaderRegister};
        if (!reflected.insert(key).second)
            return Failure<void>("shader-reflection", "shader reflection contains a duplicate binding");
        const auto found = expected.find(key);
        if (found == expected.end())
        {
            std::ostringstream message;
            message << StageName(shader.stage) << " shader declares " << ParameterKindName(*semanticKind)
                    << " register " << binding.shaderRegister
                    << " that is absent from ProgramInterface";
            return Failure<void>("shader-reflection", message.str());
        }
        if (binding.bindCount != 1)
            return Failure<void>("shader-reflection", "Level 2 v1 does not support shader binding arrays");
        if (*semanticKind == semantic::ProgramParameterKind::ConstantBuffer &&
            binding.requiredBytes != found->second->requiredBytes)
        {
            std::ostringstream message;
            message << StageName(shader.stage) << " constant buffer b" << binding.shaderRegister
                    << " reflects " << binding.requiredBytes << " bytes but ProgramInterface requires "
                    << found->second->requiredBytes;
            return Failure<void>("shader-reflection", message.str());
        }
    }
    if (reflected.size() != expected.size())
    {
        for (const auto& [key, parameter] : expected)
            if (!reflected.contains(key))
            {
                std::ostringstream message;
                message << StageName(shader.stage) << " ProgramInterface parameter "
                        << parameter->debugName << " at register " << parameter->shaderRegister
                        << " is absent from reflected shader bindings";
                return Failure<void>("shader-reflection", message.str());
            }
    }
    if ((expectsStaticSampler && samplerCount != 1) || (!expectsStaticSampler && samplerCount != 0))
        return Failure<void>("shader-reflection",
            "reflected sampler contract does not match sampled-texture ProgramParameters");

    if (shader.stage == semantic::ShaderStage::Vertex)
    {
        if (shader.vertexInputs.size() != program.interface.vertexInputs.size())
            return Failure<void>("shader-reflection",
                "Vertex shader input count does not match ProgramInterface");
        std::map<semantic::VertexInput::Meaning, std::uint16_t> expectedInputs;
        for (const auto& input : program.interface.vertexInputs)
            expectedInputs[input.meaning] = input.componentCount;
        for (const auto& reflectedInput : shader.vertexInputs)
        {
            const auto found = expectedInputs.find(reflectedInput.meaning);
            if (found == expectedInputs.end() || found->second != reflectedInput.componentCount ||
                reflectedInput.semanticIndex != 0)
                return Failure<void>("shader-reflection",
                    "Vertex shader input semantic/components do not match ProgramInterface");
        }
    }
    else if (!shader.vertexInputs.empty())
    {
        return Failure<void>("shader-reflection", "non-Vertex shader unexpectedly reflects vertex inputs");
    }
    return base::Result<void, CompileError>::Success();
}

base::Result<CompiledShaderStage, CompileError> CompileAndReflectShader(
    const semantic::Program& program,
    semantic::ShaderStage shaderStage,
    const std::string& entry,
    const char* profile)
{
    ComPtr<ID3DBlob> shader;
    ComPtr<ID3DBlob> errors;
    constexpr UINT flags = D3DCOMPILE_ENABLE_STRICTNESS |
                           D3DCOMPILE_WARNINGS_ARE_ERRORS |
                           D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const HRESULT result = D3DCompile(
        program.source.hlslSource.data(), program.source.hlslSource.size(), nullptr, nullptr, nullptr,
        entry.c_str(), profile, flags, 0, &shader, &errors);
    if (FAILED(result))
    {
        std::string message = "D3DCompile failed";
        if (errors)
            message.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        return Failure<CompiledShaderStage>("shader-compilation", std::move(message));
    }

    ComPtr<ID3D11ShaderReflection> reflection;
    const HRESULT reflectionResult = D3DReflect(
        shader->GetBufferPointer(), shader->GetBufferSize(), IID_ID3D11ShaderReflection,
        reinterpret_cast<void**>(reflection.GetAddressOf()));
    if (FAILED(reflectionResult) || !reflection)
        return Failure<CompiledShaderStage>("shader-reflection", "D3DReflect failed");

    D3D11_SHADER_DESC shaderDescription{};
    if (FAILED(reflection->GetDesc(&shaderDescription)))
        return Failure<CompiledShaderStage>("shader-reflection", "ID3D11ShaderReflection::GetDesc failed");

    CompiledShaderStage output;
    output.stage = shaderStage;
    for (UINT index = 0; index < shaderDescription.BoundResources; ++index)
    {
        D3D11_SHADER_INPUT_BIND_DESC binding{};
        if (FAILED(reflection->GetResourceBindingDesc(index, &binding)))
            return Failure<CompiledShaderStage>("shader-reflection", "GetResourceBindingDesc failed");
        auto kind = ReflectedKind(binding, shaderStage);
        if (!kind) return base::Result<CompiledShaderStage, CompileError>::Failure(kind.Error());
        ReflectedBinding reflectedBinding;
        reflectedBinding.kind = kind.Value();
        reflectedBinding.stage = shaderStage;
        reflectedBinding.shaderRegister = binding.BindPoint;
        reflectedBinding.bindCount = binding.BindCount;
        if (binding.Type == D3D_SIT_CBUFFER)
        {
            auto* constantBuffer = reflection->GetConstantBufferByName(binding.Name);
            D3D11_SHADER_BUFFER_DESC bufferDescription{};
            if (constantBuffer == nullptr || FAILED(constantBuffer->GetDesc(&bufferDescription)))
                return Failure<CompiledShaderStage>("shader-reflection", "constant-buffer reflection failed");
            reflectedBinding.requiredBytes = bufferDescription.Size;
        }
        output.bindings.push_back(reflectedBinding);
    }
    std::sort(output.bindings.begin(), output.bindings.end(), [](const auto& left, const auto& right) {
        return std::tuple{left.kind, left.shaderRegister} < std::tuple{right.kind, right.shaderRegister};
    });

    if (shaderStage == semantic::ShaderStage::Vertex)
    {
        for (UINT index = 0; index < shaderDescription.InputParameters; ++index)
        {
            D3D11_SIGNATURE_PARAMETER_DESC input{};
            if (FAILED(reflection->GetInputParameterDesc(index, &input)))
                return Failure<CompiledShaderStage>("shader-reflection", "GetInputParameterDesc failed");
            if (input.SystemValueType != D3D_NAME_UNDEFINED) continue;
            auto meaning = VertexMeaning(input);
            if (!meaning) return base::Result<CompiledShaderStage, CompileError>::Failure(meaning.Error());
            output.vertexInputs.push_back({meaning.Value(),
                static_cast<std::uint16_t>(std::popcount(static_cast<unsigned int>(input.Mask & 0x0f))),
                static_cast<std::uint16_t>(input.SemanticIndex)});
        }
    }

    auto interfaceValidation = ValidateReflectedInterface(program, output);
    if (!interfaceValidation)
        return base::Result<CompiledShaderStage, CompileError>::Failure(interfaceValidation.Error());

    ComPtr<ID3DBlob> stripped;
    constexpr UINT stripFlags = D3DCOMPILER_STRIP_REFLECTION_DATA |
                                D3DCOMPILER_STRIP_DEBUG_INFO |
                                D3DCOMPILER_STRIP_TEST_BLOBS |
                                D3DCOMPILER_STRIP_PRIVATE_DATA;
    if (FAILED(D3DStripShader(shader->GetBufferPointer(), shader->GetBufferSize(), stripFlags, &stripped)))
        return Failure<CompiledShaderStage>("shader-compilation", "D3DStripShader failed");
    const auto* begin = static_cast<const std::byte*>(stripped->GetBufferPointer());
    output.bytecode.assign(begin, begin + stripped->GetBufferSize());
    return base::Result<CompiledShaderStage, CompileError>::Success(std::move(output));
}

base::Result<std::vector<std::byte>, CompileError> SerializeRootSignature(
    const D3D12_ROOT_SIGNATURE_DESC& description,
    const char* stage)
{
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3D12SerializeRootSignature(
        &description, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    if (FAILED(result))
    {
        std::string message = "D3D12SerializeRootSignature failed";
        if (errors)
            message.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        return Failure<std::vector<std::byte>>(stage, std::move(message));
    }
    const auto* begin = static_cast<const std::byte*>(blob->GetBufferPointer());
    return base::Result<std::vector<std::byte>, CompileError>::Success(
        std::vector<std::byte>(begin, begin + blob->GetBufferSize()));
}

enum class NativeBindingKind { Constant, ShaderResource, UnorderedAccess };
struct NativeBinding final
{
    NativeBindingKind kind = NativeBindingKind::Constant;
    std::uint32_t shaderRegister = 0;
    semantic::ShaderStage stage = semantic::ShaderStage::Vertex;
};

base::Result<std::vector<std::byte>, CompileError> SerializeBindingLayout(
    std::span<const NativeBinding> bindings,
    bool raster,
    bool staticSampler)
{
    std::size_t descriptorCount = 0;
    for (const auto& binding : bindings)
        if (binding.kind != NativeBindingKind::Constant) ++descriptorCount;

    std::vector<D3D12_DESCRIPTOR_RANGE> ranges(descriptorCount);
    std::vector<D3D12_ROOT_PARAMETER> parameters(bindings.size());
    std::size_t rangeIndex = 0;
    for (std::size_t index = 0; index < bindings.size(); ++index)
    {
        const auto& binding = bindings[index];
        auto& parameter = parameters[index];
        parameter.ShaderVisibility = binding.stage == semantic::ShaderStage::Vertex ?
            D3D12_SHADER_VISIBILITY_VERTEX :
            binding.stage == semantic::ShaderStage::Pixel ?
            D3D12_SHADER_VISIBILITY_PIXEL : D3D12_SHADER_VISIBILITY_ALL;
        if (binding.kind == NativeBindingKind::Constant)
        {
            parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            parameter.Descriptor.ShaderRegister = binding.shaderRegister;
            parameter.Descriptor.RegisterSpace = 0;
            continue;
        }
        auto& range = ranges[rangeIndex++];
        range.RangeType = binding.kind == NativeBindingKind::ShaderResource ?
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = binding.shaderRegister;
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable.NumDescriptorRanges = 1;
        parameter.DescriptorTable.pDescriptorRanges = &range;
    }

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = raster ? D3D12_SHADER_VISIBILITY_PIXEL : D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(parameters.size());
    description.pParameters = parameters.empty() ? nullptr : parameters.data();
    description.NumStaticSamplers = staticSampler ? 1u : 0u;
    description.pStaticSamplers = staticSampler ? &sampler : nullptr;
    description.Flags = raster ? D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT :
                                 D3D12_ROOT_SIGNATURE_FLAG_NONE;
    return SerializeRootSignature(description, raster ? "raster-root-signature" : "compute-root-signature");
}

pkg::ResourceState Common() noexcept { return {pkg::StateClass::Common, 0, 0}; }
pkg::ResourceState Present() noexcept { return {pkg::StateClass::Present, 0, 0}; }
pkg::ResourceState Explicit(pkg::ExplicitStateBits bits) noexcept
{
    return {pkg::StateClass::Explicit, 0, static_cast<std::uint32_t>(bits)};
}

std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

std::uint64_t AppendAligned(std::vector<std::byte>& destination,
                            std::span<const std::byte> source,
                            std::size_t alignment = 16)
{
    while (destination.size() % alignment != 0) destination.push_back(std::byte{0});
    const auto offset = static_cast<std::uint64_t>(destination.size());
    destination.insert(destination.end(), source.begin(), source.end());
    return offset;
}

const semantic::ProgramParameter* FindParameter(
    const semantic::ProgramInterface& interfaceDescription,
    semantic::ProgramParameterId id) noexcept
{
    const auto found = std::find_if(interfaceDescription.parameters.begin(),
        interfaceDescription.parameters.end(),
        [id](const semantic::ProgramParameter& parameter) { return parameter.id == id; });
    return found == interfaceDescription.parameters.end() ? nullptr : &*found;
}

std::vector<const semantic::ProgramParameter*> CanonicalParameters(
    const semantic::ProgramInterface& interfaceDescription)
{
    std::vector<const semantic::ProgramParameter*> result;
    result.reserve(interfaceDescription.parameters.size());
    for (const auto& parameter : interfaceDescription.parameters) result.push_back(&parameter);
    std::sort(result.begin(), result.end(), [](const auto* left, const auto* right) {
        return left->id.value < right->id.value;
    });
    return result;
}

std::vector<const semantic::VertexInput*> CanonicalVertexInputs(
    const semantic::ProgramInterface& interfaceDescription)
{
    std::vector<const semantic::VertexInput*> result;
    result.reserve(interfaceDescription.vertexInputs.size());
    for (const auto& input : interfaceDescription.vertexInputs) result.push_back(&input);
    std::sort(result.begin(), result.end(), [](const auto* left, const auto* right) {
        return std::tuple{left->byteOffset, left->meaning, left->componentCount} <
               std::tuple{right->byteOffset, right->meaning, right->componentCount};
    });
    return result;
}

std::vector<const semantic::WorkOperand*> CanonicalOperands(const semantic::Work& work)
{
    std::vector<const semantic::WorkOperand*> result;
    result.reserve(work.operands.size());
    for (const auto& operand : work.operands) result.push_back(&operand);
    std::sort(result.begin(), result.end(), [](const auto* left, const auto* right) {
        return std::tuple{left->kind, left->parameter.value, left->use.value} <
               std::tuple{right->kind, right->parameter.value, right->use.value};
    });
    return result;
}

base::Digest256 InterfaceDigest(const semantic::ProgramInterface& value)
{
    base::BinaryWriter writer;
    writer.WriteU32(value.vertexStrideBytes);
    const auto vertexInputs = CanonicalVertexInputs(value);
    writer.WriteU32(static_cast<std::uint32_t>(vertexInputs.size()));
    for (const auto* input : vertexInputs)
    {
        writer.WriteU16(static_cast<std::uint16_t>(input->meaning));
        writer.WriteU16(input->componentCount);
        writer.WriteU32(input->byteOffset);
    }
    const auto parameters = CanonicalParameters(value);
    writer.WriteU32(static_cast<std::uint32_t>(parameters.size()));
    for (const auto* parameter : parameters)
    {
        writer.WriteU32(parameter->id.value);
        writer.WriteU16(static_cast<std::uint16_t>(parameter->kind));
        writer.WriteU16(static_cast<std::uint16_t>(parameter->stage));
        writer.WriteU32(parameter->shaderRegister);
        writer.WriteU64(parameter->requiredBytes);
        writer.WriteU32(parameter->requiredAlignment);
    }
    return base::Sha256(writer.Bytes());
}

base::Digest256 RasterSpecializationDigest(const pkg::RasterExecutableArtifact& executable)
{
    base::BinaryWriter writer;
    writer.WriteU32(executable.program.value);
    writer.WriteU32(executable.bindingLayout.value);
    writer.WriteU32(executable.vertexElementRange.first);
    writer.WriteU32(executable.vertexElementRange.count);
    writer.WriteU32(static_cast<std::uint32_t>(executable.colorFormat));
    writer.WriteU32(static_cast<std::uint32_t>(executable.depthFormat));
    writer.WriteU32(static_cast<std::uint32_t>(executable.primitiveTopology));
    writer.WriteU32(executable.rasterStateId);
    writer.WriteU32(executable.blendStateId);
    writer.WriteU32(executable.depthStateId);
    writer.WriteU32(executable.sampleCount);
    return base::Sha256(writer.Bytes());
}

base::Digest256 ComputeSpecializationDigest(const pkg::ComputeExecutableArtifact& executable)
{
    base::BinaryWriter writer;
    writer.WriteU32(executable.program.value);
    writer.WriteU32(executable.bindingLayout.value);
    writer.WriteU32(executable.flags);
    return base::Sha256(writer.Bytes());
}

pkg::ResourceKind ResourceKind(semantic::ResourceKind kind)
{
    switch (kind)
    {
    case semantic::ResourceKind::Buffer: return pkg::ResourceKind::Buffer;
    case semantic::ResourceKind::Texture2D: return pkg::ResourceKind::Texture2D;
    case semantic::ResourceKind::SurfaceImage: return pkg::ResourceKind::SurfaceImage;
    default: return pkg::ResourceKind::Buffer;
    }
}

pkg::Format Format(semantic::FormatMeaning format)
{
    switch (format)
    {
    case semantic::FormatMeaning::Bgra8Unorm: return pkg::Format::B8G8R8A8Unorm;
    case semantic::FormatMeaning::Depth32Float: return pkg::Format::D32Float;
    default: return pkg::Format::Unknown;
    }
}

pkg::Format ResourceFormat(const semantic::Resource& resource)
{
    if (resource.kind == semantic::ResourceKind::Texture2D) return Format(resource.texture2D.formatMeaning);
    if (resource.kind == semantic::ResourceKind::SurfaceImage) return Format(resource.surface.formatMeaning);
    return pkg::Format::Unknown;
}

pkg::ViewClass ViewClass(semantic::ViewRole role)
{
    using semantic::ViewRole;
    switch (role)
    {
    case ViewRole::VertexData: return pkg::ViewClass::VertexBuffer;
    case ViewRole::ConstantData: return pkg::ViewClass::ConstantBuffer;
    case ViewRole::SampledTexture:
    case ViewRole::ShaderBuffer: return pkg::ViewClass::ShaderResource;
    case ViewRole::ColorAttachment: return pkg::ViewClass::RenderTarget;
    case ViewRole::DepthAttachment: return pkg::ViewClass::DepthStencil;
    case ViewRole::StorageBuffer: return pkg::ViewClass::UnorderedAccess;
    case ViewRole::CopySource: return pkg::ViewClass::CopySource;
    case ViewRole::CopyDestination: return pkg::ViewClass::CopyDestination;
    case ViewRole::PresentSource: return pkg::ViewClass::PresentSource;
    default: return pkg::ViewClass::ShaderResource;
    }
}

pkg::ResourceState RequiredState(semantic::ViewRole role, semantic::WorkKind workKind)
{
    using semantic::ViewRole;
    switch (role)
    {
    case ViewRole::VertexData: return Explicit(pkg::ExplicitStateBits::VertexBuffer);
    case ViewRole::ConstantData: return Explicit(pkg::ExplicitStateBits::ConstantBuffer);
    case ViewRole::ColorAttachment: return Explicit(pkg::ExplicitStateBits::RenderTarget);
    case ViewRole::DepthAttachment: return Explicit(pkg::ExplicitStateBits::DepthWrite);
    case ViewRole::StorageBuffer: return Explicit(pkg::ExplicitStateBits::UnorderedWrite);
    case ViewRole::CopySource: return Explicit(pkg::ExplicitStateBits::CopySource);
    case ViewRole::CopyDestination: return Explicit(pkg::ExplicitStateBits::CopyDestination);
    case ViewRole::PresentSource: return Present();
    default:
        // Shader visibility is an execution decision and must be frozen into the
        // Package. Dedicated Compute queues cannot consume PIXEL_SHADER_RESOURCE.
        return Explicit(workKind == semantic::WorkKind::Compute
            ? pkg::ExplicitStateBits::NonPixelShaderRead
            : pkg::ExplicitStateBits::PixelShaderRead);
    }
}

bool DescriptorBacked(pkg::ViewClass viewClass)
{
    return viewClass == pkg::ViewClass::ShaderResource ||
           viewClass == pkg::ViewClass::UnorderedAccess ||
           viewClass == pkg::ViewClass::RenderTarget ||
           viewClass == pkg::ViewClass::DepthStencil;
}

pkg::HeapClass HeapClassFor(const semantic::Resource& resource,
                            std::span<const semantic::ResourceUse* const> uses)
{
    if (resource.update == semantic::UpdateIntent::DynamicPerFrame) return pkg::HeapClass::Upload;
    if (resource.kind == semantic::ResourceKind::Buffer) return pkg::HeapClass::DefaultBuffer;
    const bool attachment = std::any_of(uses.begin(), uses.end(), [](const semantic::ResourceUse* use) {
        return use->role == semantic::ViewRole::ColorAttachment ||
               use->role == semantic::ViewRole::DepthAttachment;
    });
    return attachment ? pkg::HeapClass::RenderTargetOrDepth : pkg::HeapClass::DefaultTexture;
}

struct ShaderIds final
{
    pkg::ShaderId vertex;
    pkg::ShaderId pixel;
    pkg::ShaderId compute;
};

struct WorkArtifacts final
{
    pkg::ProgramId program;
    pkg::BindingLayoutId layout;
    pkg::ExecutableId rasterExecutable;
    pkg::RasterCommandId rasterCommand;
    pkg::ComputeExecutableId computeExecutable;
    pkg::ComputeCommandId computeCommand;
};

struct StateCell final
{
    std::uint32_t resource = 0;
    std::uint32_t temporalRelation = 0; // 0 normal, 1 current, 2 previous
    auto operator<=>(const StateCell&) const = default;
};

struct LoweredPackageStage final
{
    pkg::D3D12PackageDescription description;
};

}

base::Result<void, CompileError> ValidateLevel2Capability(
    const semantic::SemanticGraph& graph,
    const target::D3D12TargetProfile& targetProfile)
{
    if (targetProfile.framesInFlight == 0 || targetProfile.directQueueCount != 1 ||
        targetProfile.computeQueueCount > 1 || targetProfile.copyQueueCount > 1 ||
        targetProfile.shaderBinaryFormat != target::ShaderBinaryFormat::Dxbc ||
        targetProfile.shaderModelMajor != 5 || targetProfile.shaderModelMinor > 1 ||
        targetProfile.rootSignatureMajor != 1 || targetProfile.rootSignatureMinor > 1 ||
        targetProfile.barrierModel != target::BarrierModel::Legacy)
        return Failure<void>("target-feasibility", "target profile is outside the Level 2 D3D12 v1 capability constitution");

    std::uint32_t surfaceCount = 0;
    bool hasTemporal = false;
    bool hasSurfaceRelativeResource = false;
    for (const auto& resource : graph.resources)
    {
        if (resource.kind == semantic::ResourceKind::SurfaceImage) ++surfaceCount;
        if (resource.lifetime == semantic::LifetimeIntent::Temporal) hasTemporal = true;
        if (resource.kind == semantic::ResourceKind::Texture2D &&
            resource.texture2D.extentMeaning == semantic::TextureExtentMeaning::PresentationSurface)
            hasSurfaceRelativeResource = true;
        if (resource.kind == semantic::ResourceKind::Texture2D && resource.texture2D.mipLevels != 1)
            return Failure<void>("target-feasibility", "Level 2 D3D12 v1 supports Texture2D with exactly one mip level");
        if (resource.kind == semantic::ResourceKind::Texture2D &&
            resource.texture2D.extentMeaning == semantic::TextureExtentMeaning::Fixed &&
            resource.texture2D.formatMeaning != semantic::FormatMeaning::Bgra8Unorm)
            return Failure<void>("target-feasibility", "fixed Texture2D is limited to Bgra8Unorm in Level 2 D3D12 v1");
        if (resource.kind == semantic::ResourceKind::Texture2D &&
            resource.texture2D.extentMeaning == semantic::TextureExtentMeaning::PresentationSurface &&
            resource.texture2D.formatMeaning != semantic::FormatMeaning::Depth32Float)
            return Failure<void>("target-feasibility", "surface-relative Texture2D is limited to Depth32Float in Level 2 D3D12 v1");
    }
    if (surfaceCount > 1)
        return Failure<void>("target-feasibility", "Level 2 D3D12 v1 supports at most one presentation SurfaceImage");
    if (surfaceCount != 0 && targetProfile.surfaceImageCount == 0)
        return Failure<void>("target-feasibility", "a graph with a SurfaceImage requires a non-zero surface image count");
    if (hasSurfaceRelativeResource && surfaceCount == 0)
        return Failure<void>("target-feasibility", "a surface-relative Texture2D requires a presentation SurfaceImage");
    if (hasTemporal && targetProfile.framesInFlight < 2)
        return Failure<void>("target-feasibility", "Temporal resources require at least two frames in flight");

    bool hasRaster = false;
    for (const auto& work : graph.works)
    {
        if (work.kind == semantic::WorkKind::Present)
            return Failure<void>("target-feasibility", "Level 2 D3D12 v1 uses the Raster PresentSource operand; standalone Present Work is deferred");
        if (work.kind == semantic::WorkKind::Raster)
        {
            hasRaster = true;
            std::uint32_t color = 0;
            std::uint32_t depth = 0;
            std::uint32_t vertex = 0;
            for (const auto& operand : work.operands)
            {
                if (operand.kind == semantic::WorkOperandKind::ColorAttachment) ++color;
                if (operand.kind == semantic::WorkOperandKind::DepthAttachment) ++depth;
                if (operand.kind == semantic::WorkOperandKind::VertexData) ++vertex;
            }
            if (vertex != 1 || color != 1 || depth > 1)
                return Failure<void>("target-feasibility", "Level 2 D3D12 v1 raster work requires one vertex buffer, one color surface, and at most one depth attachment");
        }
    }
    if (surfaceCount != 0 && !hasRaster)
        return Failure<void>("target-feasibility", "a presentation SurfaceImage must be consumed by at least one Raster Work");
    return base::Result<void, CompileError>::Success();
}

base::Result<ValidatedSourceStage, CompileError> ValidateSourceStage(
    const semantic::SemanticGraph& graph,
    const target::D3D12TargetProfile& targetProfile)
{
    auto analyzedResult = analysis::Analyze(graph);
    if (!analyzedResult)
    {
        std::string message;
        for (const auto& diagnostic : analyzedResult.Error())
        {
            if (!message.empty()) message += "; ";
            message += diagnostic.message;
        }
        return Failure<ValidatedSourceStage>("semantic-analysis", std::move(message));
    }
    auto capability = ValidateLevel2Capability(graph, targetProfile);
    if (!capability)
        return base::Result<ValidatedSourceStage, CompileError>::Failure(capability.Error());

    ValidatedSourceStage output;
    output.source = &graph;
    output.targetProfile = targetProfile;
    output.analyzed = std::move(analyzedResult).Value();
    return base::Result<ValidatedSourceStage, CompileError>::Success(std::move(output));
}

base::Result<ProgramCompilationStage, CompileError> CompileProgramStage(
    const ValidatedSourceStage& validated)
{
    if (validated.source == nullptr || validated.analyzed.source != validated.source)
        return Failure<ProgramCompilationStage>("shader-compilation", "validated source stage is detached from its SemanticGraph");

    std::map<std::uint32_t, const semantic::Program*> programs;
    for (const auto& program : validated.source->programs)
        programs[program.id.value] = &program;
    std::set<std::uint32_t> usedProgramIds;
    for (const auto& work : validated.source->works)
    {
        if (work.kind == semantic::WorkKind::Raster) usedProgramIds.insert(work.raster.program.value);
        if (work.kind == semantic::WorkKind::Compute) usedProgramIds.insert(work.compute.program.value);
    }

    ProgramCompilationStage output;
    for (const auto programId : validated.analyzed.canonicalProgramOrder)
    {
        if (!usedProgramIds.contains(programId.value)) continue;
        const auto found = programs.find(programId.value);
        if (found == programs.end())
            return Failure<ProgramCompilationStage>("shader-compilation", "canonical Program is missing from the validated source");
        const auto& program = *found->second;
        CompiledProgram compiled;
        compiled.sourceProgram = program.id;
        if (program.kind == semantic::ProgramKind::Raster)
        {
            auto vertex = CompileAndReflectShader(
                program, semantic::ShaderStage::Vertex, program.source.vertexEntry, "vs_5_1");
            if (!vertex) return base::Result<ProgramCompilationStage, CompileError>::Failure(vertex.Error());
            auto pixel = CompileAndReflectShader(
                program, semantic::ShaderStage::Pixel, program.source.pixelEntry, "ps_5_1");
            if (!pixel) return base::Result<ProgramCompilationStage, CompileError>::Failure(pixel.Error());
            compiled.shaders.push_back(std::move(vertex).Value());
            compiled.shaders.push_back(std::move(pixel).Value());
        }
        else
        {
            auto compute = CompileAndReflectShader(
                program, semantic::ShaderStage::Compute, program.source.computeEntry, "cs_5_1");
            if (!compute) return base::Result<ProgramCompilationStage, CompileError>::Failure(compute.Error());
            compiled.shaders.push_back(std::move(compute).Value());
        }
        output.programs.push_back(std::move(compiled));
    }
    return base::Result<ProgramCompilationStage, CompileError>::Success(std::move(output));
}

namespace
{
base::Result<LoweredPackageStage, CompileError> LowerPackageStage(
    const ValidatedSourceStage& validated,
    const ProgramCompilationStage& compiledPrograms)
{
    if (validated.source == nullptr)
        return Failure<LoweredPackageStage>("package-lowering", "validated source stage has no SemanticGraph");
    const auto& graph = *validated.source;
    const auto& targetProfile = validated.targetProfile;
    const auto& analyzed = validated.analyzed;

    std::map<std::uint32_t, const semantic::Resource*> resources;
    std::map<std::uint32_t, const semantic::ResourceUse*> uses;
    std::map<std::uint32_t, const semantic::Program*> programs;
    std::map<std::uint32_t, const semantic::Work*> works;
    for (const auto& value : graph.resources) resources[value.id.value] = &value;
    for (const auto& value : graph.resourceUses) uses[value.id.value] = &value;
    for (const auto& value : graph.programs) programs[value.id.value] = &value;
    for (const auto& value : graph.works) works[value.id.value] = &value;

    std::map<std::uint32_t, semantic::WorkKind> useWorkKinds;
    for (const auto& work : graph.works)
        for (const auto& operand : work.operands)
            useWorkKinds[operand.use.value] = work.kind;

    pkg::D3D12PackageDescription description;
    description.profile.minimumFeatureLevel = targetProfile.minimumFeatureLevel;
    description.profile.shaderModelMajor = targetProfile.shaderModelMajor;
    description.profile.shaderModelMinor = targetProfile.shaderModelMinor;
    description.profile.rootSignatureMajor = targetProfile.rootSignatureMajor;
    description.profile.rootSignatureMinor = targetProfile.rootSignatureMinor;
    description.profile.barrierModel = pkg::BarrierModel::Legacy;
    description.profile.shaderBinaryFormat = pkg::ShaderBinaryFormat::Dxbc;
    description.profile.framesInFlight = targetProfile.framesInFlight;
    description.profile.directQueueCount = targetProfile.directQueueCount;
    description.profile.computeQueueCount = targetProfile.computeQueueCount;
    description.profile.copyQueueCount = targetProfile.copyQueueCount;
    const bool packageHasSurface = std::any_of(graph.resources.begin(), graph.resources.end(),
        [](const semantic::Resource& resource) { return resource.kind == semantic::ResourceKind::SurfaceImage; });
    description.profile.surfaceImageCount = packageHasSurface ? targetProfile.surfaceImageCount : 0;
    description.profile.rtvDescriptorCount = targetProfile.rtvDescriptorCount;
    description.profile.dsvDescriptorCount = targetProfile.dsvDescriptorCount;
    description.profile.shaderDescriptorCount = targetProfile.shaderDescriptorCount;
    description.profile.samplerDescriptorCount = targetProfile.samplerDescriptorCount;

    std::map<std::uint32_t, pkg::ResourceId> resourceMap;
    std::map<std::uint32_t, std::vector<const semantic::ResourceUse*>> resourceUses;
    for (const auto& use : graph.resourceUses) resourceUses[use.resource.value].push_back(&use);
    for (auto& [resource, resourceUseList] : resourceUses)
        std::sort(resourceUseList.begin(), resourceUseList.end(), [](const auto* left, const auto* right) {
            return left->id.value < right->id.value;
        });

    std::map<std::uint32_t, std::uint32_t> physicalInstances;
    for (const auto resourceId : analyzed.canonicalResourceOrder)
    {
        const auto& source = *resources.at(resourceId.value);
        const pkg::ResourceId packageId{static_cast<std::uint32_t>(description.resources.size())};
        resourceMap[source.id.value] = packageId;

        pkg::ResourceArtifact artifact;
        artifact.id = packageId;
        artifact.resourceKind = ResourceKind(source.kind);
        artifact.format = ResourceFormat(source);
        artifact.sampleCount = 1;
        artifact.planeCount = 1;
        if (source.kind == semantic::ResourceKind::SurfaceImage)
        {
            artifact.origin = pkg::ResourceOrigin::Surface;
            artifact.rebuildPolicy = pkg::RebuildPolicy::RuntimeManaged;
            artifact.extentMode = pkg::ExtentMode::SurfaceRelative;
            artifact.physicalInstanceCount = 0;
            artifact.initialState = Present();
        }
        else if (source.lifetime == semantic::LifetimeIntent::External)
        {
            artifact.origin = pkg::ResourceOrigin::External;
            artifact.rebuildPolicy = pkg::RebuildPolicy::RequireExternalRebind;
            artifact.physicalInstanceCount = 1;
            artifact.initialState = Common();
        }
        else
        {
            artifact.origin = pkg::ResourceOrigin::PackageOwned;
            artifact.rebuildPolicy = pkg::RebuildPolicy::RecreateFromPackage;
            artifact.physicalInstanceCount =
                (source.lifetime == semantic::LifetimeIntent::FrameLocal ||
                 source.lifetime == semantic::LifetimeIntent::Temporal) ? targetProfile.framesInFlight : 1;
            artifact.initialState = source.update == semantic::UpdateIntent::DynamicPerFrame ?
                Explicit(pkg::ExplicitStateBits::ConstantBuffer) : Common();
        }
        physicalInstances[source.id.value] = artifact.physicalInstanceCount;
        if (source.lifetime == semantic::LifetimeIntent::FrameLocal)
            artifact.flags |= static_cast<std::uint32_t>(pkg::ResourceFlags::FrameLocal);
        if (source.lifetime == semantic::LifetimeIntent::Temporal)
            artifact.flags |= static_cast<std::uint32_t>(pkg::ResourceFlags::Temporal);

        if (source.kind == semantic::ResourceKind::Buffer)
        {
            artifact.sizeBytes = source.update == semantic::UpdateIntent::DynamicPerFrame ?
                AlignUp(source.buffer.sizeBytes, ConstantPlacementAlignment) : source.buffer.sizeBytes;
        }
        else if (source.kind == semantic::ResourceKind::Texture2D)
        {
            artifact.extentMode = source.texture2D.extentMeaning == semantic::TextureExtentMeaning::Fixed ?
                pkg::ExtentMode::Fixed : pkg::ExtentMode::SurfaceRelative;
            artifact.width = source.texture2D.width;
            artifact.height = source.texture2D.height;
            artifact.depthOrArraySize = 1;
            artifact.mipLevels = source.texture2D.mipLevels;
        }
        if (!source.initialContent.empty())
        {
            artifact.initialDataOffset = AppendAligned(description.initialData, source.initialContent);
            artifact.initialDataSize = source.initialContent.size();
        }
        description.resources.push_back(artifact);
    }

    std::map<std::uint32_t, pkg::ViewId> viewMap;
    std::uint32_t rtvDescriptors = 0;
    std::uint32_t dsvDescriptors = 0;
    std::uint32_t shaderDescriptors = 0;
    for (const auto resourceId : analyzed.canonicalResourceOrder)
    {
        const auto packageResource = resourceMap.at(resourceId.value);
        auto& resourceArtifact = description.resources[packageResource.value];
        resourceArtifact.firstView = static_cast<std::uint32_t>(description.views.size());
        const auto foundUses = resourceUses.find(resourceId.value);
        if (foundUses != resourceUses.end())
        {
            for (const auto* sourceUse : foundUses->second)
            {
                const auto& sourceResource = *resources.at(resourceId.value);
                pkg::ResourceViewArtifact view;
                view.id = {static_cast<std::uint32_t>(description.views.size())};
                view.resource = packageResource;
                view.viewClass = ViewClass(sourceUse->role);
                view.format = ResourceFormat(sourceResource);
                if (sourceResource.kind == semantic::ResourceKind::Buffer)
                {
                    view.byteSize = sourceResource.buffer.sizeBytes;
                    view.strideBytes = sourceResource.buffer.strideBytes;
                }
                else
                {
                    view.mipCount = sourceResource.kind == semantic::ResourceKind::Texture2D ?
                        sourceResource.texture2D.mipLevels : 1;
                    view.arrayLayerCount = 1;
                    view.planeCount = 1;
                }
                if (sourceResource.lifetime == semantic::LifetimeIntent::Temporal)
                    view.flags = static_cast<std::uint32_t>(
                        sourceUse->temporalRelation == semantic::TemporalRelation::Previous ?
                        pkg::ResourceViewFlags::TemporalPrevious : pkg::ResourceViewFlags::TemporalCurrent);

                if (DescriptorBacked(view.viewClass))
                {
                    const auto instances = sourceResource.kind == semantic::ResourceKind::SurfaceImage ?
                        targetProfile.surfaceImageCount : std::max(1u, physicalInstances[sourceResource.id.value]);
                    view.descriptorInstanceStride = instances > 1 ? 1u : 0u;
                    if (view.viewClass == pkg::ViewClass::RenderTarget)
                    {
                        view.descriptorHeapClass = 1;
                        view.descriptorIndex = rtvDescriptors;
                        rtvDescriptors += instances;
                    }
                    else if (view.viewClass == pkg::ViewClass::DepthStencil)
                    {
                        view.descriptorHeapClass = 3;
                        view.descriptorIndex = dsvDescriptors;
                        dsvDescriptors += instances;
                    }
                    else
                    {
                        view.descriptorHeapClass = 2;
                        view.descriptorIndex = shaderDescriptors;
                        shaderDescriptors += instances;
                    }
                }
                viewMap[sourceUse->id.value] = view.id;
                description.views.push_back(view);
            }
        }
        resourceArtifact.viewCount = static_cast<std::uint32_t>(description.views.size()) - resourceArtifact.firstView;
    }

    if (rtvDescriptors > targetProfile.rtvDescriptorCount ||
        dsvDescriptors > targetProfile.dsvDescriptorCount ||
        shaderDescriptors > targetProfile.shaderDescriptorCount)
        return Failure<LoweredPackageStage>("descriptor-planning", "generated descriptor plan exceeds the target profile");

    std::map<std::uint32_t, pkg::DynamicSlotId> dynamicSlots;
    std::map<std::uint32_t, pkg::ExternalSlotId> externalSlots;
    std::map<std::uint32_t, pkg::SurfaceSlotId> surfaceSlots;
    for (const auto resourceId : analyzed.canonicalResourceOrder)
    {
        const auto& source = *resources.at(resourceId.value);
        const auto packageResource = resourceMap.at(resourceId.value);
        if (source.update == semantic::UpdateIntent::DynamicPerFrame)
        {
            pkg::DynamicDataSlotArtifact slot;
            slot.id = {static_cast<std::uint32_t>(description.dynamicSlots.size())};
            slot.destinationResource = packageResource;
            slot.requiredBytes = source.dynamicData.requiredBytes;
            slot.requiredAlignment = source.dynamicData.requiredAlignment;
            dynamicSlots[source.id.value] = slot.id;
            description.dynamicSlots.push_back(slot);
        }
        if (source.lifetime == semantic::LifetimeIntent::External &&
            source.kind != semantic::ResourceKind::SurfaceImage)
        {
            pkg::ExternalResourceSlotArtifact slot;
            slot.id = {static_cast<std::uint32_t>(description.externalSlots.size())};
            slot.resource = packageResource;
            slot.requiredKind = ResourceKind(source.kind);
            slot.requiredFormat = ResourceFormat(source);
            slot.minimumBytes = source.kind == semantic::ResourceKind::Buffer ? source.buffer.sizeBytes : 0;
            // Stage F finalizes the boundary states from canonical first/last use,
            // not from ResourceUse registration order.
            slot.requiredIncomingState = Common();
            slot.guaranteedOutgoingState = Common();
            description.resources[packageResource.value].initialState = Common();
            externalSlots[source.id.value] = slot.id;
            description.externalSlots.push_back(slot);
        }
        if (source.kind == semantic::ResourceKind::SurfaceImage)
        {
            pkg::SurfaceSlotArtifact slot;
            slot.id = {static_cast<std::uint32_t>(description.surfaceSlots.size())};
            slot.imageResource = packageResource;
            slot.requiredFormat = ResourceFormat(source);
            slot.acquiredState = Present();
            slot.presentedState = Present();
            surfaceSlots[source.id.value] = slot.id;
            description.surfaceSlots.push_back(slot);
        }
    }

    // Stage D: alias intent belongs to Resource, not to a shader-input role.
    // SemanticAnalysis has already validated shape compatibility and exclusive
    // ownership of each Preparation resource.
    std::map<std::uint32_t, analysis::ResourceLifetime> lifetimes;
    for (const auto& lifetime : analyzed.resourceLifetimes)
        lifetimes[lifetime.resource.value] = lifetime;
    std::map<std::uint32_t, std::uint32_t> aliasPairs;
    for (const auto resourceId : analyzed.canonicalResourceOrder)
    {
        const auto& targetResource = *resources.at(resourceId.value);
        if (!targetResource.aliasPreparation.IsValid()) continue;
        const auto& preparation = *resources.at(targetResource.aliasPreparation.value);
        const auto& targetLifetime = lifetimes.at(targetResource.id.value);
        const auto& preparationLifetime = lifetimes.at(preparation.id.value);
        const bool nonOverlapping = !preparationLifetime.usedByWork || !targetLifetime.usedByWork ||
            preparationLifetime.lastUse < targetLifetime.firstUse ||
            targetLifetime.lastUse < preparationLifetime.firstUse;
        if (!nonOverlapping)
            return Failure<LoweredPackageStage>("allocation-planning", "explicit alias Resource lifetimes overlap");
        aliasPairs[preparation.id.value] = targetResource.id.value;
    }

    std::map<std::uint32_t, pkg::AllocationId> allocationForResource;
    std::set<std::uint32_t> allocatedResources;
    std::uint32_t aliasGroup = 0;
    for (const auto resourceId : analyzed.canonicalResourceOrder)
    {
        const auto packageResource = resourceMap.at(resourceId.value);
        const auto& source = *resources.at(resourceId.value);
        if (source.lifetime == semantic::LifetimeIntent::External || allocatedResources.contains(resourceId.value))
            continue;

        const auto pairAsPreparation = aliasPairs.find(resourceId.value);
        auto pairAsTarget = std::find_if(aliasPairs.begin(), aliasPairs.end(), [&](const auto& pair) {
            return pair.second == resourceId.value;
        });
        const bool aliased = pairAsPreparation != aliasPairs.end() || pairAsTarget != aliasPairs.end();
        std::uint32_t partnerId = package::InvalidIndex;
        if (pairAsPreparation != aliasPairs.end()) partnerId = pairAsPreparation->second;
        if (pairAsTarget != aliasPairs.end()) partnerId = pairAsTarget->first;

        const auto& sourceUseList = resourceUses[source.id.value];
        pkg::AllocationArtifact allocation;
        allocation.id = {static_cast<std::uint32_t>(description.allocations.size())};
        allocation.kind = aliased ? pkg::AllocationKind::Placed : pkg::AllocationKind::Committed;
        allocation.heapClass = HeapClassFor(source, sourceUseList);
        allocation.physicalInstanceCount = description.resources[packageResource.value].physicalInstanceCount;
        allocation.alignment = source.update == semantic::UpdateIntent::DynamicPerFrame ?
            ConstantPlacementAlignment : DefaultPlacementAlignment;
        allocation.sizeBytes = source.kind == semantic::ResourceKind::Buffer ?
            (aliased ? AlignUp(source.buffer.sizeBytes, DefaultPlacementAlignment) :
             description.resources[packageResource.value].sizeBytes) : 0;
        allocation.aliasGroup = aliased ? aliasGroup++ : package::InvalidIndex;
        description.allocations.push_back(allocation);

        allocationForResource[source.id.value] = allocation.id;
        description.resources[packageResource.value].allocation = allocation.id;
        allocatedResources.insert(source.id.value);
        if (aliased)
        {
            const auto partnerPackage = resourceMap.at(partnerId);
            if (description.resources[partnerPackage.value].physicalInstanceCount != allocation.physicalInstanceCount)
                return Failure<LoweredPackageStage>("allocation-planning", "alias candidates have incompatible physical instance counts");
            allocationForResource[partnerId] = allocation.id;
            description.resources[partnerPackage.value].allocation = allocation.id;
            description.resources[packageResource.value].flags |= static_cast<std::uint32_t>(pkg::ResourceFlags::Aliased);
            description.resources[partnerPackage.value].flags |= static_cast<std::uint32_t>(pkg::ResourceFlags::Aliased);
            allocatedResources.insert(partnerId);
        }
    }

    std::map<std::uint32_t, ShaderIds> shaderMap;
    for (const auto& compiledProgram : compiledPrograms.programs)
    {
        ShaderIds ids;
        for (const auto& shader : compiledProgram.shaders)
        {
            pkg::ShaderStage packageStage{};
            pkg::ShaderId* destination = nullptr;
            if (shader.stage == semantic::ShaderStage::Vertex)
            {
                packageStage = pkg::ShaderStage::Vertex;
                destination = &ids.vertex;
            }
            else if (shader.stage == semantic::ShaderStage::Pixel)
            {
                packageStage = pkg::ShaderStage::Pixel;
                destination = &ids.pixel;
            }
            else if (shader.stage == semantic::ShaderStage::Compute)
            {
                packageStage = pkg::ShaderStage::Compute;
                destination = &ids.compute;
            }
            else
            {
                return Failure<LoweredPackageStage>("package-lowering", "compiled shader stage is unknown");
            }
            *destination = {static_cast<std::uint32_t>(description.shaders.size())};
            const auto offset = AppendAligned(description.shaderData, shader.bytecode);
            description.shaders.push_back({*destination, packageStage, pkg::ShaderBinaryFormat::Dxbc,
                targetProfile.shaderModelMajor, targetProfile.shaderModelMinor, 0,
                {package::SectionKind::ShaderData, 0, offset, shader.bytecode.size()},
                base::Sha256(shader.bytecode)});
        }
        shaderMap[compiledProgram.sourceProgram.value] = ids;
    }

    std::map<std::uint32_t, WorkArtifacts> workArtifacts;
    std::uint32_t descriptorBindingOffset = 0;
    std::uint32_t staticSamplerOffset = 0;
    for (const auto workId : analyzed.canonicalWorkOrder)
    {
        const auto& work = *works.at(workId.value);
        if (work.kind != semantic::WorkKind::Raster && work.kind != semantic::WorkKind::Compute) continue;
        const auto sourceProgramId = work.kind == semantic::WorkKind::Raster ?
            work.raster.program : work.compute.program;
        const auto& sourceProgram = *programs.at(sourceProgramId.value);

        std::vector<const semantic::WorkOperand*> bindingOperands;
        for (const auto& operand : work.operands)
            if (operand.kind == semantic::WorkOperandKind::ProgramParameter)
                bindingOperands.push_back(&operand);
        std::sort(bindingOperands.begin(), bindingOperands.end(), [](const auto* left, const auto* right) {
            return left->parameter.value < right->parameter.value;
        });

        const std::uint32_t rootCost = static_cast<std::uint32_t>(std::count_if(
            bindingOperands.begin(), bindingOperands.end(), [&](const auto* operand) {
                return FindParameter(sourceProgram.interface, operand->parameter)->kind ==
                       semantic::ProgramParameterKind::ConstantBuffer;
            })) * 2u + static_cast<std::uint32_t>(std::count_if(
            bindingOperands.begin(), bindingOperands.end(), [&](const auto* operand) {
                return FindParameter(sourceProgram.interface, operand->parameter)->kind !=
                       semantic::ProgramParameterKind::ConstantBuffer;
            }));
        if (rootCost > 64)
            return Failure<LoweredPackageStage>("binding-layout", "root signature exceeds the D3D12 64-DWORD limit");

        const pkg::BindingLayoutId layoutId{static_cast<std::uint32_t>(description.bindingLayouts.size())};
        const auto parameterFirst = static_cast<std::uint32_t>(description.rootParameters.size());
        std::uint32_t descriptorCount = 0;
        std::vector<NativeBinding> nativeBindings;
        bool hasStaticSampler = false;
        for (std::uint32_t index = 0; index < bindingOperands.size(); ++index)
        {
            const auto* operand = bindingOperands[index];
            const auto* use = uses.at(operand->use.value);
            const auto* sourceParameterPointer = FindParameter(sourceProgram.interface, operand->parameter);
            if (sourceParameterPointer == nullptr)
                return Failure<LoweredPackageStage>("binding-layout", "validated ProgramParameter identity is missing");
            const auto& sourceParameter = *sourceParameterPointer;
            pkg::RootParameterArtifact parameter;
            parameter.id = {static_cast<std::uint32_t>(description.rootParameters.size())};
            parameter.rootParameterIndex = index;
            parameter.shaderRegister = sourceParameter.shaderRegister;
            parameter.visibility = sourceParameter.stage == semantic::ShaderStage::Vertex ?
                pkg::ShaderVisibility::Vertex :
                sourceParameter.stage == semantic::ShaderStage::Pixel ?
                pkg::ShaderVisibility::Pixel : pkg::ShaderVisibility::All;
            switch (sourceParameter.kind)
            {
            case semantic::ProgramParameterKind::ConstantBuffer:
                parameter.kind = pkg::RootParameterKind::ConstantBuffer;
                parameter.dynamicSlot = dynamicSlots.at(use->resource.value);
                nativeBindings.push_back({NativeBindingKind::Constant, parameter.shaderRegister, sourceParameter.stage});
                break;
            case semantic::ProgramParameterKind::UnorderedBuffer:
                parameter.kind = pkg::RootParameterKind::UnorderedAccessTable;
                parameter.staticView = viewMap.at(use->id.value);
                nativeBindings.push_back({NativeBindingKind::UnorderedAccess, parameter.shaderRegister, sourceParameter.stage});
                ++descriptorCount;
                break;
            case semantic::ProgramParameterKind::SampledTexture:
                hasStaticSampler = true;
                [[fallthrough]];
            case semantic::ProgramParameterKind::ReadOnlyBuffer:
                parameter.kind = pkg::RootParameterKind::ShaderResourceTable;
                parameter.staticView = viewMap.at(use->id.value);
                nativeBindings.push_back({NativeBindingKind::ShaderResource, parameter.shaderRegister, sourceParameter.stage});
                ++descriptorCount;
                break;
            default:
                return Failure<LoweredPackageStage>("binding-layout", "ProgramParameter kind is unsupported");
            }
            description.rootParameters.push_back(parameter);
        }

        auto rootBytes = SerializeBindingLayout(nativeBindings,
            work.kind == semantic::WorkKind::Raster, hasStaticSampler);
        if (!rootBytes) return base::Result<LoweredPackageStage, CompileError>::Failure(rootBytes.Error());
        const auto rootOffset = AppendAligned(description.nativeObjectData, rootBytes.Value());
        pkg::BindingLayoutArtifact layout;
        layout.id = layoutId;
        layout.rootSignatureMajor = targetProfile.rootSignatureMajor;
        layout.rootSignatureMinor = targetProfile.rootSignatureMinor;
        layout.parameterRange = {parameterFirst, static_cast<std::uint32_t>(bindingOperands.size())};
        layout.descriptorRange = {descriptorBindingOffset, descriptorCount};
        layout.staticSamplerRange = {staticSamplerOffset, hasStaticSampler ? 1u : 0u};
        layout.serializedRootSignature = {package::SectionKind::NativeObjectData, 0,
                                          rootOffset, rootBytes.Value().size()};
        layout.layoutDigest = base::Sha256(rootBytes.Value());
        description.bindingLayouts.push_back(layout);
        descriptorBindingOffset += descriptorCount;
        if (hasStaticSampler) ++staticSamplerOffset;

        WorkArtifacts artifacts;
        artifacts.layout = layoutId;
        artifacts.program = {static_cast<std::uint32_t>(description.programs.size())};
        pkg::ProgramArtifact programArtifact;
        programArtifact.id = artifacts.program;
        programArtifact.bindingLayout = layoutId;
        programArtifact.interfaceDigest = InterfaceDigest(sourceProgram.interface);
        const auto shaderIds = shaderMap.at(sourceProgram.id.value);
        if (work.kind == semantic::WorkKind::Raster)
        {
            programArtifact.kind = pkg::ProgramKind::Raster;
            programArtifact.vertexShader = shaderIds.vertex;
            programArtifact.pixelShader = shaderIds.pixel;
        }
        else
        {
            programArtifact.kind = pkg::ProgramKind::Compute;
            programArtifact.computeShader = shaderIds.compute;
        }
        description.programs.push_back(programArtifact);

        if (work.kind == semantic::WorkKind::Raster)
        {
            const auto vertexFirst = static_cast<std::uint32_t>(description.vertexElements.size());
            for (const auto* input : CanonicalVertexInputs(sourceProgram.interface))
            {
                pkg::VertexElementArtifact element;
                element.id = static_cast<std::uint32_t>(description.vertexElements.size());
                element.meaning = static_cast<pkg::VertexMeaning>(input->meaning);
                element.format = input->componentCount == 2 ? pkg::Format::R32G32Float :
                                 input->componentCount == 3 ? pkg::Format::R32G32B32Float :
                                                            pkg::Format::R32G32B32A32Float;
                element.alignedByteOffset = input->byteOffset;
                description.vertexElements.push_back(element);
            }

            const semantic::ResourceUse* vertex = nullptr;
            const semantic::ResourceUse* color = nullptr;
            const semantic::ResourceUse* depth = nullptr;
            for (const auto& operand : work.operands)
            {
                const auto* use = uses.at(operand.use.value);
                if (operand.kind == semantic::WorkOperandKind::VertexData) vertex = use;
                if (operand.kind == semantic::WorkOperandKind::ColorAttachment) color = use;
                if (operand.kind == semantic::WorkOperandKind::DepthAttachment) depth = use;
            }
            artifacts.rasterExecutable = {static_cast<std::uint32_t>(description.executables.size())};
            pkg::RasterExecutableArtifact executable;
            executable.id = artifacts.rasterExecutable;
            executable.program = artifacts.program;
            executable.bindingLayout = layoutId;
            executable.vertexElementRange = {vertexFirst, static_cast<std::uint32_t>(sourceProgram.interface.vertexInputs.size())};
            executable.colorFormatRange = {0, 1};
            executable.colorFormat = ResourceFormat(*resources.at(color->resource.value));
            executable.depthFormat = depth ? ResourceFormat(*resources.at(depth->resource.value)) : pkg::Format::Unknown;
            executable.primitiveTopology = pkg::PrimitiveTopology::TriangleList;
            executable.primitiveTopologyType = pkg::PrimitiveTopologyType::Triangle;
            executable.depthStateId = depth ? 1u : 0u;
            executable.sampleCount = 1;
            executable.specializationDigest = RasterSpecializationDigest(executable);
            description.executables.push_back(executable);

            pkg::AttachmentOperationArtifact attachment;
            attachment.id = {static_cast<std::uint32_t>(description.attachmentOperations.size())};
            attachment.depthLoad = depth ? pkg::AttachmentLoadOp::Clear : pkg::AttachmentLoadOp::Discard;
            attachment.depthStore = depth ? pkg::AttachmentStoreOp::Store : pkg::AttachmentStoreOp::Discard;
            description.attachmentOperations.push_back(attachment);

            artifacts.rasterCommand = {static_cast<std::uint32_t>(description.rasterCommands.size())};
            pkg::RasterCommandArtifact command;
            command.id = artifacts.rasterCommand;
            command.executable = artifacts.rasterExecutable;
            command.vertexViewRange = {viewMap.at(vertex->id.value).value, 1};
            command.colorAttachmentRange = {viewMap.at(color->id.value).value, 1};
            if (depth) command.depthAttachment = viewMap.at(depth->id.value);
            command.vertexCount = work.raster.vertexCount;
            command.instanceCount = 1;
            command.viewportId = 0;
            command.scissorId = 0;
            command.attachmentOperation = attachment.id;
            description.rasterCommands.push_back(command);
        }
        else
        {
            artifacts.computeExecutable = {static_cast<std::uint32_t>(description.computeExecutables.size())};
            pkg::ComputeExecutableArtifact executable;
            executable.id = artifacts.computeExecutable;
            executable.program = artifacts.program;
            executable.bindingLayout = layoutId;
            executable.specializationDigest = ComputeSpecializationDigest(executable);
            description.computeExecutables.push_back(executable);

            artifacts.computeCommand = {static_cast<std::uint32_t>(description.computeCommands.size())};
            description.computeCommands.push_back({artifacts.computeCommand, artifacts.computeExecutable,
                work.compute.threadGroupCountX, work.compute.threadGroupCountY,
                work.compute.threadGroupCountZ, 0});
        }
        workArtifacts[work.id.value] = artifacts;
    }

    const pkg::QueueId noQueue{};
    const pkg::QueueId directQueue{0};
    const pkg::QueueId computeQueue = targetProfile.computeQueueCount != 0 ?
        pkg::QueueId{targetProfile.directQueueCount} : directQueue;
    const pkg::QueueId copyQueue = targetProfile.copyQueueCount != 0 ?
        pkg::QueueId{targetProfile.directQueueCount + targetProfile.computeQueueCount} : directQueue;
    std::map<std::uint32_t, pkg::QueueId> workQueues;
    for (const auto workId : analyzed.canonicalWorkOrder)
    {
        const auto kind = works.at(workId.value)->kind;
        workQueues[workId.value] = kind == semantic::WorkKind::Copy ? copyQueue :
                                   kind == semantic::WorkKind::Compute ? computeQueue : directQueue;
    }

    // SignalPointId is local to one operation stream.  Frame points are assigned
    // before lowering so WaitTemporal can reference a producer that appears later
    // in the current frame but belongs to the previous frame generation.
    std::map<std::uint32_t, pkg::SignalPointId> workSignalPoints;
    std::uint32_t nextFrameSignalPoint = 0;
    for (const auto workId : analyzed.canonicalWorkOrder)
        workSignalPoints[workId.value] = {nextFrameSignalPoint++};
    const pkg::SignalPointId presentSignalPoint = description.surfaceSlots.empty() ?
        pkg::SignalPointId{} : pkg::SignalPointId{nextFrameSignalPoint++};

    std::map<std::uint32_t, std::uint32_t> temporalCurrentWriterWork;
    for (const auto workId : analyzed.canonicalWorkOrder)
    {
        const auto& work = *works.at(workId.value);
        for (const auto& operand : work.operands)
        {
            const auto* use = uses.at(operand.use.value);
            const auto& resource = *resources.at(use->resource.value);
            if (resource.lifetime == semantic::LifetimeIntent::Temporal &&
                use->temporalRelation == semantic::TemporalRelation::Current &&
                (use->effect == semantic::Effect::Write || use->effect == semantic::Effect::ReadWrite))
                temporalCurrentWriterWork[use->resource.value] = work.id.value;
        }
    }

    const auto addOperation = [&](pkg::D3D12OperationCode code, pkg::QueueId queue,
                                  std::vector<std::byte> payload = {})
    {
        const auto operationVersion = pkg::OperationVersion(code);
        description.operations.push_back({code, operationVersion, 0, queue, std::move(payload)});
    };

    addOperation(pkg::D3D12OperationCode::CreateDescriptorHeaps, noQueue);
    for (const auto& resource : description.resources)
        if (resource.origin == pkg::ResourceOrigin::PackageOwned)
            addOperation(pkg::D3D12OperationCode::CreateResource, noQueue,
                         pkg::Encode(pkg::CreateResourcePayload{resource.id}));

    std::vector<std::uint32_t> loadResources;
    for (const auto resourceId : analyzed.canonicalResourceOrder)
        if (resources.at(resourceId.value)->lifetime == semantic::LifetimeIntent::Preparation)
            loadResources.push_back(resourceId.value);
    for (const auto resourceId : analyzed.canonicalResourceOrder)
        if (resources.at(resourceId.value)->lifetime != semantic::LifetimeIntent::Preparation &&
            resources.at(resourceId.value)->lifetime != semantic::LifetimeIntent::External)
            loadResources.push_back(resourceId.value);

    const bool hasLoadWork = std::any_of(loadResources.begin(), loadResources.end(), [&](std::uint32_t id) {
        const auto packageResource = resourceMap.at(id);
        return description.resources[packageResource.value].initialDataSize != 0 ||
               (description.resources[packageResource.value].flags & static_cast<std::uint32_t>(pkg::ResourceFlags::Aliased)) != 0;
    });
    if (hasLoadWork)
    {
        addOperation(pkg::D3D12OperationCode::BeginQueueBatch, copyQueue);
        std::map<std::uint32_t, pkg::ResourceId> activeAlias;
        for (const auto sourceId : loadResources)
        {
            const auto packageResource = resourceMap.at(sourceId);
            const auto& source = *resources.at(sourceId);
            const auto& artifact = description.resources[packageResource.value];
            const bool aliased = (artifact.flags & static_cast<std::uint32_t>(pkg::ResourceFlags::Aliased)) != 0;
            if (aliased)
            {
                const auto allocation = description.allocations[artifact.allocation.value];
                auto before = activeAlias.find(allocation.aliasGroup);
                addOperation(pkg::D3D12OperationCode::ActivateAlias, copyQueue,
                    pkg::Encode(pkg::ActivateAliasPayload{
                        before == activeAlias.end() ? pkg::ResourceId{} : before->second, packageResource}));
                activeAlias[allocation.aliasGroup] = packageResource;
            }
            if (artifact.initialDataSize == 0) continue;
            addOperation(pkg::D3D12OperationCode::InitializeState, copyQueue,
                pkg::Encode(pkg::InitializeStatePayload{packageResource, artifact.initialState,
                    Explicit(pkg::ExplicitStateBits::CopyDestination)}));
            if (source.kind == semantic::ResourceKind::Buffer)
            {
                addOperation(pkg::D3D12OperationCode::UploadBuffer, copyQueue,
                    pkg::Encode(pkg::UploadBufferPayload{packageResource, artifact.initialDataOffset,
                                                         artifact.initialDataSize}));
                addOperation(pkg::D3D12OperationCode::InitializeState, copyQueue,
                    pkg::Encode(pkg::InitializeStatePayload{packageResource,
                        Explicit(pkg::ExplicitStateBits::CopyDestination),
                        Explicit(pkg::ExplicitStateBits::CopySource)}));
                addOperation(pkg::D3D12OperationCode::VerifyBufferContents, copyQueue,
                    pkg::Encode(pkg::VerifyBufferContentsPayload{packageResource, 0,
                        artifact.initialDataOffset, artifact.initialDataSize}));
            }
            else
            {
                addOperation(pkg::D3D12OperationCode::UploadTexture, copyQueue,
                    pkg::Encode(pkg::UploadTexturePayload{packageResource, artifact.initialDataOffset,
                        source.texture2D.rowBytes, static_cast<std::uint32_t>(artifact.initialDataSize), 0, 0, 0, 0}));
                addOperation(pkg::D3D12OperationCode::InitializeState, copyQueue,
                    pkg::Encode(pkg::InitializeStatePayload{packageResource,
                        Explicit(pkg::ExplicitStateBits::CopyDestination),
                        Explicit(pkg::ExplicitStateBits::CopySource)}));
                addOperation(pkg::D3D12OperationCode::VerifyTextureContents, copyQueue,
                    pkg::Encode(pkg::VerifyTextureContentsPayload{packageResource, artifact.initialDataOffset,
                        source.texture2D.rowBytes, source.texture2D.width, source.texture2D.height, 0}));
            }
            addOperation(pkg::D3D12OperationCode::InitializeState, copyQueue,
                pkg::Encode(pkg::InitializeStatePayload{packageResource,
                    Explicit(pkg::ExplicitStateBits::CopySource), Common()}));
        }
        addOperation(pkg::D3D12OperationCode::EndQueueBatch, copyQueue);
        addOperation(pkg::D3D12OperationCode::SignalQueue, copyQueue,
                     pkg::Encode(pkg::SignalQueuePayload{pkg::SignalPointId{0}}));
    }

    for (const auto& layout : description.bindingLayouts)
        addOperation(pkg::D3D12OperationCode::CreateRootSignature, noQueue,
                     pkg::Encode(pkg::CreateRootSignaturePayload{layout.id}));
    for (const auto& executable : description.executables)
        addOperation(pkg::D3D12OperationCode::CreateGraphicsPipeline, noQueue,
                     pkg::Encode(pkg::CreateGraphicsPipelinePayload{executable.id}));
    for (const auto& executable : description.computeExecutables)
        addOperation(pkg::D3D12OperationCode::CreateComputePipeline, noQueue,
                     pkg::Encode(pkg::CreateComputePipelinePayload{executable.id}));

    const auto loadCount = static_cast<std::uint32_t>(description.operations.size());
    for (const auto& slot : description.dynamicSlots)
        addOperation(pkg::D3D12OperationCode::ApplyDynamicData, noQueue,
                     pkg::Encode(pkg::ApplyDynamicDataPayload{slot.id}));
    for (const auto& slot : description.externalSlots)
        addOperation(pkg::D3D12OperationCode::AcquireExternal, noQueue,
                     pkg::Encode(pkg::AcquireExternalPayload{slot.id}));
    for (const auto& slot : description.surfaceSlots)
        addOperation(pkg::D3D12OperationCode::AcquireSurfaceImage, noQueue,
                     pkg::Encode(pkg::AcquireSurfaceImagePayload{slot.id}));

    std::map<std::uint32_t, std::uint32_t> workPosition;
    for (std::uint32_t index = 0; index < analyzed.canonicalWorkOrder.size(); ++index)
        workPosition[analyzed.canonicalWorkOrder[index].value] = index;
    std::map<std::uint32_t, std::uint32_t> firstExternalUse;
    std::map<std::uint32_t, std::uint32_t> lastExternalUse;
    std::map<std::uint32_t, const semantic::ResourceUse*> firstExternalUseContract;
    std::map<std::uint32_t, const semantic::ResourceUse*> lastExternalUseContract;
    for (std::uint32_t position = 0; position < analyzed.canonicalWorkOrder.size(); ++position)
    {
        const auto& work = *works.at(analyzed.canonicalWorkOrder[position].value);
        for (const auto* operand : CanonicalOperands(work))
        {
            const auto* use = uses.at(operand->use.value);
            if (!externalSlots.contains(use->resource.value)) continue;
            if (!firstExternalUse.contains(use->resource.value))
            {
                firstExternalUse[use->resource.value] = position;
                firstExternalUseContract[use->resource.value] = use;
            }
            lastExternalUse[use->resource.value] = position;
            lastExternalUseContract[use->resource.value] = use;
        }
    }
    for (const auto& [resourceId, slotId] : externalSlots)
    {
        const auto firstUse = firstExternalUseContract.at(resourceId);
        const auto lastUse = lastExternalUseContract.at(resourceId);
        auto& slot = description.externalSlots[slotId.value];
        slot.requiredIncomingState = RequiredState(firstUse->role,
            useWorkKinds.at(firstUse->id.value));
        slot.guaranteedOutgoingState = RequiredState(lastUse->role,
            useWorkKinds.at(lastUse->id.value));
        description.resources[slot.resource.value].initialState = slot.requiredIncomingState;
    }

    const auto cellFor = [&](const semantic::ResourceUse& use) {
        const auto& source = *resources.at(use.resource.value);
        std::uint32_t relation = 0;
        if (source.lifetime == semantic::LifetimeIntent::Temporal)
            relation = use.temporalRelation == semantic::TemporalRelation::Previous ? 2u : 1u;
        return StateCell{resourceMap.at(use.resource.value).value, relation};
    };
    const auto baselineState = [&](StateCell cell) {
        const auto& artifact = description.resources[cell.resource];
        if (artifact.initialDataSize != 0) return Common();
        return artifact.initialState;
    };
    std::map<StateCell, pkg::ResourceState> states;
    const auto currentState = [&](StateCell cell) {
        const auto found = states.find(cell);
        return found == states.end() ? baselineState(cell) : found->second;
    };
    const auto emitTransition = [&](pkg::QueueId queue, pkg::ViewId view, StateCell cell,
                                    pkg::ResourceState after)
    {
        const auto before = currentState(cell);
        if (before == after) return;
        addOperation(pkg::D3D12OperationCode::Transition, queue,
            pkg::Encode(pkg::TransitionPayload{view, 0, before, after}));
        states[cell] = after;
    };

    for (std::uint32_t position = 0; position < analyzed.canonicalWorkOrder.size(); ++position)
    {
        const auto workId = analyzed.canonicalWorkOrder[position];
        const auto& work = *works.at(workId.value);
        const auto queue = workQueues.at(work.id.value);

        // One wait per producer queue is sufficient, but it must reference the
        // latest producer Work on that queue, not whichever dependency happens
        // to appear first in the analysis record vector.
        std::map<std::uint32_t, std::pair<std::uint32_t, pkg::SignalPointId>> queueWaits;
        for (const auto& edge : analyzed.dependencies)
        {
            if (edge.consumer != work.id) continue;
            const auto producerQueue = workQueues.at(edge.producer.value);
            if (producerQueue == queue) continue;
            const auto producerPosition = workPosition.at(edge.producer.value);
            auto found = queueWaits.find(producerQueue.value);
            if (found == queueWaits.end() || producerPosition > found->second.first)
                queueWaits[producerQueue.value] = {producerPosition, workSignalPoints.at(edge.producer.value)};
        }
        for (const auto& [producerQueue, wait] : queueWaits)
            addOperation(pkg::D3D12OperationCode::WaitQueue, queue,
                         pkg::Encode(pkg::WaitQueuePayload{wait.second}));
        const auto canonicalOperands = CanonicalOperands(work);
        for (const auto* operand : canonicalOperands)
        {
            const auto* use = uses.at(operand->use.value);
            const auto first = firstExternalUse.find(use->resource.value);
            if (first != firstExternalUse.end() && first->second == position)
                addOperation(pkg::D3D12OperationCode::WaitExternal, queue,
                             pkg::Encode(pkg::WaitExternalPayload{externalSlots.at(use->resource.value)}));
            if (use->temporalRelation == semantic::TemporalRelation::Previous)
                addOperation(pkg::D3D12OperationCode::WaitTemporal, queue,
                             pkg::Encode(pkg::WaitTemporalPayload{resourceMap.at(use->resource.value),
                                 workSignalPoints.at(temporalCurrentWriterWork.at(use->resource.value))}));
        }

        addOperation(pkg::D3D12OperationCode::BeginQueueBatch, queue);
        std::map<StateCell, const semantic::ResourceUse*> activeUses;
        std::set<std::uint32_t> rasterPresentationResources;
        if (work.kind == semantic::WorkKind::Raster)
            for (const auto* operand : canonicalOperands)
            {
                const auto* use = uses.at(operand->use.value);
                if (operand->kind == semantic::WorkOperandKind::PresentSource)
                    rasterPresentationResources.insert(use->resource.value);
            }

        for (const auto* operand : canonicalOperands)
        {
            const auto* use = uses.at(operand->use.value);
            if (work.kind == semantic::WorkKind::Raster && use->role == semantic::ViewRole::PresentSource)
                continue;
            const auto cell = cellFor(*use);
            const auto required = RequiredState(use->role, work.kind);
            const auto existing = activeUses.find(cell);
            if (existing != activeUses.end() && RequiredState(existing->second->role, work.kind) != required)
                return Failure<LoweredPackageStage>("state-planning", "one Work requires incompatible states for the same state cell");
            activeUses[cell] = use;
            emitTransition(queue, viewMap.at(use->id.value), cell, required);
        }

        if (work.kind == semantic::WorkKind::Copy)
        {
            const semantic::ResourceUse* sourceUse = nullptr;
            const semantic::ResourceUse* destinationUse = nullptr;
            for (const auto* operand : canonicalOperands)
            {
                if (operand->kind == semantic::WorkOperandKind::CopySource)
                    sourceUse = uses.at(operand->use.value);
                if (operand->kind == semantic::WorkOperandKind::CopyDestination)
                    destinationUse = uses.at(operand->use.value);
            }
            if (sourceUse == nullptr || destinationUse == nullptr)
                return Failure<LoweredPackageStage>("operation-lowering", "validated Copy Work has missing operands");
            addOperation(pkg::D3D12OperationCode::ExecuteCopy, queue,
                pkg::Encode(pkg::CopyBufferPayload{viewMap.at(sourceUse->id.value),
                    viewMap.at(destinationUse->id.value), 0, 0, work.copy.bytes}));
        }
        else if (work.kind == semantic::WorkKind::Compute)
        {
            addOperation(pkg::D3D12OperationCode::ExecuteCompute, queue,
                pkg::Encode(pkg::ExecuteComputePayload{workArtifacts.at(work.id.value).computeCommand}));
        }
        else if (work.kind == semantic::WorkKind::Raster)
        {
            addOperation(pkg::D3D12OperationCode::ExecuteRaster, queue,
                pkg::Encode(pkg::ExecuteRasterPayload{workArtifacts.at(work.id.value).rasterCommand}));
            for (const auto* operand : canonicalOperands)
            {
                const auto* use = uses.at(operand->use.value);
                if (operand->kind != semantic::WorkOperandKind::PresentSource) continue;
                const auto cell = cellFor(*use);
                emitTransition(queue, viewMap.at(use->id.value), cell, Present());
                activeUses[cell] = use;
            }
        }

        for (const auto& [cell, use] : activeUses)
        {
            if (rasterPresentationResources.contains(use->resource.value)) continue;
            pkg::ResourceState desired = baselineState(cell);
            bool nextFound = false;
            for (std::uint32_t next = position + 1; next < analyzed.canonicalWorkOrder.size() && !nextFound; ++next)
            {
                const auto& nextWork = *works.at(analyzed.canonicalWorkOrder[next].value);
                for (const auto* nextOperand : CanonicalOperands(nextWork))
                {
                    const auto* nextUse = uses.at(nextOperand->use.value);
                    if (cellFor(*nextUse) != cell) continue;
                    desired = workQueues.at(nextWork.id.value) == queue ? RequiredState(nextUse->role, nextWork.kind) : Common();
                    nextFound = true;
                    break;
                }
            }
            if (!nextFound && externalSlots.contains(use->resource.value))
                desired = description.externalSlots[externalSlots.at(use->resource.value).value].guaranteedOutgoingState;
            emitTransition(queue, viewMap.at(use->id.value), cell, desired);
        }
        addOperation(pkg::D3D12OperationCode::EndQueueBatch, queue);
        addOperation(pkg::D3D12OperationCode::SignalQueue, queue,
                     pkg::Encode(pkg::SignalQueuePayload{workSignalPoints.at(work.id.value)}));
    }

    for (const auto& slot : description.surfaceSlots)
        addOperation(pkg::D3D12OperationCode::PresentSurface, noQueue,
                     pkg::Encode(pkg::PresentSurfacePayload{slot.id}));
    if (!description.surfaceSlots.empty())
        addOperation(pkg::D3D12OperationCode::SignalQueue, directQueue,
                     pkg::Encode(pkg::SignalQueuePayload{presentSignalPoint}));
    for (const auto& slot : description.externalSlots)
    {
        const auto sourceResource = std::find_if(resourceMap.begin(), resourceMap.end(), [&](const auto& pair) {
            return pair.second == slot.resource;
        });
        if (sourceResource == resourceMap.end())
            return Failure<LoweredPackageStage>("external-boundary", "external Package resource has no Source resource mapping");
        const auto lastPosition = lastExternalUse.at(sourceResource->first);
        const auto lastWork = analyzed.canonicalWorkOrder[lastPosition];
        addOperation(pkg::D3D12OperationCode::ReleaseExternal, noQueue,
                     pkg::Encode(pkg::ReleaseExternalPayload{slot.id, workSignalPoints.at(lastWork.value)}));
    }

    description.operationStreams.push_back({pkg::OperationStreamKind::Load, 0, loadCount, 0});
    description.operationStreams.push_back({pkg::OperationStreamKind::Frame, loadCount,
        static_cast<std::uint32_t>(description.operations.size()) - loadCount, 0});

    LoweredPackageStage output;
    output.description = std::move(description);
    return base::Result<LoweredPackageStage, CompileError>::Success(std::move(output));
}

base::Result<CompileOutput, CompileError> FreezePackageStage(LoweredPackageStage lowered)
{
    auto packageBytes = pkg::BuildFrozenPackage(lowered.description);
    if (!packageBytes)
        return Failure<CompileOutput>("package-serialization", packageBytes.Error().message);
    auto verified = package::PackageReader::Read(packageBytes.Value());
    if (!verified)
        return Failure<CompileOutput>("package-validation", verified.Error().message);
    auto decoded = pkg::D3D12PackageView::Decode(verified.Value());
    if (!decoded)
        return Failure<CompileOutput>("package-schema-validation", decoded.Error().message);

    CompileOutput output;
    output.executionDigestHex = base::ToHex(verified.Value().ExecutionDigest());
    output.packageBytes = std::move(packageBytes).Value();
    return base::Result<CompileOutput, CompileError>::Success(std::move(output));
}
}

base::Result<CompileOutput, CompileError> Compile(
    const semantic::SemanticGraph& graph,
    const target::D3D12TargetProfile& targetProfile)
{
    auto validated = ValidateSourceStage(graph, targetProfile);
    if (!validated)
        return base::Result<CompileOutput, CompileError>::Failure(validated.Error());
    auto programs = CompileProgramStage(validated.Value());
    if (!programs)
        return base::Result<CompileOutput, CompileError>::Failure(programs.Error());
    auto lowered = LowerPackageStage(validated.Value(), programs.Value());
    if (!lowered)
        return base::Result<CompileOutput, CompileError>::Failure(lowered.Error());
    auto frozen = FreezePackageStage(std::move(lowered).Value());
    if (!frozen)
        return frozen;
    frozen.Value().completedStages = {
        "source-validation",
        "shader-compilation-reflection",
        "package-lowering",
        "package-serialization-validation"};
    return frozen;
}
}
