#include "SemanticAnalysis.h"

#include "../00_Base/CheckedMath.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace sge::analysis
{
namespace
{
using namespace semantic;

void Push(std::vector<Diagnostic>& diagnostics, std::string message,
          std::uint32_t source = base::InvalidIndex)
{
    diagnostics.push_back({std::move(message), source});
}

template<class T, class IdFn>
bool IndexById(const std::vector<T>& values, std::map<std::uint32_t, const T*>& output,
               IdFn id, const char* label, std::vector<Diagnostic>& diagnostics)
{
    bool valid = true;
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        const auto valueId = id(values[index]);
        if (!valueId.IsValid())
        {
            Push(diagnostics, std::string(label) + " ID is invalid", static_cast<std::uint32_t>(index));
            valid = false;
            continue;
        }
        if (!output.emplace(valueId.value, &values[index]).second)
        {
            Push(diagnostics, std::string(label) + " IDs must be unique", static_cast<std::uint32_t>(index));
            valid = false;
        }
    }
    return valid;
}

bool IsRead(Effect effect) noexcept
{
    return effect == Effect::Read || effect == Effect::ReadWrite;
}

bool IsWrite(Effect effect) noexcept
{
    return effect == Effect::Write || effect == Effect::ReadWrite;
}

std::uint32_t WorkKindRank(WorkKind kind) noexcept
{
    switch (kind)
    {
    case WorkKind::Copy: return 0;
    case WorkKind::Compute: return 1;
    case WorkKind::Raster: return 2;
    case WorkKind::Present: return 3;
    default: return 4;
    }
}

std::tuple<std::uint32_t, std::uint32_t> WorkKey(const Work& work) noexcept
{
    return {WorkKindRank(work.kind), work.id.value};
}

bool HasPath(const std::map<std::uint32_t, std::set<std::uint32_t>>& edges,
             std::uint32_t from, std::uint32_t to)
{
    std::set<std::uint32_t> visited;
    std::vector<std::uint32_t> pending{from};
    while (!pending.empty())
    {
        const auto current = pending.back();
        pending.pop_back();
        if (current == to) return true;
        if (!visited.insert(current).second) continue;
        const auto found = edges.find(current);
        if (found == edges.end()) continue;
        pending.insert(pending.end(), found->second.begin(), found->second.end());
    }
    return false;
}

struct Access final
{
    const Work* work = nullptr;
    const ResourceUse* use = nullptr;
};

void ValidateResource(const Resource& resource, std::uint32_t source,
                      std::vector<Diagnostic>& diagnostics)
{
    if (resource.kind == ResourceKind::Buffer)
    {
        if (resource.buffer.sizeBytes == 0)
            Push(diagnostics, "buffer shape is incomplete", source);

        switch (resource.update)
        {
        case UpdateIntent::Immutable:
            if (resource.buffer.strideBytes == 0 ||
                resource.initialContent.size() != resource.buffer.sizeBytes)
                Push(diagnostics, "immutable buffer contract is incomplete", source);
            if (resource.lifetime != LifetimeIntent::Persistent &&
                resource.lifetime != LifetimeIntent::Preparation)
                Push(diagnostics, "immutable buffer lifetime must be Persistent or Preparation", source);
            break;
        case UpdateIntent::DynamicPerFrame:
            if (resource.lifetime != LifetimeIntent::FrameLocal ||
                !resource.initialContent.empty() ||
                resource.dynamicData.requiredBytes != resource.buffer.sizeBytes ||
                !base::IsPowerOfTwo(resource.dynamicData.requiredAlignment))
                Push(diagnostics, "dynamic buffer contract is incomplete", source);
            break;
        case UpdateIntent::GpuWritten:
            if ((resource.lifetime != LifetimeIntent::FrameLocal &&
                 resource.lifetime != LifetimeIntent::Temporal &&
                 resource.lifetime != LifetimeIntent::Persistent) ||
                resource.buffer.strideBytes == 0 ||
                resource.buffer.sizeBytes % resource.buffer.strideBytes != 0)
                Push(diagnostics, "GPU-written buffer contract is incomplete", source);
            if (resource.lifetime == LifetimeIntent::Temporal &&
                resource.initialContent.size() != resource.buffer.sizeBytes)
                Push(diagnostics, "temporal buffer requires one complete initial generation", source);
            if (resource.lifetime != LifetimeIntent::Temporal && !resource.initialContent.empty())
                Push(diagnostics, "non-temporal GPU-written buffer cannot contain initial data", source);
            break;
        case UpdateIntent::External:
            if (resource.lifetime != LifetimeIntent::External ||
                resource.visibility != Visibility::Published ||
                resource.buffer.strideBytes == 0 ||
                resource.buffer.sizeBytes % resource.buffer.strideBytes != 0 ||
                !resource.initialContent.empty())
                Push(diagnostics, "external buffer contract is incomplete", source);
            break;
        default:
            Push(diagnostics, "buffer update intent is unknown", source);
            break;
        }
        return;
    }

    if (resource.kind == ResourceKind::Texture2D)
    {
        if (resource.texture2D.formatMeaning == FormatMeaning::Unknown ||
            resource.texture2D.mipLevels == 0)
            Push(diagnostics, "Texture2D shape is incomplete", source);
        if (resource.texture2D.extentMeaning == TextureExtentMeaning::Fixed &&
            (resource.texture2D.width == 0 || resource.texture2D.height == 0))
            Push(diagnostics, "fixed Texture2D extent is incomplete", source);
        if (resource.update == UpdateIntent::Immutable)
        {
            const auto required = static_cast<std::uint64_t>(resource.texture2D.rowBytes) *
                                  resource.texture2D.height;
            if (resource.lifetime != LifetimeIntent::Persistent ||
                resource.texture2D.rowBytes == 0 || required != resource.initialContent.size())
                Push(diagnostics, "immutable Texture2D contract is incomplete", source);
        }
        else if (resource.update == UpdateIntent::GpuWritten)
        {
            if (!resource.initialContent.empty() ||
                (resource.lifetime != LifetimeIntent::Persistent &&
                 resource.lifetime != LifetimeIntent::FrameLocal))
                Push(diagnostics, "GPU-written Texture2D contract is incomplete", source);
        }
        else
        {
            Push(diagnostics, "Texture2D update intent is unsupported", source);
        }
        return;
    }

    if (resource.kind == ResourceKind::SurfaceImage)
    {
        if (resource.lifetime != LifetimeIntent::External ||
            resource.update != UpdateIntent::External ||
            resource.visibility != Visibility::Published ||
            resource.surface.formatMeaning == FormatMeaning::Unknown ||
            !resource.initialContent.empty())
            Push(diagnostics, "surface contract is incomplete", source);
        return;
    }

    Push(diagnostics, "resource kind is unknown", source);
}

void ValidateProgram(const Program& program, std::uint32_t source,
                     std::vector<Diagnostic>& diagnostics)
{
    if (program.kind == ProgramKind::Raster)
    {
        if (program.interface.vertexInputs.empty() ||
            program.interface.vertexStrideBytes == 0 ||
            program.source.hlslSource.empty() ||
            program.source.vertexEntry.empty() ||
            program.source.pixelEntry.empty())
            Push(diagnostics, "raster program contract is incomplete", source);
    }
    else if (program.kind == ProgramKind::Compute)
    {
        if (!program.interface.vertexInputs.empty() ||
            program.interface.vertexStrideBytes != 0 ||
            program.source.hlslSource.empty() ||
            program.source.computeEntry.empty())
            Push(diagnostics, "compute program contract is incomplete", source);
    }
    else
    {
        Push(diagnostics, "program kind is unknown", source);
    }

    std::set<std::pair<std::uint16_t, std::uint32_t>> registerKeys;
    for (std::size_t index = 0; index < program.interface.parameters.size(); ++index)
    {
        const auto& parameter = program.interface.parameters[index];
        if (!parameter.id.IsValid() || parameter.id.value != index)
            Push(diagnostics, "ProgramParameter IDs must be dense and ordered", source);

        const bool rasterStage = parameter.stage == ShaderStage::Vertex || parameter.stage == ShaderStage::Pixel;
        if ((program.kind == ProgramKind::Raster && !rasterStage) ||
            (program.kind == ProgramKind::Compute && parameter.stage != ShaderStage::Compute))
            Push(diagnostics, "ProgramParameter stage is incompatible with Program kind", source);

        std::uint16_t registerClass = 0;
        switch (parameter.kind)
        {
        case ProgramParameterKind::ConstantBuffer:
            registerClass = 1;
            if (parameter.requiredBytes == 0 || !base::IsPowerOfTwo(parameter.requiredAlignment))
                Push(diagnostics, "constant-buffer parameter requires bytes and power-of-two alignment", source);
            break;
        case ProgramParameterKind::SampledTexture:
            registerClass = 2;
            if (parameter.stage != ShaderStage::Pixel || parameter.requiredBytes != 0)
                Push(diagnostics, "sampled-texture parameter must be a Pixel-stage texture", source);
            break;
        case ProgramParameterKind::ReadOnlyBuffer:
            registerClass = 2;
            if (parameter.stage == ShaderStage::Vertex || parameter.requiredBytes != 0)
                Push(diagnostics, "read-only-buffer parameter must be Pixel or Compute stage", source);
            break;
        case ProgramParameterKind::UnorderedBuffer:
            registerClass = 3;
            if (parameter.stage != ShaderStage::Compute || parameter.requiredBytes != 0)
                Push(diagnostics, "unordered-buffer parameter is limited to Compute stage in Level 2 v1", source);
            break;
        default:
            Push(diagnostics, "ProgramParameter kind is unknown", source);
            break;
        }
        if (registerClass != 0 && !registerKeys.insert({registerClass, parameter.shaderRegister}).second)
            Push(diagnostics, "ProgramParameter register is duplicated within its register class", source);
    }
}

void ValidateResourceUse(const ResourceUse& use, const Resource& resource,
                         std::uint32_t source, std::vector<Diagnostic>& diagnostics)
{
    const bool read = IsRead(use.effect);
    const bool write = IsWrite(use.effect);
    switch (use.role)
    {
    case ViewRole::VertexData:
        if (resource.kind != ResourceKind::Buffer || !read || write)
            Push(diagnostics, "VertexData requires a read-only Buffer", source);
        break;
    case ViewRole::ConstantData:
        if (resource.kind != ResourceKind::Buffer || resource.update != UpdateIntent::DynamicPerFrame || !read || write)
            Push(diagnostics, "ConstantData requires a read-only DynamicPerFrame Buffer", source);
        break;
    case ViewRole::SampledTexture:
        if (resource.kind != ResourceKind::Texture2D || resource.texture2D.formatMeaning != FormatMeaning::Bgra8Unorm || !read || write)
            Push(diagnostics, "SampledTexture requires a read-only Bgra8Unorm Texture2D", source);
        break;
    case ViewRole::ShaderBuffer:
        if (resource.kind != ResourceKind::Buffer || !read || write)
            Push(diagnostics, "ShaderBuffer requires a read-only Buffer", source);
        break;
    case ViewRole::StorageBuffer:
        if (resource.kind != ResourceKind::Buffer || !write || resource.update != UpdateIntent::GpuWritten)
            Push(diagnostics, "StorageBuffer requires a GPU-written Buffer with write effect", source);
        break;
    case ViewRole::ColorAttachment:
        if (resource.kind != ResourceKind::SurfaceImage || !write)
            Push(diagnostics, "Level 2 v1 ColorAttachment requires a writable SurfaceImage", source);
        break;
    case ViewRole::DepthAttachment:
        if (resource.kind != ResourceKind::Texture2D ||
            resource.texture2D.formatMeaning != FormatMeaning::Depth32Float || !write)
            Push(diagnostics, "DepthAttachment requires a writable Depth32Float Texture2D", source);
        break;
    case ViewRole::PresentSource:
        if (resource.kind != ResourceKind::SurfaceImage || !read || write)
            Push(diagnostics, "PresentSource requires a read-only SurfaceImage", source);
        break;
    case ViewRole::CopySource:
        if (resource.kind != ResourceKind::Buffer || !read || write)
            Push(diagnostics, "Level 2 v1 CopySource requires a read-only Buffer", source);
        break;
    case ViewRole::CopyDestination:
        if (resource.kind != ResourceKind::Buffer || !write)
            Push(diagnostics, "Level 2 v1 CopyDestination requires a writable Buffer", source);
        break;
    default:
        Push(diagnostics, "ResourceUse role is unknown", source);
        break;
    }

    if (use.temporalRelation == TemporalRelation::Previous)
    {
        if (resource.lifetime != LifetimeIntent::Temporal || use.role != ViewRole::ShaderBuffer || !read || write)
            Push(diagnostics, "Previous temporal relation requires a read-only ShaderBuffer use of a Temporal resource", source);
    }
    else if (use.temporalRelation != TemporalRelation::Current)
    {
        Push(diagnostics, "ResourceUse temporal relation is unknown", source);
    }
}

bool ParameterRoleMatches(ProgramParameterKind kind, ViewRole role) noexcept
{
    switch (kind)
    {
    case ProgramParameterKind::ConstantBuffer: return role == ViewRole::ConstantData;
    case ProgramParameterKind::SampledTexture: return role == ViewRole::SampledTexture;
    case ProgramParameterKind::ReadOnlyBuffer: return role == ViewRole::ShaderBuffer;
    case ProgramParameterKind::UnorderedBuffer: return role == ViewRole::StorageBuffer;
    default: return false;
    }
}

void ValidateWorkInterface(const Work& work, const Program* program,
                           const std::map<std::uint32_t, const ResourceUse*>& uses,
                           const std::map<std::uint32_t, const Resource*>& resources,
                           std::uint32_t source, std::vector<Diagnostic>& diagnostics)
{
    std::uint32_t vertex = 0;
    std::uint32_t color = 0;
    std::uint32_t depth = 0;
    std::uint32_t present = 0;
    std::uint32_t copySource = 0;
    std::uint32_t copyDestination = 0;
    std::set<std::uint32_t> uniqueUses;
    std::set<std::uint32_t> boundParameters;
    const ResourceUse* colorUse = nullptr;
    const ResourceUse* presentUse = nullptr;
    const ResourceUse* copySourceUse = nullptr;
    const ResourceUse* copyDestinationUse = nullptr;

    for (const auto& operand : work.operands)
    {
        if (!operand.use.IsValid() || !uniqueUses.insert(operand.use.value).second)
            Push(diagnostics, "Work operands must contain unique valid ResourceUse IDs", source);
        const auto found = uses.find(operand.use.value);
        if (found == uses.end())
        {
            Push(diagnostics, "Work operand references an unknown ResourceUse", source);
            continue;
        }
        const auto* use = found->second;
        switch (operand.kind)
        {
        case WorkOperandKind::ProgramParameter:
        {
            if (program == nullptr || !operand.parameter.IsValid() ||
                operand.parameter.value >= program->interface.parameters.size())
            {
                Push(diagnostics, "ProgramParameter operand references an unknown parameter", source);
                break;
            }
            if (!boundParameters.insert(operand.parameter.value).second)
                Push(diagnostics, "ProgramParameter is bound more than once", source);
            const auto& parameter = program->interface.parameters[operand.parameter.value];
            if (!ParameterRoleMatches(parameter.kind, use->role))
                Push(diagnostics, "ProgramParameter kind does not match ResourceUse role", source);
            else
            {
                const auto resourceFound = resources.find(use->resource.value);
                if (resourceFound != resources.end() && parameter.kind == ProgramParameterKind::ConstantBuffer)
                {
                    const auto* resource = resourceFound->second;
                    if (resource->dynamicData.requiredBytes != parameter.requiredBytes ||
                        resource->dynamicData.requiredAlignment < parameter.requiredAlignment)
                        Push(diagnostics, "constant-buffer parameter size/alignment does not match the bound Resource", source);
                }
            }
            break;
        }
        case WorkOperandKind::VertexData:
            ++vertex;
            if (use->role != ViewRole::VertexData) Push(diagnostics, "VertexData operand role mismatch", source);
            break;
        case WorkOperandKind::ColorAttachment:
            ++color; colorUse = use;
            if (use->role != ViewRole::ColorAttachment) Push(diagnostics, "ColorAttachment operand role mismatch", source);
            break;
        case WorkOperandKind::DepthAttachment:
            ++depth;
            if (use->role != ViewRole::DepthAttachment) Push(diagnostics, "DepthAttachment operand role mismatch", source);
            break;
        case WorkOperandKind::PresentSource:
            ++present; presentUse = use;
            if (use->role != ViewRole::PresentSource) Push(diagnostics, "PresentSource operand role mismatch", source);
            break;
        case WorkOperandKind::CopySource:
            ++copySource; copySourceUse = use;
            if (use->role != ViewRole::CopySource) Push(diagnostics, "CopySource operand role mismatch", source);
            break;
        case WorkOperandKind::CopyDestination:
            ++copyDestination; copyDestinationUse = use;
            if (use->role != ViewRole::CopyDestination) Push(diagnostics, "CopyDestination operand role mismatch", source);
            break;
        default:
            Push(diagnostics, "WorkOperand kind is unknown", source);
            break;
        }
    }

    if (program != nullptr && boundParameters.size() != program->interface.parameters.size())
        Push(diagnostics, "Work must bind every ProgramParameter exactly once", source);

    if (work.kind == WorkKind::Raster)
    {
        if (program == nullptr || program->kind != ProgramKind::Raster || work.raster.vertexCount == 0)
            Push(diagnostics, "raster work contract is incomplete", source);
        if (vertex != 1 || color != 1 || depth > 1 || present != 1 || copySource != 0 || copyDestination != 0)
            Push(diagnostics, "raster work operand structure is outside Level 2 v1", source);
        if (colorUse != nullptr && presentUse != nullptr && colorUse->resource != presentUse->resource)
            Push(diagnostics, "raster ColorAttachment and PresentSource must reference the same SurfaceImage", source);
    }
    else if (work.kind == WorkKind::Compute)
    {
        if (program == nullptr || program->kind != ProgramKind::Compute ||
            work.compute.threadGroupCountX == 0 || work.compute.threadGroupCountY == 0 ||
            work.compute.threadGroupCountZ == 0)
            Push(diagnostics, "compute work contract is incomplete", source);
        if (vertex != 0 || color != 0 || depth != 0 || present != 0 || copySource != 0 || copyDestination != 0)
            Push(diagnostics, "compute work may contain only ProgramParameter operands", source);
    }
    else if (work.kind == WorkKind::Copy)
    {
        if (program != nullptr || copySource != 1 || copyDestination != 1 ||
            work.copy.bytes == 0 || work.operands.size() != 2)
            Push(diagnostics, "copy work contract is incomplete", source);
        if (copySourceUse != nullptr && copyDestinationUse != nullptr)
        {
            const auto sourceResource = resources.find(copySourceUse->resource.value);
            const auto destinationResource = resources.find(copyDestinationUse->resource.value);
            if (sourceResource != resources.end() && destinationResource != resources.end() &&
                (work.copy.bytes > sourceResource->second->buffer.sizeBytes ||
                 work.copy.bytes > destinationResource->second->buffer.sizeBytes))
                Push(diagnostics, "copy work byte range exceeds a Buffer", source);
        }
    }
    else if (work.kind == WorkKind::Present)
    {
        if (program != nullptr || present != 1 || work.operands.size() != 1)
            Push(diagnostics, "present work contract is incomplete", source);
    }
    else
    {
        Push(diagnostics, "work kind is unknown", source);
    }
}
}

