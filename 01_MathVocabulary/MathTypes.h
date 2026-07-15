#pragma once

#include <array>
#include <cstdint>
#include <cmath>

namespace sge::math
{
struct Float2 { float x = 0.0f; float y = 0.0f; };
struct Float3 { float x = 0.0f; float y = 0.0f; float z = 0.0f; };
struct Float4 { float x = 0.0f; float y = 0.0f; float z = 0.0f; float w = 0.0f; };
struct Matrix4x4 { std::array<float, 16> values{}; };
struct Transform { Matrix4x4 matrix{}; };
struct CameraSpecification { float verticalFieldOfViewRadians = 1.04719755f; float nearPlane = 0.1f; float farPlane = 1000.0f; };
struct ExperimentTarget { std::uint32_t width = 1280; std::uint32_t height = 720; };
}
