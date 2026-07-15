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
struct WorkTag;
using ResourceId = base::Id32<ResourceTag>;
using ResourceUseId = base::Id32<ResourceUseTag>;
using ProgramId = base::Id32<ProgramTag>;
using WorkId = base::Id32<WorkTag>;

enum class ResourceKind : std::uint16_t { Buffer = 1, Texture2D = 2, SurfaceImage = 3 };
enum class FormatMeaning : std::uint16_t { Unknown = 0, Bgra8Unorm = 1, Depth32Float = 2 };
enum class LifetimeIntent : std::uint16_t { Persistent = 1, FrameLocal = 2, Temporal = 3, External = 4, Preparation = 5 };
enum class UpdateIntent : std::uint16_t { Immutable = 1, DynamicPerFrame = 2, External = 3, GpuWritten = 4 };
enum class Visibility : std::uint16_t { Internal = 1, Published = 2 };
enum class Effect : std::uint16_t { Read = 1, Write = 2, ReadWrite = 3 };
enum class ViewRole : std::uint16_t
{
    VertexData = 1,
    ConstantData = 2,
    SampledTexture = 3,
    ColorAttachment = 4,
    PresentSource = 5,
    DepthAttachment = 6,
    StorageBuffer = 7,
    ComputedBuffer = 8,
    CopySource = 9,
    CopyDestination = 10,
    CopiedBuffer = 11,
    TemporalPreviousBuffer = 12,
    AliasedBuffer = 13,
    ExternalBuffer = 14
};
enum class WorkKind : std::uint16_t { Raster = 1, Compute = 2, Copy = 3, Present = 4 };
enum class ProgramKind : std::uint16_t { Raster = 1, Compute = 2 };

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
};

struct ResourceUse final
{
    ResourceUseId id;
    ResourceId resource;
    Effect effect = Effect::Read;
    ViewRole role = ViewRole::VertexData;
};

struct VertexInput final
{
    enum class Meaning : std::uint16_t { Position = 1, Color = 2, TexCoord = 3 };
    Meaning meaning = Meaning::Position;
    std::uint16_t componentCount = 3;
    std::uint32_t byteOffset = 0;
};

struct ProgramInterface final
{
    std::vector<VertexInput> vertexInputs;
    std::uint32_t vertexStrideBytes = 0;
    std::uint64_t constantDataBytes = 0;
    std::uint32_t constantDataAlignment = 1;
    std::uint32_t sampledTextureCount = 0;
    std::uint32_t sampledBufferCount = 0;
    std::uint32_t unorderedBufferCount = 0;
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

struct RasterPayload final
{
    ProgramId program;
    ResourceUseId vertexData;
    ResourceUseId constantData;
    ResourceUseId sampledTexture;
    ResourceUseId computedData;
    ResourceUseId copiedData;
    ResourceUseId aliasedData;
    ResourceUseId externalData;
    ResourceUseId colorAttachment;
    ResourceUseId depthAttachment;
    ResourceUseId presentSource;
    std::uint32_t vertexCount = 0;
};

struct CopyPayload final
{
    ResourceUseId source;
    ResourceUseId destination;
    std::uint64_t bytes = 0;
};

struct ComputePayload final
{
    ProgramId program;
    ResourceUseId constantData;
    ResourceUseId previous;
    ResourceUseId output;
    std::uint32_t threadGroupCountX = 1;
    std::uint32_t threadGroupCountY = 1;
    std::uint32_t threadGroupCountZ = 1;
};

struct Work final
{
    WorkId id;
    std::string debugName;
    WorkKind kind = WorkKind::Raster;
    std::vector<ResourceUseId> uses;
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