base::Result<AnalyzedGraph, std::vector<Diagnostic>> Analyze(const SemanticGraph& graph)
{
    std::vector<Diagnostic> diagnostics;
    if (graph.resources.empty()) Push(diagnostics, "semantic graph contains no resources");
    if (graph.works.empty()) Push(diagnostics, "semantic graph contains no work");

    std::map<std::uint32_t, const Resource*> resources;
    std::map<std::uint32_t, const ResourceUse*> uses;
    std::map<std::uint32_t, const Program*> programs;
    std::map<std::uint32_t, const Work*> works;
    IndexById(graph.resources, resources, [](const Resource& value) { return value.id; },
              "resource", diagnostics);
    IndexById(graph.resourceUses, uses, [](const ResourceUse& value) { return value.id; },
              "ResourceUse", diagnostics);
    IndexById(graph.programs, programs, [](const Program& value) { return value.id; },
              "program", diagnostics);
    IndexById(graph.works, works, [](const Work& value) { return value.id; },
              "work", diagnostics);

    for (std::size_t index = 0; index < graph.resources.size(); ++index)
        ValidateResource(graph.resources[index], static_cast<std::uint32_t>(index), diagnostics);

    for (std::size_t index = 0; index < graph.resourceUses.size(); ++index)
    {
        const auto& use = graph.resourceUses[index];
        const auto found = resources.find(use.resource.value);
        if (!use.resource.IsValid() || found == resources.end())
            Push(diagnostics, "ResourceUse references an unknown resource", static_cast<std::uint32_t>(index));
        else
            ValidateResourceUse(use, *found->second, static_cast<std::uint32_t>(index), diagnostics);
        if (use.effect != Effect::Read && use.effect != Effect::Write && use.effect != Effect::ReadWrite)
            Push(diagnostics, "ResourceUse effect is unknown", static_cast<std::uint32_t>(index));
    }

    for (std::size_t index = 0; index < graph.programs.size(); ++index)
        ValidateProgram(graph.programs[index], static_cast<std::uint32_t>(index), diagnostics);

    std::map<std::uint32_t, std::uint32_t> useOwners;
    for (std::size_t index = 0; index < graph.works.size(); ++index)
    {
        const auto& work = graph.works[index];
        const Program* program = nullptr;
        ProgramId programId;
        if (work.kind == WorkKind::Raster) programId = work.raster.program;
        if (work.kind == WorkKind::Compute) programId = work.compute.program;
        if (programId.IsValid())
        {
            const auto found = programs.find(programId.value);
            if (found != programs.end()) program = found->second;
            else Push(diagnostics, "work references an unknown program", static_cast<std::uint32_t>(index));
        }
        ValidateWorkInterface(work, program, uses, resources, static_cast<std::uint32_t>(index), diagnostics);
        for (const auto& operand : work.operands) ++useOwners[operand.use.value];

        for (const auto dependency : work.dependencies)
            if (!dependency.IsValid() || dependency == work.id || works.find(dependency.value) == works.end())
                Push(diagnostics, "work contains an invalid explicit dependency", static_cast<std::uint32_t>(index));
    }

    for (const auto& [id, use] : uses)
    {
        const auto ownerCount = useOwners[id];
        if (ownerCount != 1)
            Push(diagnostics, "each ResourceUse must be owned by exactly one Work operand", id);
        const auto resource = resources.find(use->resource.value);
        if (resource != resources.end() && resource->second->lifetime == LifetimeIntent::Preparation)
            Push(diagnostics, "Preparation resource cannot be referenced by frame Work", id);
    }

    std::set<std::uint32_t> usedPreparations;
    for (const auto& resource : graph.resources)
    {
        if (!resource.aliasPreparation.IsValid()) continue;
        const auto preparation = resources.find(resource.aliasPreparation.value);
        if (preparation == resources.end())
        {
            Push(diagnostics, "alias contract references an unknown Preparation resource", resource.id.value);
            continue;
        }
        if (!usedPreparations.insert(resource.aliasPreparation.value).second)
            Push(diagnostics, "one Preparation resource cannot back multiple alias targets in Level 2 v1", resource.id.value);
        if (resource.lifetime == LifetimeIntent::Preparation ||
            resource.lifetime == LifetimeIntent::External ||
            resource.kind == ResourceKind::SurfaceImage)
            Push(diagnostics, "alias target must be a package-owned Buffer or Texture2D", resource.id.value);
        const auto* candidate = preparation->second;
        if (candidate->lifetime != LifetimeIntent::Preparation || candidate->kind != resource.kind)
            Push(diagnostics, "alias preparation kind/lifetime is incompatible", resource.id.value);
        else if (resource.kind == ResourceKind::Buffer &&
                 (candidate->buffer.sizeBytes != resource.buffer.sizeBytes ||
                  candidate->buffer.strideBytes != resource.buffer.strideBytes))
            Push(diagnostics, "alias Buffer shapes are incompatible", resource.id.value);
        else if (resource.kind == ResourceKind::Texture2D &&
                 (candidate->texture2D.extentMeaning != resource.texture2D.extentMeaning ||
                  candidate->texture2D.width != resource.texture2D.width ||
                  candidate->texture2D.height != resource.texture2D.height ||
                  candidate->texture2D.formatMeaning != resource.texture2D.formatMeaning))
            Push(diagnostics, "alias Texture2D shapes are incompatible", resource.id.value);
    }
    for (const auto& resource : graph.resources)
        if (resource.lifetime == LifetimeIntent::Preparation && !usedPreparations.contains(resource.id.value))
            Push(diagnostics, "Preparation resource must be owned by exactly one alias contract", resource.id.value);

    if (!diagnostics.empty())
        return base::Result<AnalyzedGraph, std::vector<Diagnostic>>::Failure(std::move(diagnostics));

    std::map<std::uint32_t, std::set<std::uint32_t>> explicitAdjacency;
    for (const auto& [id, work] : works)
        for (const auto dependency : work->dependencies)
            explicitAdjacency[dependency.value].insert(id);

    std::vector<DependencyEdge> dependencyRecords;
    std::map<std::uint32_t, std::set<std::uint32_t>> adjacency = explicitAdjacency;
    std::set<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, DependencyKind>> uniqueRecords;
    const auto addEdge = [&](WorkId producer, WorkId consumer, ResourceId resource, DependencyKind kind)
    {
        if (producer == consumer) return;
        adjacency[producer.value].insert(consumer.value);
        const auto key = std::tuple{producer.value, consumer.value, resource.value, kind};
        if (uniqueRecords.insert(key).second)
            dependencyRecords.push_back({producer, consumer, resource, kind});
    };

    for (const auto& [id, work] : works)
        for (const auto dependency : work->dependencies)
            addEdge(dependency, work->id, {}, DependencyKind::Explicit);

    std::map<std::uint32_t, std::vector<Access>> accesses;
    for (const auto& [workId, work] : works)
    {
        for (const auto& operand : work->operands)
        {
            const auto found = uses.find(operand.use.value);
            if (found != uses.end()) accesses[found->second->resource.value].push_back({work, found->second});
        }
    }

    for (const auto& [resourceId, resourceAccesses] : accesses)
    {
        std::vector<Access> writers;
        std::vector<Access> readers;
        for (const auto& access : resourceAccesses)
        {
            if (IsWrite(access.use->effect)) writers.push_back(access);
            if (IsRead(access.use->effect) && access.use->temporalRelation != TemporalRelation::Previous)
                readers.push_back(access);
        }

        for (const auto& writer : writers)
        {
            for (const auto& reader : readers)
            {
                if (writer.work == reader.work) continue;
                if (HasPath(explicitAdjacency, reader.work->id.value, writer.work->id.value))
                    addEdge(reader.work->id, writer.work->id, {resourceId}, DependencyKind::WriteAfterRead);
                else
                    addEdge(writer.work->id, reader.work->id, {resourceId},
                            reader.use->role == ViewRole::PresentSource ? DependencyKind::Present :
                                                                        DependencyKind::ReadAfterWrite);
            }
        }

        std::sort(writers.begin(), writers.end(), [](const Access& left, const Access& right) {
            return WorkKey(*left.work) < WorkKey(*right.work);
        });
        writers.erase(std::unique(writers.begin(), writers.end(), [](const Access& left, const Access& right) {
            return left.work->id == right.work->id;
        }), writers.end());
        for (std::size_t left = 0; left < writers.size(); ++left)
        {
            for (std::size_t right = left + 1; right < writers.size(); ++right)
            {
                auto producer = writers[left].work->id;
                auto consumer = writers[right].work->id;
                if (HasPath(explicitAdjacency, consumer.value, producer.value)) std::swap(producer, consumer);
                addEdge(producer, consumer, {resourceId}, DependencyKind::WriteAfterWrite);
            }
        }
    }

    std::map<std::uint32_t, std::uint32_t> indegree;
    for (const auto& [id, work] : works) indegree[id] = 0;
    for (const auto& [producer, consumers] : adjacency)
        for (const auto consumer : consumers) ++indegree[consumer];

    std::vector<WorkId> canonicalWorkOrder;
    while (canonicalWorkOrder.size() != works.size())
    {
        const Work* selected = nullptr;
        for (const auto& [id, work] : works)
        {
            if (indegree[id] != 0) continue;
            const bool alreadyScheduled = std::any_of(canonicalWorkOrder.begin(), canonicalWorkOrder.end(),
                [id](WorkId scheduled) { return scheduled.value == id; });
            if (alreadyScheduled) continue;
            if (selected == nullptr || WorkKey(*work) < WorkKey(*selected)) selected = work;
        }
        if (selected == nullptr)
        {
            Push(diagnostics, "semantic dependency graph contains a cycle");
            break;
        }
        canonicalWorkOrder.push_back(selected->id);
        const auto outgoing = adjacency.find(selected->id.value);
        if (outgoing != adjacency.end())
            for (const auto consumer : outgoing->second) --indegree[consumer];
    }

    if (!diagnostics.empty())
        return base::Result<AnalyzedGraph, std::vector<Diagnostic>>::Failure(std::move(diagnostics));

    AnalyzedGraph output;
    output.source = &graph;
    for (const auto& [id, resource] : resources) output.canonicalResourceOrder.push_back(resource->id);
    for (const auto& [id, use] : uses) output.canonicalResourceUseOrder.push_back(use->id);
    for (const auto& [id, program] : programs) output.canonicalProgramOrder.push_back(program->id);
    output.canonicalWorkOrder = std::move(canonicalWorkOrder);
    std::sort(dependencyRecords.begin(), dependencyRecords.end(), [](const DependencyEdge& left, const DependencyEdge& right) {
        return std::tuple{left.consumer.value, left.producer.value, left.resource.value, left.kind} <
               std::tuple{right.consumer.value, right.producer.value, right.resource.value, right.kind};
    });
    output.dependencies = std::move(dependencyRecords);

    std::map<std::uint32_t, std::uint32_t> workPosition;
    for (std::uint32_t index = 0; index < output.canonicalWorkOrder.size(); ++index)
        workPosition[output.canonicalWorkOrder[index].value] = index;
    for (const auto resourceId : output.canonicalResourceOrder)
    {
        ResourceLifetime lifetime;
        lifetime.resource = resourceId;
        for (const auto& [workId, work] : works)
        {
            const auto used = std::any_of(work->operands.begin(), work->operands.end(), [&](const WorkOperand& operand) {
                const auto found = uses.find(operand.use.value);
                return found != uses.end() && found->second->resource == resourceId;
            });
            if (!used) continue;
            const auto position = workPosition[workId];
            lifetime.firstUse = lifetime.usedByWork ? std::min(lifetime.firstUse, position) : position;
            lifetime.lastUse = lifetime.usedByWork ? std::max(lifetime.lastUse, position) : position;
            lifetime.usedByWork = true;
        }
        output.resourceLifetimes.push_back(lifetime);
    }
    return base::Result<AnalyzedGraph, std::vector<Diagnostic>>::Success(std::move(output));
}
}
