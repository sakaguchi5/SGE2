#include "SemanticBuilder.h"

#include "../00_Base/CheckedMath.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace sge::semantic
{
namespace
{
template<class Id>
Id NextId(std::size_t size)
{
    return Id{static_cast<std::uint32_t>(size)};
}
}

base::Result<ResourceId, std::string> SemanticBuilder::AddImmutableBuffer(
    std::string debugName,
    std::uint32_t strideBytes,
    std::span<const std::byte> initialContent)
{
    if (initialContent.empty()) return base::Result<ResourceId, std::string>::Failure("immutable buffer requires initial content");
    if (strideBytes == 0 || initialContent.size() % strideBytes != 0)
        return base::Result<ResourceId, std::string>::Failure("buffer stride must divide initial content size");
    if (initialContent.size() > std::numeric_limits<std::uint32_t>::max())
        return base::Result<ResourceId, std::string>::Failure("buffer is too large for slice 12");

    const auto id = NextId<ResourceId>(graph_.resources.size());
    Resource resource;
    resource.id = id;
    resource.debugName = std::move(debugName);
    resource.kind = ResourceKind::Buffer;
    resource.lifetime = LifetimeIntent::Persistent;
    resource.update = UpdateIntent::Immutable;
    resource.visibility = Visibility::Internal;
    resource.buffer.sizeBytes = initialContent.size();
    resource.buffer.strideBytes = strideBytes;
    resource.initialContent.assign(initialContent.begin(), initialContent.end());
    graph_.resources.push_back(std::move(resource));
    return base::Result<ResourceId, std::string>::Success(id);
}

base::Result<ResourceId, std::string> SemanticBuilder::AddExternalBuffer(
    std::string debugName,
    std::uint64_t sizeBytes,
    std::uint32_t strideBytes)
{
    if (sizeBytes == 0 || strideBytes == 0 || sizeBytes % strideBytes != 0)
        return base::Result<ResourceId, std::string>::Failure("external buffer requires a positive stride-aligned byte count");
    const auto id = NextId<ResourceId>(graph_.resources.size());
    Resource resource;
    resource.id = id;
    resource.debugName = std::move(debugName);
    resource.kind = ResourceKind::Buffer;
    resource.lifetime = LifetimeIntent::External;
    resource.update = UpdateIntent::External;
    resource.visibility = Visibility::Published;
    resource.buffer.sizeBytes = sizeBytes;
    resource.buffer.strideBytes = strideBytes;
    graph_.resources.push_back(std::move(resource));
    return base::Result<ResourceId, std::string>::Success(id);
}

base::Result<ResourceId, std::string> SemanticBuilder::AddPreparationBuffer(
    std::string debugName,
    std::uint32_t strideBytes,
    std::span<const std::byte> initialContent)
{
    if (initialContent.empty()) return base::Result<ResourceId, std::string>::Failure("preparation buffer requires initial content");
    if (strideBytes == 0 || initialContent.size() % strideBytes != 0)
        return base::Result<ResourceId, std::string>::Failure("preparation buffer stride must divide initial content size");
    if (initialContent.size() > std::numeric_limits<std::uint32_t>::max())
        return base::Result<ResourceId, std::string>::Failure("preparation buffer is too large for slice 12");

    const auto id = NextId<ResourceId>(graph_.resources.size());
    Resource resource;
    resource.id = id;
    resource.debugName = std::move(debugName);
    resource.kind = ResourceKind::Buffer;
    resource.lifetime = LifetimeIntent::Preparation;
    resource.update = UpdateIntent::Immutable;
    resource.visibility = Visibility::Internal;
    resource.buffer.sizeBytes = initialContent.size();
    resource.buffer.strideBytes = strideBytes;
    resource.initialContent.assign(initialContent.begin(), initialContent.end());
    graph_.resources.push_back(std::move(resource));
    return base::Result<ResourceId, std::string>::Success(id);
}

base::Result<ResourceId, std::string> SemanticBuilder::AddImmutableTexture2D(
    std::string debugName,
    std::uint32_t width,
    std::uint32_t height,
    FormatMeaning formatMeaning,
    std::uint32_t rowBytes,
    std::span<const std::byte> initialContent)
{
    if (width == 0 || height == 0) return base::Result<ResourceId, std::string>::Failure("texture dimensions must be positive");
    if (formatMeaning != FormatMeaning::Bgra8Unorm)
        return base::Result<ResourceId, std::string>::Failure("slice 12 immutable Texture2D upload supports only Bgra8Unorm; depth initial data is forbidden");
    if (rowBytes == 0) return base::Result<ResourceId, std::string>::Failure("texture row byte count must be positive");
    const std::uint64_t expectedBytes = static_cast<std::uint64_t>(rowBytes) * height;
    if (expectedBytes != initialContent.size())
        return base::Result<ResourceId, std::string>::Failure("texture initial content does not match rowBytes * height");

    const auto id = NextId<ResourceId>(graph_.resources.size());
    Resource resource;
    resource.id = id;
    resource.debugName = std::move(debugName);
    resource.kind = ResourceKind::Texture2D;
    resource.lifetime = LifetimeIntent::Persistent;
    resource.update = UpdateIntent::Immutable;
    resource.visibility = Visibility::Internal;
    resource.texture2D.extentMeaning = TextureExtentMeaning::Fixed;
    resource.texture2D.width = width;
    resource.texture2D.height = height;
    resource.texture2D.formatMeaning = formatMeaning;
    resource.texture2D.rowBytes = rowBytes;
    resource.texture2D.mipLevels = 1;
    resource.initialContent.assign(initialContent.begin(), initialContent.end());
    graph_.resources.push_back(std::move(resource));
    return base::Result<ResourceId, std::string>::Success(id);
}

base::Result<ResourceId, std::string> SemanticBuilder::AddDepthAttachmentTexture2D(
    std::string debugName,
    FormatMeaning formatMeaning)
{
    if (formatMeaning != FormatMeaning::Depth32Float)
        return base::Result<ResourceId, std::string>::Failure("slice 12 supports only Depth32Float depth attachments");

    const auto id = NextId<ResourceId>(graph_.resources.size());
    Resource resource;
    resource.id = id;
    resource.debugName = std::move(debugName);
    resource.kind = ResourceKind::Texture2D;
    resource.lifetime = LifetimeIntent::Persistent;
    resource.update = UpdateIntent::GpuWritten;
    resource.visibility = Visibility::Internal;
    resource.texture2D.extentMeaning = TextureExtentMeaning::PresentationSurface;
    resource.texture2D.formatMeaning = formatMeaning;
    resource.texture2D.mipLevels = 1;
    graph_.resources.push_back(std::move(resource));
    return base::Result<ResourceId, std::string>::Success(id);
}

base::Result<ResourceId, std::string> SemanticBuilder::AddTemporalGpuWrittenBuffer(
    std::string debugName,
    std::uint32_t strideBytes,
    std::span<const std::byte> initialContent)
{
    if (initialContent.empty() || strideBytes == 0 || initialContent.size() % strideBytes != 0)
        return base::Result<ResourceId, std::string>::Failure("temporal buffer requires initial content divisible by stride");
    if (initialContent.size() > std::numeric_limits<std::uint32_t>::max())
        return base::Result<ResourceId, std::string>::Failure("temporal buffer is too large for slice 12");

    const auto id = NextId<ResourceId>(graph_.resources.size());
    Resource resource;
    resource.id = id;
    resource.debugName = std::move(debugName);
    resource.kind = ResourceKind::Buffer;
    resource.lifetime = LifetimeIntent::Temporal;
    resource.update = UpdateIntent::GpuWritten;
    resource.visibility = Visibility::Internal;
    resource.buffer.sizeBytes = initialContent.size();
    resource.buffer.strideBytes = strideBytes;
    resource.initialContent.assign(initialContent.begin(), initialContent.end());
    graph_.resources.push_back(std::move(resource));
    return base::Result<ResourceId, std::string>::Success(id);
}

base::Result<ResourceId, std::string> SemanticBuilder::AddPersistentGpuWrittenBuffer(
    std::string debugName,
    std::uint64_t sizeBytes,
    std::uint32_t strideBytes)
{
    if (sizeBytes == 0 || strideBytes == 0 || sizeBytes % strideBytes != 0)
        return base::Result<ResourceId, std::string>::Failure("persistent GPU-written buffer size must be positive and divisible by stride");
    if (sizeBytes > std::numeric_limits<std::uint32_t>::max())
        return base::Result<ResourceId, std::string>::Failure("persistent GPU-written buffer is too large for Level-1 qualification");

    const auto id = NextId<ResourceId>(graph_.resources.size());
    Resource resource;
    resource.id = id;
    resource.debugName = std::move(debugName);
    resource.kind = ResourceKind::Buffer;
    resource.lifetime = LifetimeIntent::Persistent;
    resource.update = UpdateIntent::GpuWritten;
    resource.visibility = Visibility::Internal;
    resource.buffer.sizeBytes = sizeBytes;
    resource.buffer.strideBytes = strideBytes;
    graph_.resources.push_back(std::move(resource));
    return base::Result<ResourceId, std::string>::Success(id);
}

base::Result<ResourceId, std::string> SemanticBuilder::AddGpuWrittenBuffer(
    std::string debugName,
    std::uint64_t sizeBytes,
    std::uint32_t strideBytes)
{
    if (sizeBytes == 0 || strideBytes == 0 || sizeBytes % strideBytes != 0)
        return base::Result<ResourceId, std::string>::Failure("GPU-written buffer size must be positive and divisible by stride");
    if (sizeBytes > std::numeric_limits<std::uint32_t>::max())
        return base::Result<ResourceId, std::string>::Failure("GPU-written buffer is too large for slice 12");

    const auto id = NextId<ResourceId>(graph_.resources.size());
    Resource resource;
    resource.id = id;
    resource.debugName = std::move(debugName);
    resource.kind = ResourceKind::Buffer;
    resource.lifetime = LifetimeIntent::FrameLocal;
    resource.update = UpdateIntent::GpuWritten;
    resource.visibility = Visibility::Internal;
    resource.buffer.sizeBytes = sizeBytes;
    resource.buffer.strideBytes = strideBytes;
    graph_.resources.push_back(std::move(resource));
    return base::Result<ResourceId, std::string>::Success(id);
}

base::Result<ResourceId, std::string> SemanticBuilder::AddDynamicBuffer(
    std::string debugName,
    std::uint64_t requiredBytes,
    std::uint32_t requiredAlignment)
{
    if (requiredBytes == 0) return base::Result<ResourceId, std::string>::Failure("dynamic buffer requires a positive byte count");
    if (!base::IsPowerOfTwo(requiredAlignment))
        return base::Result<ResourceId, std::string>::Failure("dynamic buffer alignment must be a power of two");

    const auto id = NextId<ResourceId>(graph_.resources.size());
    Resource resource;
    resource.id = id;
    resource.debugName = std::move(debugName);
    resource.kind = ResourceKind::Buffer;
    resource.lifetime = LifetimeIntent::FrameLocal;
    resource.update = UpdateIntent::DynamicPerFrame;
    resource.visibility = Visibility::Internal;
    resource.buffer.sizeBytes = requiredBytes;
    resource.dynamicData.requiredBytes = requiredBytes;
    resource.dynamicData.requiredAlignment = requiredAlignment;
    graph_.resources.push_back(std::move(resource));
    return base::Result<ResourceId, std::string>::Success(id);
}

base::Result<ResourceId, std::string> SemanticBuilder::AddPresentationSurface(
    std::string debugName,
    FormatMeaning formatMeaning)
{
    if (formatMeaning != FormatMeaning::Bgra8Unorm)
        return base::Result<ResourceId, std::string>::Failure("slice 12 presentation surface requires Bgra8Unorm");
    const auto id = NextId<ResourceId>(graph_.resources.size());
    Resource resource;
    resource.id = id;
    resource.debugName = std::move(debugName);
    resource.kind = ResourceKind::SurfaceImage;
    resource.lifetime = LifetimeIntent::External;
    resource.update = UpdateIntent::External;
    resource.visibility = Visibility::Published;
    resource.surface.formatMeaning = formatMeaning;
    graph_.resources.push_back(std::move(resource));
    return base::Result<ResourceId, std::string>::Success(id);
}

base::Result<ResourceUseId, std::string> SemanticBuilder::AddUse(ResourceId resource, Effect effect, ViewRole role)
{
    if (!resource.IsValid() || resource.value >= graph_.resources.size())
        return base::Result<ResourceUseId, std::string>::Failure("resource use references an unknown resource");
    const auto id = NextId<ResourceUseId>(graph_.resourceUses.size());
    graph_.resourceUses.push_back(ResourceUse{id, resource, effect, role});
    return base::Result<ResourceUseId, std::string>::Success(id);
}

base::Result<ProgramId, std::string> SemanticBuilder::AddRasterProgram(
    std::string debugName,
    ProgramInterface interfaceDescription,
    ProgramSource source)
{
    if (interfaceDescription.vertexInputs.empty() || interfaceDescription.vertexStrideBytes == 0)
        return base::Result<ProgramId, std::string>::Failure("raster program requires an explicit vertex interface");
    if (interfaceDescription.constantDataBytes != 0 && !base::IsPowerOfTwo(interfaceDescription.constantDataAlignment))
        return base::Result<ProgramId, std::string>::Failure("raster program constant-data alignment must be a power of two");
    if (source.hlslSource.empty() || source.vertexEntry.empty() || source.pixelEntry.empty())
        return base::Result<ProgramId, std::string>::Failure("raster program source and entry points must be explicit compiler inputs");
    const auto id = NextId<ProgramId>(graph_.programs.size());
    graph_.programs.push_back(Program{id, std::move(debugName), ProgramKind::Raster, std::move(interfaceDescription), std::move(source)});
    return base::Result<ProgramId, std::string>::Success(id);
}

base::Result<ProgramId, std::string> SemanticBuilder::AddComputeProgram(
    std::string debugName,
    ProgramInterface interfaceDescription,
    ProgramSource source)
{
    if (!interfaceDescription.vertexInputs.empty() || interfaceDescription.vertexStrideBytes != 0)
        return base::Result<ProgramId, std::string>::Failure("compute program cannot declare a raster vertex interface");
    if (interfaceDescription.constantDataBytes != 0 && !base::IsPowerOfTwo(interfaceDescription.constantDataAlignment))
        return base::Result<ProgramId, std::string>::Failure("compute program constant-data alignment must be a power of two");
    if (source.hlslSource.empty() || source.computeEntry.empty())
        return base::Result<ProgramId, std::string>::Failure("compute program source and entry point must be explicit compiler inputs");
    const auto id = NextId<ProgramId>(graph_.programs.size());
    graph_.programs.push_back(Program{id, std::move(debugName), ProgramKind::Compute, std::move(interfaceDescription), std::move(source)});
    return base::Result<ProgramId, std::string>::Success(id);
}

base::Result<WorkId, std::string> SemanticBuilder::AddRasterWork(
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
    std::uint32_t vertexCount)
{
    if (!program.IsValid() || program.value >= graph_.programs.size())
        return base::Result<WorkId, std::string>::Failure("raster work references an unknown program");
    const auto validUse = [this](ResourceUseId id) { return id.IsValid() && id.value < graph_.resourceUses.size(); };
    if (!validUse(vertexData) || !validUse(constantData) || !validUse(sampledTexture) || !validUse(computedData) || !validUse(copiedData) || !validUse(aliasedData) || !validUse(externalData) || !validUse(colorAttachment) || !validUse(depthAttachment) || !validUse(presentSource))
        return base::Result<WorkId, std::string>::Failure("raster work references an unknown resource use");
    if (vertexCount == 0) return base::Result<WorkId, std::string>::Failure("raster work requires a positive vertex count");

    const auto id = NextId<WorkId>(graph_.works.size());
    Work work;
    work.id = id;
    work.debugName = std::move(debugName);
    work.kind = WorkKind::Raster;
    work.uses = {vertexData, constantData, sampledTexture, computedData, copiedData, aliasedData, externalData, colorAttachment, depthAttachment, presentSource};
    work.raster = RasterPayload{program, vertexData, constantData, sampledTexture, computedData, copiedData, aliasedData, externalData, colorAttachment, depthAttachment, presentSource, vertexCount};
    graph_.works.push_back(std::move(work));
    return base::Result<WorkId, std::string>::Success(id);
}

base::Result<WorkId, std::string> SemanticBuilder::AddCopyWork(
    std::string debugName,
    ResourceUseId source,
    ResourceUseId destination,
    std::uint64_t bytes)
{
    const auto validUse = [this](ResourceUseId id) { return id.IsValid() && id.value < graph_.resourceUses.size(); };
    if (!validUse(source) || !validUse(destination))
        return base::Result<WorkId, std::string>::Failure("copy work references an unknown resource use");
    if (bytes == 0)
        return base::Result<WorkId, std::string>::Failure("copy work byte count must be positive");

    const auto id = NextId<WorkId>(graph_.works.size());
    Work work;
    work.id = id;
    work.debugName = std::move(debugName);
    work.kind = WorkKind::Copy;
    work.uses = {source, destination};
    work.copy = CopyPayload{source, destination, bytes};
    graph_.works.push_back(std::move(work));
    return base::Result<WorkId, std::string>::Success(id);
}

base::Result<WorkId, std::string> SemanticBuilder::AddRasterWorkGeneric(
    std::string debugName,
    ProgramId program,
    std::span<const ResourceUseId> uses,
    std::uint32_t vertexCount)
{
    if (!program.IsValid() || program.value >= graph_.programs.size() ||
        graph_.programs[program.value].kind != ProgramKind::Raster)
        return base::Result<WorkId, std::string>::Failure("raster work references an unknown raster program");
    if (uses.empty() || vertexCount == 0)
        return base::Result<WorkId, std::string>::Failure("raster work requires uses and a positive vertex count");
    for (const auto use : uses)
        if (!use.IsValid() || use.value >= graph_.resourceUses.size())
            return base::Result<WorkId, std::string>::Failure("raster work references an unknown resource use");

    const auto id = NextId<WorkId>(graph_.works.size());
    Work work;
    work.id = id;
    work.debugName = std::move(debugName);
    work.kind = WorkKind::Raster;
    work.uses.assign(uses.begin(), uses.end());
    work.raster.program = program;
    work.raster.vertexCount = vertexCount;
    graph_.works.push_back(std::move(work));
    return base::Result<WorkId, std::string>::Success(id);
}

base::Result<WorkId, std::string> SemanticBuilder::AddComputeWork(
    std::string debugName,
    ProgramId program,
    ResourceUseId constantData,
    ResourceUseId previous,
    ResourceUseId output,
    std::uint32_t threadGroupCountX,
    std::uint32_t threadGroupCountY,
    std::uint32_t threadGroupCountZ)
{
    if (!program.IsValid() || program.value >= graph_.programs.size() || graph_.programs[program.value].kind != ProgramKind::Compute)
        return base::Result<WorkId, std::string>::Failure("compute work references an unknown compute program");
    const auto validUse = [this](ResourceUseId id) { return id.IsValid() && id.value < graph_.resourceUses.size(); };
    if (!validUse(constantData) || !validUse(previous) || !validUse(output))
        return base::Result<WorkId, std::string>::Failure("compute work references an unknown resource use");
    if (threadGroupCountX == 0 || threadGroupCountY == 0 || threadGroupCountZ == 0)
        return base::Result<WorkId, std::string>::Failure("compute work dispatch dimensions must be positive");

    const auto id = NextId<WorkId>(graph_.works.size());
    Work work;
    work.id = id;
    work.debugName = std::move(debugName);
    work.kind = WorkKind::Compute;
    work.uses = {constantData, previous, output};
    work.compute = ComputePayload{program, constantData, previous, output, threadGroupCountX, threadGroupCountY, threadGroupCountZ};
    graph_.works.push_back(std::move(work));
    return base::Result<WorkId, std::string>::Success(id);
}

base::Result<WorkId, std::string> SemanticBuilder::AddComputeWorkGeneric(
    std::string debugName,
    ProgramId program,
    std::span<const ResourceUseId> uses,
    std::uint32_t threadGroupCountX,
    std::uint32_t threadGroupCountY,
    std::uint32_t threadGroupCountZ)
{
    if (!program.IsValid() || program.value >= graph_.programs.size() ||
        graph_.programs[program.value].kind != ProgramKind::Compute)
        return base::Result<WorkId, std::string>::Failure("compute work references an unknown compute program");
    if (uses.empty() || threadGroupCountX == 0 || threadGroupCountY == 0 || threadGroupCountZ == 0)
        return base::Result<WorkId, std::string>::Failure("compute work requires uses and positive dispatch dimensions");
    for (const auto use : uses)
        if (!use.IsValid() || use.value >= graph_.resourceUses.size())
            return base::Result<WorkId, std::string>::Failure("compute work references an unknown resource use");

    const auto id = NextId<WorkId>(graph_.works.size());
    Work work;
    work.id = id;
    work.debugName = std::move(debugName);
    work.kind = WorkKind::Compute;
    work.uses.assign(uses.begin(), uses.end());
    work.compute.program = program;
    work.compute.threadGroupCountX = threadGroupCountX;
    work.compute.threadGroupCountY = threadGroupCountY;
    work.compute.threadGroupCountZ = threadGroupCountZ;
    graph_.works.push_back(std::move(work));
    return base::Result<WorkId, std::string>::Success(id);
}

base::Result<WorkId, std::string> SemanticBuilder::AddPresentWork(
    std::string debugName,
    ResourceUseId presentSource)
{
    if (!presentSource.IsValid() || presentSource.value >= graph_.resourceUses.size())
        return base::Result<WorkId, std::string>::Failure("present work references an unknown resource use");
    const auto id = NextId<WorkId>(graph_.works.size());
    Work work;
    work.id = id;
    work.debugName = std::move(debugName);
    work.kind = WorkKind::Present;
    work.uses = {presentSource};
    graph_.works.push_back(std::move(work));
    return base::Result<WorkId, std::string>::Success(id);
}

base::Result<void, std::string> SemanticBuilder::AddDependency(
    WorkId predecessor,
    WorkId successor)
{
    if (!predecessor.IsValid() || !successor.IsValid() || predecessor == successor ||
        predecessor.value >= graph_.works.size() || successor.value >= graph_.works.size())
        return base::Result<void, std::string>::Failure("dependency references an unknown or identical Work");
    auto& dependencies = graph_.works[successor.value].dependencies;
    if (std::find(dependencies.begin(), dependencies.end(), predecessor) == dependencies.end())
        dependencies.push_back(predecessor);
    return base::Result<void, std::string>::Success();
}

SemanticGraph SemanticBuilder::Build() && { return std::move(graph_); }
}
