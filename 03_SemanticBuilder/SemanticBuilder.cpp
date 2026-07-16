#include "SemanticBuilder.h"

#include "../00_Base/CheckedMath.h"

#include <algorithm>
#include <limits>
#include <set>
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

base::Result<void, std::string> SemanticBuilder::SetAliasPreparation(
    ResourceId target,
    ResourceId preparation)
{
    if (!target.IsValid() || target.value >= graph_.resources.size() ||
        !preparation.IsValid() || preparation.value >= graph_.resources.size())
        return base::Result<void, std::string>::Failure("alias contract references an unknown resource");
    if (target == preparation)
        return base::Result<void, std::string>::Failure("alias target and preparation resource must differ");
    if (graph_.resources[preparation.value].lifetime != LifetimeIntent::Preparation)
        return base::Result<void, std::string>::Failure("alias preparation resource must have Preparation lifetime");
    if (graph_.resources[target.value].lifetime == LifetimeIntent::Preparation ||
        graph_.resources[target.value].lifetime == LifetimeIntent::External)
        return base::Result<void, std::string>::Failure("alias target must be a package-owned frame resource");
    if (graph_.resources[target.value].aliasPreparation.IsValid())
        return base::Result<void, std::string>::Failure("alias target already has a preparation resource");
    graph_.resources[target.value].aliasPreparation = preparation;
    return base::Result<void, std::string>::Success();
}

base::Result<ResourceUseId, std::string> SemanticBuilder::AddUse(
    ResourceId resource,
    Effect effect,
    ViewRole role,
    TemporalRelation temporalRelation)
{
    if (!resource.IsValid() || resource.value >= graph_.resources.size())
        return base::Result<ResourceUseId, std::string>::Failure("resource use references an unknown resource");
    const auto id = NextId<ResourceUseId>(graph_.resourceUses.size());
    graph_.resourceUses.push_back(ResourceUse{id, resource, effect, role, temporalRelation});
    return base::Result<ResourceUseId, std::string>::Success(id);
}

namespace
{
bool ValidateParameterIds(const ProgramInterface& interfaceDescription)
{
    std::set<std::uint32_t> identities;
    for (const auto& parameter : interfaceDescription.parameters)
        if (!parameter.id.IsValid() ||
            parameter.id.value >= interfaceDescription.parameters.size() ||
            !identities.insert(parameter.id.value).second)
            return false;
    return identities.size() == interfaceDescription.parameters.size();
}
}

base::Result<ProgramId, std::string> SemanticBuilder::AddRasterProgram(
    std::string debugName,
    ProgramInterface interfaceDescription,
    ProgramSource source)
{
    if (interfaceDescription.vertexInputs.empty() || interfaceDescription.vertexStrideBytes == 0)
        return base::Result<ProgramId, std::string>::Failure("raster program requires an explicit vertex interface");
    if (!ValidateParameterIds(interfaceDescription))
        return base::Result<ProgramId, std::string>::Failure("raster program parameter IDs must be unique and dense");
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
    if (!ValidateParameterIds(interfaceDescription))
        return base::Result<ProgramId, std::string>::Failure("compute program parameter IDs must be unique and dense");
    if (source.hlslSource.empty() || source.computeEntry.empty())
        return base::Result<ProgramId, std::string>::Failure("compute program source and entry point must be explicit compiler inputs");
    const auto id = NextId<ProgramId>(graph_.programs.size());
    graph_.programs.push_back(Program{id, std::move(debugName), ProgramKind::Compute, std::move(interfaceDescription), std::move(source)});
    return base::Result<ProgramId, std::string>::Success(id);
}

base::Result<void, std::string> SemanticBuilder::ValidateOperands(
    ProgramId program,
    std::span<const WorkOperand> operands,
    ProgramKind expectedKind) const
{
    if (!program.IsValid() || program.value >= graph_.programs.size() ||
        graph_.programs[program.value].kind != expectedKind)
        return base::Result<void, std::string>::Failure("work references an unknown program of the required kind");
    if (operands.empty())
        return base::Result<void, std::string>::Failure("work requires at least one operand");
    for (const auto& operand : operands)
    {
        if (!operand.use.IsValid() || operand.use.value >= graph_.resourceUses.size())
            return base::Result<void, std::string>::Failure("work operand references an unknown ResourceUse");
        if (operand.kind == WorkOperandKind::ProgramParameter &&
            (!operand.parameter.IsValid() || operand.parameter.value >= graph_.programs[program.value].interface.parameters.size()))
            return base::Result<void, std::string>::Failure("work operand references an unknown ProgramParameter");
        if (operand.kind != WorkOperandKind::ProgramParameter && operand.parameter.IsValid())
            return base::Result<void, std::string>::Failure("non-parameter work operand cannot carry a ProgramParameter ID");
    }
    return base::Result<void, std::string>::Success();
}

