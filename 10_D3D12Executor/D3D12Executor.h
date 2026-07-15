#pragma once

#include "../08_D3D12PackageSchema/D3D12Encoding.h"
#include "../09_PackageRuntime/PackageRuntime.h"

#include <array>

namespace sge::d3d12
{
struct ExecutorOptions final
{
    bool forceWarp = false;
    bool enableDebugLayer = true;
};


struct ExternalBufferBinding final
{
    std::shared_ptr<runtime::IExternalResource> resource;
    std::shared_ptr<runtime::ICompletionToken> availableAfter;
};

class D3D12Executor final : public runtime::IPackageExecutor
{
public:
    explicit D3D12Executor(ExecutorOptions options = {}) : options_(options) {}

    [[nodiscard]] base::Result<std::unique_ptr<runtime::IPackageInstance>, runtime::RuntimeError> Load(
        std::shared_ptr<const package::FrozenExecutablePackage> package,
        runtime::ISurfaceHost& surface) override;

    [[nodiscard]] base::Result<runtime::FrameSubmission, runtime::RuntimeError> Submit(
        runtime::IPackageInstance& instance,
        const runtime::FrameInvocation& invocation) override;

    [[nodiscard]] base::Result<runtime::DeviceRecoveryReport, runtime::RuntimeError> RecoverDevice(
        runtime::IPackageInstance& instance,
        runtime::DeviceRecoveryMode mode) override;

    [[nodiscard]] base::Result<ExternalBufferBinding, runtime::RuntimeError> CreateExternalColorBuffer(
        runtime::IPackageInstance& instance,
        const std::array<float, 4>& color);

    [[nodiscard]] static bool SupportsOperation(package::d3d12_v13::D3D12OperationCode code) noexcept;

private:
    ExecutorOptions options_;
};
}
