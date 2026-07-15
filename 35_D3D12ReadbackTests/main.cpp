#include "../00_Base/FileIO.h"
#include "../07_FrozenPackageCore/PackageReader.h"
#include "../09_PackageRuntime/PackageRuntime.h"
#include "../10_D3D12Executor/D3D12Executor.h"
#include "../11_PlatformWin32/Win32Window.h"

#include <Windows.h>

#include <array>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
constexpr std::array<float, 16> Identity = {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f
};
constexpr std::array<float, 4> ExternalColor{0.96f, 0.88f, 0.72f, 1.0f};

int RunFreshProcessChild(const std::filesystem::path& packagePath)
{
    auto bytes = sge::base::ReadAllBytes(packagePath);
    if (!bytes) return 40;
    auto package = sge::package::PackageReader::Read(std::move(bytes).Value());
    if (!package) return 41;
    auto window = sge::platform::Win32Window::Create(L"SGE2 Slice 15 Fresh Process", 64, 64);
    if (!window) return 42;

    sge::d3d12::D3D12Executor executor({true, true});
    auto loaded = sge::runtime::LoadPackage(std::move(package).Value(), executor, *window.Value());
    if (!loaded) return 43;
    auto external = executor.CreateExternalColorBuffer(loaded.Value().Instance(), ExternalColor);
    if (!external) return 44;

    const auto constantBytes = std::as_bytes(std::span<const float>(Identity));
    const sge::runtime::DynamicDataBinding dynamicBinding{0, constantBytes};
    const sge::runtime::ExternalResourceBinding externalBinding{0, external.Value().resource, external.Value().availableAfter};
    sge::runtime::FrameInvocation invocation;
    invocation.frameNumber = 0;
    invocation.dynamicData = std::span<const sge::runtime::DynamicDataBinding>(&dynamicBinding, 1);
    invocation.externalResources = std::span<const sge::runtime::ExternalResourceBinding>(&externalBinding, 1);
    auto submitted = sge::runtime::Submit(loaded.Value(), executor, invocation);
    if (!submitted || submitted.Value().deviceEpoch != 1 || submitted.Value().frameSlot != 0 ||
        submitted.Value().temporalPreviousInstance != 1 || submitted.Value().temporalCurrentInstance != 0)
        return 45;

    std::cout << "Fresh-process Frozen Package rematerialization passed.\n";
    return 0;
}

int RunFreshProcess(const std::filesystem::path& packagePath)
{
    std::array<wchar_t, 32768> executable{};
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) return 30;

    std::wstring command = L"\"";
    command.append(executable.data(), length);
    command += L"\" \"";
    command += packagePath.wstring();
    command += L"\" --fresh-process-child";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
        return 31;
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 0;
    const BOOL queried = GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (!queried) return 32;
    return static_cast<int>(exitCode);
}
}