base::Result<WorkId, std::string> SemanticBuilder::AddRasterWorkGeneric(
    std::string debugName,
    ProgramId program,
    std::span<const WorkOperand> operands,
    std::uint32_t vertexCount)
{
    auto valid = ValidateOperands(program, operands, ProgramKind::Raster);
    if (!valid) return base::Result<WorkId, std::string>::Failure(valid.Error());
    if (vertexCount == 0)
        return base::Result<WorkId, std::string>::Failure("raster work requires a positive vertex count");

    const auto id = NextId<WorkId>(graph_.works.size());
    Work work;
    work.id = id;
    work.debugName = std::move(debugName);
    work.kind = WorkKind::Raster;
    work.operands.assign(operands.begin(), operands.end());
    work.raster.program = program;
    work.raster.vertexCount = vertexCount;
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
        return base::Result<WorkId, std::string>::Failure("copy work references an unknown ResourceUse");
    if (bytes == 0)
        return base::Result<WorkId, std::string>::Failure("copy work byte count must be positive");

    const auto id = NextId<WorkId>(graph_.works.size());
    Work work;
    work.id = id;
    work.debugName = std::move(debugName);
    work.kind = WorkKind::Copy;
    work.operands = {
        WorkOperand{WorkOperandKind::CopySource, source, {}},
        WorkOperand{WorkOperandKind::CopyDestination, destination, {}}};
    work.copy.bytes = bytes;
    graph_.works.push_back(std::move(work));
    return base::Result<WorkId, std::string>::Success(id);
}

base::Result<WorkId, std::string> SemanticBuilder::AddComputeWorkGeneric(
    std::string debugName,
    ProgramId program,
    std::span<const WorkOperand> operands,
    std::uint32_t threadGroupCountX,
    std::uint32_t threadGroupCountY,
    std::uint32_t threadGroupCountZ)
{
    auto valid = ValidateOperands(program, operands, ProgramKind::Compute);
    if (!valid) return base::Result<WorkId, std::string>::Failure(valid.Error());
    if (threadGroupCountX == 0 || threadGroupCountY == 0 || threadGroupCountZ == 0)
        return base::Result<WorkId, std::string>::Failure("compute work dispatch dimensions must be positive");

    const auto id = NextId<WorkId>(graph_.works.size());
    Work work;
    work.id = id;
    work.debugName = std::move(debugName);
    work.kind = WorkKind::Compute;
    work.operands.assign(operands.begin(), operands.end());
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
        return base::Result<WorkId, std::string>::Failure("present work references an unknown ResourceUse");
    const auto id = NextId<WorkId>(graph_.works.size());
    Work work;
    work.id = id;
    work.debugName = std::move(debugName);
    work.kind = WorkKind::Present;
    work.operands = {WorkOperand{WorkOperandKind::PresentSource, presentSource, {}}};
    graph_.works.push_back(std::move(work));
    return base::Result<WorkId, std::string>::Success(id);
}

base::Result<void, std::string> SemanticBuilder::AddDependency(
    WorkId predecessor,
    WorkId successor)
{
    if (!predecessor.IsValid() || !successor.IsValid() || predecessor == successor ||
        predecessor.value >= graph_.works.size() || successor.value >= graph_.works.size())
        return base::Result<void, std::string>::Failure("dependency references an unknown Work or self");
    auto& dependencies = graph_.works[successor.value].dependencies;
    if (std::find(dependencies.begin(), dependencies.end(), predecessor) != dependencies.end())
        return base::Result<void, std::string>::Failure("dependency is duplicated");
    dependencies.push_back(predecessor);
    return base::Result<void, std::string>::Success();
}

SemanticGraph SemanticBuilder::Build() &&
{
    return std::move(graph_);
}
}
