#pragma once

#include "../00_Base/StrongId.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sge::semantic
{
struct ResourceTag;
struct ResourceUseTag;
struct ProgramTag;
struct ProgramParameterTag;
struct WorkTag;
using ResourceId = base::Id32<ResourceTag>;
using ResourceUseId = base::Id32<ResourceUseTag>;
using ProgramId = base::Id32<ProgramTag>;
using ProgramParameterId = base::Id32<ProgramParameterTag>;
using WorkId = base::Id32<WorkTag>;

enum class ResourceKind : std::uint16_t { Buffer = 1, Texture2D = 2, SurfaceImage = 3 };
enum class FormatMeaning : std::uint16_t { Unknown = 0, Bgra8Unorm = 1, Depth32Float = 2 };
enum class LifetimeIntent : std::uint16_t { Persistent = 1, FrameLocal = 2, Temporal = 3, External = 4, Preparation = 5 };
enum class UpdateIntent : std::uint16_t { Immutable = 1, DynamicPerFrame = 2, External = 3, GpuWritten = 4 };
enum class Visibility : std::uint16_t { Internal = 1, Published = 2 };
enum class Effect : std::uint16_t { Read = 1, Write = 2, ReadWrite = 3 };

// A ResourceUse describes the view required by one Work. It deliberately does
// not encode where the Resource came from (compute/copy/external/alias). Those
// facts belong to Resource lifetime/update/alias contracts and graph edges.
enum class ViewRole : std::uint16_t
{
    VertexData = 1,
    ConstantData = 2,
    SampledTexture = 3,
    ColorAttachment = 4,
    PresentSource = 5,
    DepthAttachment = 6,
    StorageBuffer = 7,
    ShaderBuffer = 8,
    CopySource = 9,
    CopyDestination = 10
};

enum class TemporalRelation : std::uint16_t { Current = 1, Previous = 2 };
enum class WorkKind : std::uint16_t { Raster = 1, Compute = 2, Copy = 3, Present = 4 };
enum class ProgramKind : std::uint16_t { Raster = 1, Compute = 2 };
enum class ProgramParameterKind : std::uint16_t
{
    ConstantBuffer = 1,
    SampledTexture = 2,
    ReadOnlyBuffer = 3,
    UnorderedBuffer = 4
};
enum class ShaderStage : std::uint16_t { Vertex = 1, Pixel = 2, Compute = 3 };
enum class WorkOperandKind : std::uint16_t
{
    ProgramParameter = 1,
    VertexData = 2,
    ColorAttachment = 3,
    DepthAttachment = 4,
    PresentSource = 5,
    CopySource = 6,
    CopyDestination = 7
};

struct BufferShape final
{
    std::uint64_t sizeBytes = 0;
    std::uint32_t strideBytes = 0;
};

enum class TextureExtentMeaning : std::uint16_t { Fixed = 1, PresentationSurface = 2 };

struct Texture2DShape final
{
    TextureExtentMeaning extentMeaning = TextureExtentMeaning::Fixed;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    FormatMeaning formatMeaning = FormatMeaning::Unknown;
    std::uint32_t rowBytes = 0;
    std::uint16_t mipLevels = 1;
};

struct DynamicDataContract final
{
    std::uint64_t requiredBytes = 0;
    std::uint32_t requiredAlignment = 1;
};

struct SurfaceShape final
{
    FormatMeaning formatMeaning = FormatMeaning::Unknown;
};

struct Resource final
{
    ResourceId id;
    std::string debugName;
    ResourceKind kind = ResourceKind::Buffer;
    LifetimeIntent lifetime = LifetimeIntent::Persistent;
    UpdateIntent update = UpdateIntent::Immutable;
    Visibility visibility = Visibility::Internal;
    BufferShape buffer;
    Texture2DShape texture2D;
    DynamicDataContract dynamicData;
    SurfaceShape surface;
    std::vector<std::byte> initialContent;

    // Optional explicit alias contract. The target Resource may reuse the
    // compatible Preparation Resource allocation after lifetime validation.
    ResourceId aliasPreparation;
};

struct ResourceUse final
{
    ResourceUseId id;
    ResourceId resource;
    Effect effect = Effect::Read;
    ViewRole role = ViewRole::VertexData;
    TemporalRelation temporalRelation = TemporalRelation::Current;
};

struct VertexInput final
{
    enum class Meaning : std::uint16_t { Position = 1, Color = 2, TexCoord = 3 };
    Meaning meaning = Meaning::Position;
    std::uint16_t componentCount = 3;
    std::uint32_t byteOffset = 0;
};

struct ProgramParameter final
{
    ProgramParameterId id;
    std::string debugName;
    ProgramParameterKind kind = ProgramParameterKind::ConstantBuffer;
    ShaderStage stage = ShaderStage::Vertex;
    std::uint32_t shaderRegister = 0;
    std::uint64_t requiredBytes = 0;
    std::uint32_t requiredAlignment = 1;
};

struct ProgramInterface final
{
    std::vector<VertexInput> vertexInputs;
    std::uint32_t vertexStrideBytes = 0;
    std::vector<ProgramParameter> parameters;
};

struct ProgramSource final
{
    std::string hlslSource;
    std::string vertexEntry;
    std::string pixelEntry;
    std::string computeEntry;
};

struct Program final
{
    ProgramId id;
    std::string debugName;
    ProgramKind kind = ProgramKind::Raster;
    ProgramInterface interface;
    ProgramSource source;
};

// Every ResourceUse owned by a Work appears exactly once as a WorkOperand.
// Program bindings refer to a stable parameter identity instead of relying on
// ResourceUse vector order or role-count conventions.
struct WorkOperand final
{
    WorkOperandKind kind = WorkOperandKind::ProgramParameter;
    ResourceUseId use;
    ProgramParameterId parameter;
};

struct RasterPayload final
{
    ProgramId program;
    std::uint32_t vertexCount = 0;
};

struct CopyPayload final
{
    std::uint64_t bytes = 0;
};

struct ComputePayload final
{
    ProgramId program;
    std::uint32_t threadGroupCountX = 1;
    std::uint32_t threadGroupCountY = 1;
    std::uint32_t threadGroupCountZ = 1;
};

struct Work final
{
    WorkId id;
    std::string debugName;
    WorkKind kind = WorkKind::Raster;
    std::vector<WorkOperand> operands;
    // Optional semantic ordering constraints. The compiler also derives hazard
    // edges from ResourceUse; these edges are only for relations that cannot be
    // inferred from resource effects (for example, an intentional WAR order).
    std::vector<WorkId> dependencies;
    RasterPayload raster;
    CopyPayload copy;
    ComputePayload compute;
};

struct SemanticGraph final
{
    std::vector<Resource> resources;
    std::vector<ResourceUse> resourceUses;
    std::vector<Program> programs;
    std::vector<Work> works;
};
}
