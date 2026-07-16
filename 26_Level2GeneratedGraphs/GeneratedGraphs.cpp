#include "GeneratedGraphs.h"

#include <algorithm>
#include <array>
#include <map>
#include <numeric>
#include <set>
#include <span>
#include <tuple>
#include <utility>

namespace sge::level2::generated
{
namespace
{
namespace sem = semantic;

class SplitMix64 final
{
public:
    explicit SplitMix64(std::uint64_t state) noexcept : state_(state) {}

    [[nodiscard]] std::uint64_t Next() noexcept
    {
        std::uint64_t value = (state_ += 0x9E3779B97F4A7C15ull);
        value = (value ^ (value >> 30u)) * 0xBF58476D1CE4E5B9ull;
        value = (value ^ (value >> 27u)) * 0x94D049BB133111EBull;
        return value ^ (value >> 31u);
    }

private:
    std::uint64_t state_ = 0;
};

template<class T>
void Shuffle(std::vector<T>& values, SplitMix64& random)
{
    for (std::size_t index = values.size(); index > 1; --index)
    {
        const auto other = static_cast<std::size_t>(random.Next() % index);
        std::swap(values[index - 1], values[other]);
    }
}

std::uint32_t Rank(NodeKind kind) noexcept
{
    return kind == NodeKind::Copy ? 0u : 1u;
}

std::uint32_t QueueFor(NodeKind kind, const target::D3D12TargetProfile& profile) noexcept
{
    if (kind == NodeKind::Compute)
        return profile.computeQueueCount == 0 ? 0u : profile.directQueueCount;
    return profile.copyQueueCount == 0 ? 0u :
        profile.directQueueCount + profile.computeQueueCount;
}

bool HasPath(const std::vector<std::set<std::uint32_t>>& adjacency,
             std::uint32_t from, std::uint32_t to)
{
    std::vector<std::uint32_t> pending{from};
    std::vector<bool> visited(adjacency.size(), false);
    while (!pending.empty())
    {
        const auto current = pending.back();
        pending.pop_back();
        if (current == to) return true;
        if (visited[current]) continue;
        visited[current] = true;
        pending.insert(pending.end(), adjacency[current].begin(), adjacency[current].end());
    }
    return false;
}

base::Result<void, std::string> ValidateSpec(const GeneratedSpec& spec)
{
    if (spec.kinds.empty())
        return base::Result<void, std::string>::Failure("generated graph requires at least one node");
    if (spec.kinds.size() > 4096)
        return base::Result<void, std::string>::Failure("generated graph node count exceeds the Stage-J model limit");

    std::set<Edge> unique;
    for (const auto& edge : spec.edges)
    {
        if (edge.producerNode >= spec.kinds.size() || edge.consumerNode >= spec.kinds.size())
            return base::Result<void, std::string>::Failure("generated edge references a node outside the graph");
        if (edge.producerNode >= edge.consumerNode)
            return base::Result<void, std::string>::Failure("generated edges must follow the logical forward order");
        if (!unique.insert(edge).second)
            return base::Result<void, std::string>::Failure("generated graph contains a duplicate edge");
    }
    return base::Result<void, std::string>::Success();
}

Oracle BuildOracle(const GeneratedSpec& spec,
                   const std::vector<sem::WorkId>& workIdByNode,
                   const target::D3D12TargetProfile& profile)
{
    const auto nodeCount = static_cast<std::uint32_t>(spec.kinds.size());
    std::vector<std::set<std::uint32_t>> adjacency(nodeCount);
    std::vector<std::uint32_t> indegree(nodeCount, 0);
    for (const auto& edge : spec.edges)
        if (adjacency[edge.producerNode].insert(edge.consumerNode).second)
            ++indegree[edge.consumerNode];

    std::vector<std::uint32_t> canonicalNodes;
    canonicalNodes.reserve(nodeCount);
    std::vector<bool> scheduled(nodeCount, false);
    while (canonicalNodes.size() != nodeCount)
    {
        std::uint32_t selected = nodeCount;
        for (std::uint32_t node = 0; node < nodeCount; ++node)
        {
            if (scheduled[node] || indegree[node] != 0) continue;
            if (selected == nodeCount ||
                std::tuple{Rank(spec.kinds[node]), workIdByNode[node].value} <
                std::tuple{Rank(spec.kinds[selected]), workIdByNode[selected].value})
                selected = node;
        }
        if (selected == nodeCount) break;
        scheduled[selected] = true;
        canonicalNodes.push_back(selected);
        for (const auto consumer : adjacency[selected]) --indegree[consumer];
    }

    auto reduced = adjacency;
    for (const auto& edge : spec.edges)
    {
        reduced[edge.producerNode].erase(edge.consumerNode);
        if (!HasPath(reduced, edge.producerNode, edge.consumerNode))
            reduced[edge.producerNode].insert(edge.consumerNode);
    }

    Oracle oracle;
    for (const auto kind : spec.kinds)
    {
        if (kind == NodeKind::Compute) ++oracle.computeWorkCount;
        else ++oracle.copyWorkCount;
    }
    for (const auto node : canonicalNodes)
    {
        oracle.canonicalWorkOrder.push_back(workIdByNode[node]);
        oracle.queueByPosition.push_back(QueueFor(spec.kinds[node], profile));
    }

    std::map<std::uint32_t, std::uint32_t> positionByNode;
    for (std::uint32_t position = 0; position < canonicalNodes.size(); ++position)
        positionByNode[canonicalNodes[position]] = position;

    for (std::uint32_t producer = 0; producer < nodeCount; ++producer)
        for (const auto consumer : reduced[producer])
            oracle.reducedEdges.push_back({workIdByNode[producer], workIdByNode[consumer]});
    std::sort(oracle.reducedEdges.begin(), oracle.reducedEdges.end());

    for (std::uint32_t consumerPosition = 0; consumerPosition < canonicalNodes.size(); ++consumerPosition)
    {
        const auto consumerNode = canonicalNodes[consumerPosition];
        const auto consumerQueue = QueueFor(spec.kinds[consumerNode], profile);
        std::map<std::uint32_t, std::uint32_t> latestProducerByQueue;
        for (std::uint32_t producerNode = 0; producerNode < nodeCount; ++producerNode)
        {
            if (!reduced[producerNode].contains(consumerNode)) continue;
            const auto producerQueue = QueueFor(spec.kinds[producerNode], profile);
            if (producerQueue == consumerQueue) continue;
            const auto producerPosition = positionByNode.at(producerNode);
            const auto found = latestProducerByQueue.find(producerQueue);
            if (found == latestProducerByQueue.end() || producerPosition > found->second)
                latestProducerByQueue[producerQueue] = producerPosition;
        }
        for (const auto& [producerQueue, producerPosition] : latestProducerByQueue)
            oracle.waits.push_back({consumerPosition, consumerQueue, producerQueue, producerPosition});
    }
    return oracle;
}

std::vector<std::byte> InitialCopyBytes()
{
    constexpr std::array<std::uint32_t, 4> words = {
        0x3F800000u, 0x40000000u, 0x40400000u, 0x40800000u};
    const auto bytes = std::as_bytes(std::span<const std::uint32_t>(words));
    return {bytes.begin(), bytes.end()};
}

sem::Program MakeComputeProgram()
{
    sem::Program program;
    program.id = {0};
    program.debugName = "StageJ.Generated.ComputeProgram";
    program.kind = sem::ProgramKind::Compute;
    program.interface.parameters.push_back({
        sem::ProgramParameterId{0},
        "GeneratedOutput",
        sem::ProgramParameterKind::UnorderedBuffer,
        sem::ShaderStage::Compute,
        0,
        0,
        1});
    program.source.hlslSource = R"hlsl(
RWStructuredBuffer<float4> GeneratedOutput : register(u0);
[numthreads(1, 1, 1)]
void CSGenerated(uint3 id : SV_DispatchThreadID)
{
    GeneratedOutput[0] = float4(0.125f, 0.25f, 0.5f, 1.0f);
}
)hlsl";
    program.source.computeEntry = "CSGenerated";
    return program;
}
}