int wmain(int argc, wchar_t** argv)
{
    const std::filesystem::path packagePath = argc >= 2
        ? std::filesystem::path(argv[1])
        : std::filesystem::path(L"CommonExperiment.sgep");
    if (argc >= 3 && std::wstring_view(argv[2]) == L"--fresh-process-child")
        return RunFreshProcessChild(packagePath);

    auto bytes = sge::base::ReadAllBytes(packagePath);
    if (!bytes) { std::cerr << "Read failed: " << bytes.Error() << '\n'; return 1; }
    auto package = sge::package::PackageReader::Read(std::move(bytes).Value());
    if (!package) { std::cerr << "Package rejected: " << package.Error().message << '\n'; return 2; }
    auto window = sge::platform::Win32Window::Create(L"SGE2 Slice 15 WARP Recovery Test", 64, 64);
    if (!window) { std::cerr << "Window creation failed: " << window.Error() << '\n'; return 3; }

    sge::d3d12::D3D12Executor executor({true, true});
    auto loaded = sge::runtime::LoadPackage(std::move(package).Value(), executor, *window.Value());
    if (!loaded)
    {
        std::cerr << "WARP readback failed at " << loaded.Error().stage << ": " << loaded.Error().message << '\n';
        return 4;
    }

    const auto constantBytes = std::as_bytes(std::span<const float>(Identity));
    const sge::runtime::DynamicDataBinding dynamicBinding{0, constantBytes};
    auto external = executor.CreateExternalColorBuffer(loaded.Value().Instance(), ExternalColor);
    if (!external) return 5;
    sge::runtime::ExternalResourceBinding externalBinding{0, external.Value().resource, external.Value().availableAfter};

    for (std::uint64_t frameNumber = 0; frameNumber < 2; ++frameNumber)
    {
        sge::runtime::FrameInvocation invocation;
        invocation.frameNumber = frameNumber;
        invocation.dynamicData = std::span<const sge::runtime::DynamicDataBinding>(&dynamicBinding, 1);
        invocation.externalResources = std::span<const sge::runtime::ExternalResourceBinding>(&externalBinding, 1);
        auto submitted = sge::runtime::Submit(loaded.Value(), executor, invocation);
        if (!submitted || submitted.Value().deviceEpoch != 1 || submitted.Value().frameSlot != frameNumber ||
            submitted.Value().releasedExternalResources.size() != 1 || !submitted.Value().releasedExternalResources[0].safeAfter)
            return 6;
        externalBinding.availableAfter = submitted.Value().releasedExternalResources[0].safeAfter;
    }

    auto falseDetectedLoss = sge::runtime::RecoverDevice(
        loaded.Value(), executor, sge::runtime::DeviceRecoveryMode::RecoverDetectedLoss);
    if (falseDetectedLoss || falseDetectedLoss.Error().stage != "recovery/detected-loss")
        return 7;

    auto controlled = sge::runtime::RecoverDevice(
        loaded.Value(), executor, sge::runtime::DeviceRecoveryMode::ControlledRebuild);
    if (!controlled)
    {
        std::cerr << "Controlled reconstruction failed at " << controlled.Error().stage << ": " << controlled.Error().message << '\n';
        return 8;
    }
    const auto& controlledReport = controlled.Value();
    if (controlledReport.stateBefore != sge::runtime::DeviceRuntimeState::Active ||
        controlledReport.stateAfter != sge::runtime::DeviceRuntimeState::Active ||
        controlledReport.previousDeviceEpoch != 1 || controlledReport.newDeviceEpoch != 2 ||
        controlledReport.forcedRemoval || !controlledReport.adapterReacquired ||
        !controlledReport.packageObjectsRebuilt || !controlledReport.temporalHistoryReset ||
        !controlledReport.externalRebindRequired || controlledReport.removalReason != 0)
        return 9;

    sge::runtime::FrameInvocation staleInvocation;
    staleInvocation.frameNumber = 0;
    staleInvocation.dynamicData = std::span<const sge::runtime::DynamicDataBinding>(&dynamicBinding, 1);
    staleInvocation.externalResources = std::span<const sge::runtime::ExternalResourceBinding>(&externalBinding, 1);
    auto staleSubmit = sge::runtime::Submit(loaded.Value(), executor, staleInvocation);
    if (staleSubmit || staleSubmit.Error().stage != "invocation") return 10;

    auto rebound = executor.CreateExternalColorBuffer(loaded.Value().Instance(), ExternalColor);
    if (!rebound) return 11;
    externalBinding = {0, rebound.Value().resource, rebound.Value().availableAfter};
    sge::runtime::FrameInvocation epoch2Invocation;
    epoch2Invocation.frameNumber = 0;
    epoch2Invocation.dynamicData = std::span<const sge::runtime::DynamicDataBinding>(&dynamicBinding, 1);
    epoch2Invocation.externalResources = std::span<const sge::runtime::ExternalResourceBinding>(&externalBinding, 1);
    auto epoch2Frame = sge::runtime::Submit(loaded.Value(), executor, epoch2Invocation);
    if (!epoch2Frame || epoch2Frame.Value().deviceEpoch != 2 || epoch2Frame.Value().reusedSlotFenceValue != 0 ||
        epoch2Frame.Value().temporalDependencyFenceValue != 0 || epoch2Frame.Value().temporalPreviousInstance != 1 ||
        epoch2Frame.Value().temporalCurrentInstance != 0)
        return 12;

    auto removed = sge::runtime::RecoverDevice(
        loaded.Value(), executor, sge::runtime::DeviceRecoveryMode::ForceRemovalForTest);
    if (!removed)
    {
        std::cerr << "Forced removal failed at " << removed.Error().stage << ": " << removed.Error().message << '\n';
        return 13;
    }
    const auto& removedReport = removed.Value();
    if (!removedReport.forcedRemoval || removedReport.stateBefore != sge::runtime::DeviceRuntimeState::Active ||
        removedReport.stateAfter != sge::runtime::DeviceRuntimeState::AwaitingAdapter ||
        removedReport.previousDeviceEpoch != 2 || removedReport.newDeviceEpoch != 2 ||
        removedReport.removalReason == 0 || removedReport.adapterReacquired ||
        removedReport.packageObjectsRebuilt || removedReport.temporalHistoryReset ||
        !removedReport.externalRebindRequired)
        return 14;

    auto inactiveSubmit = sge::runtime::Submit(loaded.Value(), executor, epoch2Invocation);
    if (inactiveSubmit || inactiveSubmit.Error().stage != "submit/device-state") return 15;
    auto inactiveExternal = executor.CreateExternalColorBuffer(loaded.Value().Instance(), ExternalColor);
    if (inactiveExternal || inactiveExternal.Error().stage != "external/device-state") return 16;

    auto retry = sge::runtime::RecoverDevice(
        loaded.Value(), executor, sge::runtime::DeviceRecoveryMode::RetryAdapterReacquisition);
    if (!retry || retry.Value().stateBefore != sge::runtime::DeviceRuntimeState::AwaitingAdapter ||
        retry.Value().stateAfter != sge::runtime::DeviceRuntimeState::AwaitingAdapter ||
        retry.Value().adapterReacquired || retry.Value().packageObjectsRebuilt || retry.Value().newDeviceEpoch != 2)
        return 17;

    const int childResult = RunFreshProcess(packagePath);
    if (childResult != 0)
    {
        std::cerr << "Fresh-process rematerialization child failed with code " << childResult << '\n';
        return 18;
    }

    std::cout << "D3D12 WARP controlled reconstruction, actual RemoveDevice/DRED, AwaitingAdapter state, removed-LUID exclusion, and fresh-process rematerialization tests passed.\n";
    return 0;
}
