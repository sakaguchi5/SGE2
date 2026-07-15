#include "PackageRuntime.h"

namespace sge::runtime
{
base::Result<LoadedPackage, RuntimeError> LoadPackage(
    package::FrozenExecutablePackage package,
    IPackageExecutor& executor,
    ISurfaceHost& surface)
{
    auto sharedPackage = std::make_shared<const package::FrozenExecutablePackage>(std::move(package));
    auto instance = executor.Load(sharedPackage, surface);
    if (!instance) return base::Result<LoadedPackage, RuntimeError>::Failure(instance.Error());
    return base::Result<LoadedPackage, RuntimeError>::Success(
        LoadedPackage(std::move(sharedPackage), std::move(instance).Value()));
}

base::Result<FrameSubmission, RuntimeError> Submit(
    LoadedPackage& loaded,
    IPackageExecutor& executor,
    const FrameInvocation& invocation)
{
    return executor.Submit(loaded.Instance(), invocation);
}

base::Result<DeviceRecoveryReport, RuntimeError> RecoverDevice(
    LoadedPackage& loaded,
    IPackageExecutor& executor,
    DeviceRecoveryMode mode)
{
    return executor.RecoverDevice(loaded.Instance(), mode);
}
}
