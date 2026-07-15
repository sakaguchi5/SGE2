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

bool IsSampledBuffer(ViewRole role) noexcept
{
    switch (role)
    {
    case ViewRole::ComputedBuffer:
    case ViewRole::CopiedBuffer:
    case ViewRole::TemporalPreviousBuffer:
    case ViewRole::AliasedBuffer:
    case ViewRole::ExternalBuffer:
        return true;
    default:
        return false;
    }
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

    if (program.interface.constantDataBytes != 0 &&
        !base::IsPowerOfTwo(program.interface.constantDataAlignment))
        Push(diagnostics, "program constant-data alignment must be a power of two", source);
}

void ValidateWorkInterface(const Work& work, const Program* program,
                           const std::map<std::uint32_t, const ResourceUse*>& uses,
                           std::uint32_t source, std::vector<Diagnostic>& diagnostics)
{
    std::uint32_t vertex = 0;
    std::uint32_t constants = 0;
    std::uint32_t sampledTextures = 0;
    std::uint32_t sampledBuffers = 0;
    std::uint32_t unorderedBuffers = 0;
    std::uint32_t color = 0;
    std::uint32_t depth = 0;
    std::uint32_t present = 0;
    std::uint32_t copySource = 0;
    std::uint32_t copyDestination = 0;

    std::set<std::uint32_t> uniqueUses;
    for (const auto useId : work.uses)
    {
        if (!uniqueUses.insert(useId.value).second)
            Push(diagnostics, "work contains a duplicate ResourceUse", source);
        const auto found = uses.find(useId.value);
        if (!useId.IsValid() || found == uses.end())
        {
            Push(diagnostics, "work references an unknown ResourceUse", source);
            continue;
        }
        switch (found->second->role)
        {
        case ViewRole::VertexData: ++vertex; break;
        case ViewRole::ConstantData: ++constants; break;
        case ViewRole::SampledTexture: ++sampledTextures; break;
        case ViewRole::StorageBuffer: ++unorderedBuffers; break;
        case ViewRole::ColorAttachment: ++color; break;
        case ViewRole::DepthAttachment: ++depth; break;
        case ViewRole::PresentSource: ++present; break;
        case ViewRole::CopySource: ++copySource; break;
        case ViewRole::CopyDestination: ++copyDestination; break;
        default:
            if (IsSampledBuffer(found->second->role)) ++sampledBuffers;
            break;
        }
    }

    if (work.kind == WorkKind::Raster)
    {
        if (program == nullptr || program->kind != ProgramKind::Raster ||
            work.raster.vertexCount == 0)
        {
            Push(diagnostics, "raster work contract is incomplete", source);
            return;
        }
        const auto expectedConstants = program->interface.constantDataBytes == 0 ? 0u : 1u;
        if (vertex != 1 || color != 1 || depth > 1 || present > 1 ||
            constants != expectedConstants ||
            sampledTextures != program->interface.sampledTextureCount ||
            sampledBuffers != program->interface.sampledBufferCount ||
            unorderedBuffers != program->interface.unorderedBufferCount)
            Push(diagnostics, "raster work uses do not match ProgramInterface", source);
    }
    else if (work.kind == WorkKind::Compute)
    {
        if (program == nullptr || program->kind != ProgramKind::Compute ||
            work.compute.threadGroupCountX == 0 || work.compute.threadGroupCountY == 0 ||
            work.compute.threadGroupCountZ == 0)
        {
            Push(diagnostics, "compute work contract is incomplete", source);
            return;
        }
        const auto expectedConstants = program->interface.constantDataBytes == 0 ? 0u : 1u;
        if (vertex != 0 || color != 0 || depth != 0 || present != 0 ||
            constants != expectedConstants ||
            sampledTextures != program->interface.sampledTextureCount ||
            sampledBuffers != program->interface.sampledBufferCount ||
            unorderedBuffers != program->interface.unorderedBufferCount)
            Push(diagnostics, "compute work uses do not match ProgramInterface", source);
    }
    else if (work.kind == WorkKind::Copy)
    {
        if (copySource != 1 || copyDestination != 1 || work.copy.bytes == 0 ||
            work.uses.size() != 2)
            Push(diagnostics, "copy work contract is incomplete", source);
    }
    else if (work.kind == WorkKind::Present)
    {
        if (present != 1 || work.uses.size() != 1)
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
        if (!use.resource.IsValid() || resources.find(use.resource.value) == resources.end())
            Push(diagnostics, "ResourceUse references an unknown resource", static_cast<std::uint32_t>(index));
        if (use.effect != Effect::Read && use.effect != Effect::Write && use.effect != Effect::ReadWrite)
            Push(diagnostics, "ResourceUse effect is unknown", static_cast<std::uint32_t>(index));
    }

    for (std::size_t index = 0; index < graph.programs.size(); ++index)
        ValidateProgram(graph.programs[index], static_cast<std::uint32_t>(index), diagnostics);

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
        ValidateWorkInterface(work, program, uses, static_cast<std::uint32_t>(index), diagnostics);

        for (const auto dependency : work.dependencies)
            if (!dependency.IsValid() || dependency == work.id || works.find(dependency.value) == works.end())
                Push(diagnostics, "work contains an invalid explicit dependency", static_cast<std::uint32_t>(index));
    }

    for (const auto& [id, use] : uses)
    {
        const auto resource = resources.find(use->resource.value);
        if (resource != resources.end() && resource->second->lifetime == LifetimeIntent::Preparation)
        {
            const bool referenced = std::any_of(graph.works.begin(), graph.works.end(), [id](const Work& work) {
                return std::any_of(work.uses.begin(), work.uses.end(), [id](ResourceUseId useId) { return useId.value == id; });
            });
            if (referenced) Push(diagnostics, "Preparation resource cannot be referenced by frame Work", id);
        }

        if (resource != resources.end() && use->role == ViewRole::AliasedBuffer)
        {
            const auto* aliased = resource->second;
            const bool compatiblePreparation = std::any_of(graph.resources.begin(), graph.resources.end(),
                [aliased](const Resource& candidate) {
                    if (candidate.lifetime != LifetimeIntent::Preparation || candidate.kind != aliased->kind)
                        return false;
                    if (candidate.kind == ResourceKind::Buffer)
                        return candidate.buffer.sizeBytes == aliased->buffer.sizeBytes &&
                               candidate.buffer.strideBytes == aliased->buffer.strideBytes;
                    return candidate.texture2D.extentMeaning == aliased->texture2D.extentMeaning &&
                           candidate.texture2D.width == aliased->texture2D.width &&
                           candidate.texture2D.height == aliased->texture2D.height &&
                           candidate.texture2D.formatMeaning == aliased->texture2D.formatMeaning;
                });
            if (!compatiblePreparation)
                Push(diagnostics, "AliasedBuffer requires a compatible Preparation resource", id);
        }
    }

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
        for (const auto useId : work->uses)
        {
            const auto found = uses.find(useId.value);
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
            if (IsRead(access.use->effect) && access.use->role != ViewRole::TemporalPreviousBuffer)
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
            const auto used = std::any_of(work->uses.begin(), work->uses.end(), [&](ResourceUseId useId) {
                const auto found = uses.find(useId.value);
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
