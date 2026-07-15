#pragma once

#include <cstdint>

namespace sge::target
{
enum class BarrierModel : std::uint32_t { Legacy = 1, Enhanced = 2 };
enum class ShaderBinaryFormat : std::uint16_t { Dxbc = 1, Dxil = 2 };

struct D3D12TargetProfile final
{
    std::uint32_t minimumFeatureLevel = 0xb000; // stable package value for D3D_FEATURE_LEVEL_11_0
    std::uint16_t shaderModelMajor = 5;
    std::uint16_t shaderModelMinor = 1;
    std::uint16_t rootSignatureMajor = 1;
    std::uint16_t rootSignatureMinor = 0;
    BarrierModel barrierModel = BarrierModel::Legacy;
    ShaderBinaryFormat shaderBinaryFormat = ShaderBinaryFormat::Dxbc;
    std::uint32_t framesInFlight = 2;
    std::uint32_t directQueueCount = 1;
    std::uint32_t computeQueueCount = 0;
    std::uint32_t copyQueueCount = 1;
    std::uint32_t surfaceImageCount = 2;
    std::uint32_t rtvDescriptorCount = 2;
    std::uint32_t dsvDescriptorCount = 1;
    std::uint32_t shaderDescriptorCount = 12;
    std::uint32_t samplerDescriptorCount = 0;
};
}
