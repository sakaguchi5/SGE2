#pragma once

#include "../00_Base/Result.h"
#include "../02_SemanticModel/SemanticModel.h"

#include <span>
#include <string>

namespace sge::semantic
{
class SemanticBuilder final
{
public:
    [[nodiscard]] base::Result<ResourceId, std::string> AddImmutableBuffer(
        std::string debugName,
        std::uint32_t strideBytes,
        std::span<const std::byte> initialContent);

    [[nodiscard]] base::Result<ResourceId, std::string> AddExternalBuffer(
        std::string debugName,
        std::uint64_t sizeBytes,
        std::uint32_t strideBytes);

    [[nodiscard]] base::Result<ResourceId, std::string> AddPreparationBuffer(
        std::string debugName,
        std::uint32_t strideBytes,
        std::span<const std::byte> initialContent);

    [[nodiscard]] base::Result<ResourceId, std::string> AddImmutableTexture2D(
        std::string debugName,
        std::uint32_t width,
        std::uint32_t height,
        FormatMeaning formatMeaning,
        std::uint32_t rowBytes,
        std::span<const std::byte> initialContent);

    [[nodiscard]] base::Result<ResourceId, std::string> AddDepthAttachmentTexture2D(
        std::string debugName,
        FormatMeaning formatMeaning);

    [[nodiscard]] base::Result<ResourceId, std::string> AddTemporalGpuWrittenBuffer(
        std::string debugName,
        std::uint32_t strideBytes,
        std::span<const std::byte> initialContent);

    [[nodiscard]] base::Result<ResourceId, std::string> AddPersistentGpuWrittenBuffer(
        std::string debugName,
        std::uint64_t sizeBytes,
        std::uint32_t strideBytes);

    [[nodiscard]] base::Result<ResourceId, std::string> AddGpuWrittenBuffer(
        std::string debugName,
        std::uint64_t sizeBytes,
        std::uint32_t strideBytes);

    [[nodiscard]] base::Result<ResourceId, std::string> AddDynamicBuffer(
        std::string debugName,
        std::uint64_t requiredBytes,
        std::uint32_t requiredAlignment);

    [[nodiscard]] base::Result<ResourceId, std::string> AddPresentationSurface(
        std::string debugName,
        FormatMeaning formatMeaning);

    [[nodiscard]] base::Result<ResourceUseId, std::string> AddUse(
        ResourceId resource,
        Effect effect,
        ViewRole role);

    [[nodiscard]] base::Result<ProgramId, std::string> AddRasterProgram(
        std::string debugName,
        ProgramInterface interfaceDescription,
        ProgramSource source);

    [[nodiscard]] base::Result<ProgramId, std::string> AddComputeProgram(
        std::string debugName,
        ProgramInterface interfaceDescription,
        ProgramSource source);

    [[nodiscard]] base::Result<WorkId, std::string> AddRasterWork(
        std::string debugName,
        ProgramId program,
        ResourceUseId vertexData,
        ResourceUseId constantData,
        ResourceUseId sampledTexture,
        ResourceUseId computedData,
        ResourceUseId copiedData,
        ResourceUseId aliasedData,
        ResourceUseId externalData,
        ResourceUseId colorAttachment,
        ResourceUseId depthAttachment,
        ResourceUseId presentSource,
        std::uint32_t vertexCount);

    // Cardinality-independent form. Resource roles in `uses` define vertex,
    // bindings, attachments, and presentation relations.
    [[nodiscard]] base::Result<WorkId, std::string> AddRasterWorkGeneric(
        std::string debugName,
        ProgramId program,
        std::span<const ResourceUseId> uses,
        std::uint32_t vertexCount);

    [[nodiscard]] base::Result<WorkId, std::string> AddCopyWork(
        std::string debugName,
        ResourceUseId source,
        ResourceUseId destination,
        std::uint64_t bytes);

    [[nodiscard]] base::Result<WorkId, std::string> AddComputeWork(
        std::string debugName,
        ProgramId program,
        ResourceUseId constantData,
        ResourceUseId previous,
        ResourceUseId output,
        std::uint32_t threadGroupCountX,
        std::uint32_t threadGroupCountY,
        std::uint32_t threadGroupCountZ);

    [[nodiscard]] base::Result<WorkId, std::string> AddComputeWorkGeneric(
        std::string debugName,
        ProgramId program,
        std::span<const ResourceUseId> uses,
        std::uint32_t threadGroupCountX,
        std::uint32_t threadGroupCountY,
        std::uint32_t threadGroupCountZ);

    [[nodiscard]] base::Result<WorkId, std::string> AddPresentWork(
        std::string debugName,
        ResourceUseId presentSource);

    [[nodiscard]] base::Result<void, std::string> AddDependency(
        WorkId predecessor,
        WorkId successor);

    [[nodiscard]] SemanticGraph Build() &&;

private:
    SemanticGraph graph_;
};
}