semantic::SemanticGraph PermuteStorage(semantic::SemanticGraph graph,
                                       std::uint64_t seed)
{
    SplitMix64 random(seed ^ 0xD1B54A32D192ED03ull);
    Shuffle(graph.resources, random);
    Shuffle(graph.resourceUses, random);
    Shuffle(graph.programs, random);
    Shuffle(graph.works, random);
    for (auto& work : graph.works)
    {
        Shuffle(work.operands, random);
        Shuffle(work.dependencies, random);
    }
    return graph;
}

base::Result<GeneratedCase, std::string> BuildGeneratedCase(GeneratedSpec spec)
{
    auto valid = ValidateSpec(spec);
    if (!valid)
        return base::Result<GeneratedCase, std::string>::Failure(valid.Error());

    const auto nodeCount = static_cast<std::uint32_t>(spec.kinds.size());
    const bool hasCompute = std::find(spec.kinds.begin(), spec.kinds.end(), NodeKind::Compute) != spec.kinds.end();
    const bool hasCopy = std::find(spec.kinds.begin(), spec.kinds.end(), NodeKind::Copy) != spec.kinds.end();

    SplitMix64 random(spec.seed ^ 0xA0761D6478BD642Full);
    std::vector<std::uint32_t> ids(nodeCount);
    std::iota(ids.begin(), ids.end(), 0u);
    Shuffle(ids, random);

    GeneratedCase output;
    output.spec = spec;
    output.workIdByNode.reserve(nodeCount);
    for (const auto id : ids) output.workIdByNode.push_back({id});

    auto& graph = output.graph;
    std::uint32_t nextResource = 0;
    std::uint32_t copySourceResource = base::InvalidIndex;
    if (hasCopy)
    {
        copySourceResource = nextResource++;
        sem::Resource source;
        source.id = {copySourceResource};
        source.debugName = "StageJ.Generated.CopySource";
        source.kind = sem::ResourceKind::Buffer;
        source.lifetime = sem::LifetimeIntent::Persistent;
        source.update = sem::UpdateIntent::Immutable;
        source.visibility = sem::Visibility::Internal;
        source.buffer = {16, 16};
        source.initialContent = InitialCopyBytes();
        graph.resources.push_back(std::move(source));
    }

    std::vector<sem::ResourceId> outputResourceByNode;
    outputResourceByNode.reserve(nodeCount);
    for (std::uint32_t node = 0; node < nodeCount; ++node)
    {
        sem::Resource resource;
        resource.id = {nextResource++};
        resource.debugName = "StageJ.Generated.Output." + std::to_string(node);
        resource.kind = sem::ResourceKind::Buffer;
        resource.lifetime = sem::LifetimeIntent::Persistent;
        resource.update = sem::UpdateIntent::GpuWritten;
        resource.visibility = sem::Visibility::Internal;
        resource.buffer = {16, 16};
        outputResourceByNode.push_back(resource.id);
        graph.resources.push_back(std::move(resource));
    }

    if (hasCompute) graph.programs.push_back(MakeComputeProgram());

    std::uint32_t nextUse = 0;
    std::vector<sem::Work> worksByNode(nodeCount);
    for (std::uint32_t node = 0; node < nodeCount; ++node)
    {
        sem::Work work;
        work.id = output.workIdByNode[node];
        work.debugName = "StageJ.Generated.Work." + std::to_string(node);
        if (spec.kinds[node] == NodeKind::Compute)
        {
            sem::ResourceUse outputUse;
            outputUse.id = {nextUse++};
            outputUse.resource = outputResourceByNode[node];
            outputUse.effect = sem::Effect::Write;
            outputUse.role = sem::ViewRole::StorageBuffer;
            outputUse.temporalRelation = sem::TemporalRelation::Current;
            graph.resourceUses.push_back(outputUse);

            work.kind = sem::WorkKind::Compute;
            work.operands.push_back({sem::WorkOperandKind::ProgramParameter, outputUse.id,
                                     sem::ProgramParameterId{0}});
            work.compute.program = {0};
            work.compute.threadGroupCountX = 1;
            work.compute.threadGroupCountY = 1;
            work.compute.threadGroupCountZ = 1;
        }
        else
        {
            sem::ResourceUse sourceUse;
            sourceUse.id = {nextUse++};
            sourceUse.resource = {copySourceResource};
            sourceUse.effect = sem::Effect::Read;
            sourceUse.role = sem::ViewRole::CopySource;
            sourceUse.temporalRelation = sem::TemporalRelation::Current;
            graph.resourceUses.push_back(sourceUse);

            sem::ResourceUse destinationUse;
            destinationUse.id = {nextUse++};
            destinationUse.resource = outputResourceByNode[node];
            destinationUse.effect = sem::Effect::Write;
            destinationUse.role = sem::ViewRole::CopyDestination;
            destinationUse.temporalRelation = sem::TemporalRelation::Current;
            graph.resourceUses.push_back(destinationUse);

            work.kind = sem::WorkKind::Copy;
            work.operands.push_back({sem::WorkOperandKind::CopySource, sourceUse.id, {}});
            work.operands.push_back({sem::WorkOperandKind::CopyDestination, destinationUse.id, {}});
            work.copy.bytes = 16;
        }
        worksByNode[node] = std::move(work);
    }

    for (const auto& edge : spec.edges)
        worksByNode[edge.consumerNode].dependencies.push_back(output.workIdByNode[edge.producerNode]);
    graph.works = std::move(worksByNode);

    output.targetProfile = {};
    output.targetProfile.computeQueueCount = hasCompute && spec.dedicatedComputeQueue ? 1u : 0u;
    output.targetProfile.copyQueueCount = hasCopy && spec.dedicatedCopyQueue ? 1u : 0u;
    output.targetProfile.surfaceImageCount = 0;
    output.targetProfile.rtvDescriptorCount = 0;
    output.targetProfile.dsvDescriptorCount = 0;
    output.targetProfile.shaderDescriptorCount = static_cast<std::uint32_t>(
        std::count(spec.kinds.begin(), spec.kinds.end(), NodeKind::Compute));
    output.targetProfile.samplerDescriptorCount = 0;

    output.oracle = BuildOracle(spec, output.workIdByNode, output.targetProfile);
    if (spec.permuteStorage)
        output.graph = PermuteStorage(std::move(output.graph), spec.seed);
    return base::Result<GeneratedCase, std::string>::Success(std::move(output));
}

GeneratedSpec MakeForwardMaskSpec(std::string name,
                                  std::uint32_t nodeCount,
                                  std::uint64_t edgeMask,
                                  std::uint64_t seed,
                                  bool mixedKinds)
{
    GeneratedSpec spec;
    spec.name = std::move(name);
    spec.seed = seed;
    spec.kinds.resize(nodeCount, NodeKind::Compute);
    if (mixedKinds)
        for (std::uint32_t node = 0; node < nodeCount; ++node)
            spec.kinds[node] = ((seed >> (node % 61u)) & 1ull) != 0 ? NodeKind::Copy : NodeKind::Compute;
    if (mixedKinds && nodeCount > 1)
    {
        spec.kinds[0] = NodeKind::Compute;
        spec.kinds[1] = NodeKind::Copy;
    }

    std::uint32_t bit = 0;
    for (std::uint32_t producer = 0; producer < nodeCount; ++producer)
        for (std::uint32_t consumer = producer + 1; consumer < nodeCount; ++consumer, ++bit)
            if (((edgeMask >> bit) & 1ull) != 0)
                spec.edges.push_back({producer, consumer});
    return spec;
}

GeneratedSpec MakeSeededSpec(std::string name,
                             std::uint32_t nodeCount,
                             std::uint64_t seed,
                             std::uint32_t edgePercent,
                             bool mixedKinds)
{
    GeneratedSpec spec;
    spec.name = std::move(name);
    spec.seed = seed;
    spec.kinds.resize(nodeCount, NodeKind::Compute);
    SplitMix64 random(seed);
    if (mixedKinds)
        for (auto& kind : spec.kinds)
            kind = (random.Next() & 1ull) != 0 ? NodeKind::Compute : NodeKind::Copy;
    if (mixedKinds && nodeCount > 1)
    {
        spec.kinds[0] = NodeKind::Compute;
        spec.kinds[1] = NodeKind::Copy;
    }

    const auto threshold = std::min(edgePercent, 100u);
    for (std::uint32_t producer = 0; producer < nodeCount; ++producer)
        for (std::uint32_t consumer = producer + 1; consumer < nodeCount; ++consumer)
            if (random.Next() % 100u < threshold)
                spec.edges.push_back({producer, consumer});
    return spec;
}
}
