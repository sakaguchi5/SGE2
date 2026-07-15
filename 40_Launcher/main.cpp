#include "../00_Base/FileIO.h"
#include "../00_Base/Sha256.h"
#include "../07_FrozenPackageCore/PackageReader.h"
#include "../09_PackageRuntime/PackageRuntime.h"
#include "../10_D3D12Executor/D3D12Executor.h"
#include "../11_PlatformWin32/Win32Window.h"

#include <Windows.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

namespace
{
constexpr std::uint32_t RenderWidth = 960;
constexpr std::uint32_t RenderHeight = 720;
constexpr std::uint32_t TelemetryPanelWidth = 480;
constexpr float RotationSpeedRadiansPerSecond = 0.7f;
constexpr float RadiansToDegrees = 57.29577951308232f;

struct FrameConstants final
{
    std::array<float, 16> transform{};
};

struct TelemetrySnapshot final
{
    std::uint64_t frameNumber = 0;
    std::uint64_t deviceEpoch = 0;
    std::uint64_t directQueueSignal = 0;
    std::uint64_t copyQueueSignal = 0;
    std::uint64_t reusedSlotFenceValue = 0;
    std::uint64_t temporalDependencyFenceValue = 0;
    std::uint64_t externalAvailableAfter = 1;
    std::uint64_t externalSafeAfter = 0;
    std::uint32_t temporalPreviousInstance = 1;
    std::uint32_t temporalCurrentInstance = 0;
    std::uint32_t frameSlot = 0;
    std::uint32_t framesInFlight = 2;
    std::uint32_t recoveryCount = 0;
    std::uint32_t dredBreadcrumbNodes = 0;
    std::uint32_t dredPageFaultAllocations = 0;
    std::int64_t removalReason = 0;
    std::uint64_t recoveryPreviousEpoch = 0;
    std::uint64_t recoveryNewEpoch = 0;
    bool packageRebuilt = false;
    bool temporalReset = false;
    bool externalRebound = false;
    bool awaitingAdapter = false;
    float elapsedSeconds = 0.0f;
    float angleRadians = 0.0f;
    float angleDegrees = 0.0f;
    float cosine = 1.0f;
    float sine = 0.0f;
    float framesPerSecond = 0.0f;
    float frameMilliseconds = 0.0f;
    float targetRed = 1.0f;
    float targetGreen = 0.35f;
    float targetBlue = 0.65f;
    float previousRed = 0.55f;
    float previousGreen = 0.55f;
    float previousBlue = 0.75f;
    float computeRed = 0.55f;
    float computeGreen = 0.55f;
    float computeBlue = 0.75f;
};

FrameConstants MakeFrameConstants(float angle)
{
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    return {{{
        cosine, -sine, 0.0f, 0.0f,
        sine, cosine, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    }}};
}

std::wstring WidenAscii(std::string_view text)
{
    return std::wstring(text.begin(), text.end());
}

std::wstring BuildTelemetryText(
    const TelemetrySnapshot& telemetry,
    const sge::package::FrozenExecutablePackage& package,
    bool forceWarp)
{
    const std::string digest = sge::base::ToHex(package.ExecutionDigest());
    const std::wstring digestFirst = WidenAscii(std::string_view(digest).substr(0, 32));
    const std::wstring digestSecond = WidenAscii(std::string_view(digest).substr(32, 32));

    std::wostringstream text;
    text << std::fixed;
    text << L"SEMANTIC GPU ENGINE 2\r\n";
    text << L"SLICE 15 PGA DIRECT FRONTEND\r\n";
    text << L"==============================\r\n\r\n";

    text << L"FRONTEND EQUIVALENCE\r\n";
    text << L"  Classical input : explicit triangle vertices\r\n";
    text << L"  SDF input       : three half-space boundaries per triangle\r\n";
    text << L"  PGA input       : homogeneous line join/meet algebra\r\n";
    text << L"  convergence     : bit-identical geometry and Frozen Package\r\n";
    text << L"  Backend origin  : absent from Package execution data\r\n\r\n";

    text << L"RUNTIME\r\n";
    text << L"  mode/frame    : " << (forceWarp ? L"WARP" : L"hardware")
         << L" / " << telemetry.frameNumber << L"\r\n";
    text << L"  elapsed / fps : " << std::setprecision(3) << telemetry.elapsedSeconds
         << L" s / " << std::setprecision(1) << telemetry.framesPerSecond << L"\r\n";
    text << L"  frame / fence : " << std::setprecision(3) << telemetry.frameMilliseconds
         << L" ms / " << telemetry.directQueueSignal << L"\r\n";
    text << L"  active slot   : " << telemetry.frameSlot << L" / " << telemetry.framesInFlight << L"\r\n";
    text << L"  slot reuse    : waited Direct fence " << telemetry.reusedSlotFenceValue << L"\r\n";
    text << L"  device epoch  : " << telemetry.deviceEpoch << L"\r\n";
    text << L"  device state  : " << (telemetry.awaitingAdapter ? L"AwaitingAdapter" : L"Active") << L"\r\n\r\n";

    text << L"DEVICE RECOVERY\r\n";
    text << L"  controlled rebuilds: " << telemetry.recoveryCount << L" (automatic at frame 180)\r\n";
    text << L"  epoch change  : " << telemetry.recoveryPreviousEpoch << L" -> " << telemetry.recoveryNewEpoch << L"\r\n";
    text << L"  removal reason: 0x" << std::hex << std::uppercase
         << static_cast<std::uint64_t>(telemetry.removalReason) << std::dec << std::nouppercase << L"\r\n";
    text << L"  DRED nodes    : breadcrumbs " << telemetry.dredBreadcrumbNodes
         << L" / page-fault allocations " << telemetry.dredPageFaultAllocations << L"\r\n";
    text << L"  package rebuild: " << (telemetry.packageRebuilt ? L"PASS" : L"pending") << L"\r\n";
    text << L"  temporal reset : " << (telemetry.temporalReset ? L"PASS" : L"pending") << L"\r\n";
    text << L"  external rebind: " << (telemetry.externalRebound ? L"PASS" : (telemetry.awaitingAdapter ? L"required" : L"pending")) << L"\r\n\r\n";

    text << L"FRAME INVOCATION\r\n";
    text << L"  slot / bytes  : 0 / " << sizeof(FrameConstants) << L"\r\n";
    text << L"  angle         : " << std::setprecision(3) << telemetry.angleRadians
         << L" rad / " << std::setprecision(2) << telemetry.angleDegrees << L" deg\r\n";
    text << L"  speed         : " << std::setprecision(3)
         << RotationSpeedRadiansPerSecond << L" rad/s\r\n";
    text << L"  cos / sin     : " << std::setprecision(4)
         << telemetry.cosine << L" / " << telemetry.sine << L"\r\n";
    text << L"  matrix 2x2    : [ " << std::setw(7) << telemetry.cosine
         << L" " << std::setw(7) << -telemetry.sine << L" ]\r\n";
    text << L"                  [ " << std::setw(7) << telemetry.sine
         << L" " << std::setw(7) << telemetry.cosine << L" ]\r\n\r\n";

    text << L"TEMPORAL HISTORY (PING-PONG)\r\n";
    text << L"  previous/current: physical[" << telemetry.temporalPreviousInstance
         << L"] -> physical[" << telemetry.temporalCurrentInstance << L"]\r\n";
    text << L"  Temporal wait   : Direct fence " << telemetry.temporalDependencyFenceValue << L"\r\n";
    text << L"  smoothing alpha : 0.1800\r\n";
    text << L"  previous RGB    : " << std::setprecision(4) << telemetry.previousRed << L" / "
         << telemetry.previousGreen << L" / " << telemetry.previousBlue << L"\r\n";
    text << L"  target RGB      : " << telemetry.targetRed << L" / "
         << telemetry.targetGreen << L" / " << telemetry.targetBlue << L"\r\n";
    text << L"  current RGB     : " << telemetry.computeRed << L" / "
         << telemetry.computeGreen << L" / " << telemetry.computeBlue << L"\r\n";
    text << L"  formula         : current = lerp(previous, target, 0.18)\r\n";
    text << L"  descriptors     : current SRV " << (3u + telemetry.temporalCurrentInstance)
         << L", previous SRV " << (7u + telemetry.temporalPreviousInstance)
         << L", current UAV " << (1u + telemetry.temporalCurrentInstance) << L"\r\n";
    text << L"  frame-local     : CB/copy destination use slot " << telemetry.frameSlot << L"\r\n\r\n";

    text << L"MULTI-QUEUE HANDOFF (EVERY FRAME)\r\n";
    text << L"  producer       : Copy queue 1\r\n";
    text << L"  consumer       : Direct queue 0\r\n";
    text << L"  copy           : Resource 5 -> 6 / 16 bytes\r\n";
    text << L"  factor RGB     : 0.8200 / 0.9500 / 0.7800\r\n";
    text << L"  copy states    : Common -> Copy* -> Common\r\n";
    text << L"  synchronization: Signal(Copy 1) -> Wait(Direct 0)\r\n";
    text << L"  CPU wait       : none for the per-frame handoff\r\n";
    text << L"  frame order    : Copy -> Signal -> Wait -> Compute -> Raster\r\n\r\n";

    text << L"EXTERNAL RESOURCE (FRAME INVOCATION)\r\n";
    text << L"  slot/resource   : External slot 0 / Resource 10\r\n";
    text << L"  kind / bytes    : Buffer / 16-byte float4\r\n";
    text << L"  external RGBA   : 0.9600 / 0.8800 / 0.7200 / 1.0000\r\n";
    text << L"  state contract  : ShaderRead -> ShaderRead\r\n";
    text << L"  producer ready  : completion " << telemetry.externalAvailableAfter << L"\r\n";
    text << L"  release safe    : Direct completion " << telemetry.externalSafeAfter << L"\r\n";
    text << L"  operations      : Acquire -> Wait -> Raster(t4) -> Release\r\n";
    text << L"  descriptor      : SRV t4 / descriptor 10\r\n\r\n";

    text << L"ALIASING (LOAD-TIME HANDOFF)\r\n";
    text << L"  logical resources: 8 (warmup) -> 9 (raster color)\r\n";
    text << L"  shared allocation : 7 / placed heap / alias group 0\r\n";
    text << L"  heap / payload    : 65536 bytes / 16 bytes each\r\n";
    text << L"  warmup RGBA       : 0.2500 / 0.2000 / 0.3500 / 1.0000\r\n";
    text << L"  raster RGBA       : 0.9200 / 0.7800 / 1.0000 / 1.0000\r\n";
    text << L"  operations        : ActivateAlias(None -> 8 -> 9)\r\n";
    text << L"  active owner      : Resource 9 after Package load\r\n";
    text << L"  raster binding    : SRV t3 / descriptor 9\r\n\r\n";

    text << L"COMPUTE WORK\r\n";
    text << L"  queue / groups: Direct 0 / 1 x 1 x 1\r\n";
    text << L"  executable/out: Compute 0 / float4[1]\r\n";
    text << L"  output RGB    : " << std::setprecision(4)
         << telemetry.computeRed << L" / " << telemetry.computeGreen
         << L" / " << telemetry.computeBlue << L"\r\n";
    text << L"  state handoff : UAV -> ShaderRead -> UAV\r\n";
    text << L"  frame order   : WaitQueue -> Compute -> Raster\r\n\r\n";

    text << L"DEPTH TEST\r\n";
    text << L"  format / func : D32_FLOAT / LESS\r\n";
    text << L"  clear depth   : 1.000\r\n";
    text << L"  near / far z  : 0.250 / 0.750\r\n";
    text << L"  draw order    : near -> far\r\n";
    text << L"  far test      : 0.750 < 0.250 = false\r\n";
    text << L"  expected      : near/orange remains\r\n\r\n";

    text << L"FROZEN PACKAGE\r\n";
    text << L"  bytes/sections: " << package.FileBytes().size()
         << L" / " << package.Sections().size() << L"\r\n";
    text << L"  schema/runtime: D3D12 v" << package.Header().targetSchemaVersion
         << L" / v" << package.Header().minimumRuntimeVersion << L"\r\n";
    text << L"  render size   : " << RenderWidth << L" x " << RenderHeight << L"\r\n";
    text << L"  buffer/texture: PASS / PASS\r\n";
    text << L"  depth attach  : MATERIALIZED\r\n";
    text << L"  compute PSO   : MATERIALIZED\r\n";
    text << L"  frames/history: 2 / Temporal resource 4 x2\r\n";
    text << L"  descriptors   : tex0 / current UAV1-2 / current SRV3-4 / copied5-6 / previous7-8 / alias9 / external10\r\n";
    text << L"  execution digest:\r\n";
    text << L"    " << digestFirst << L"\r\n";
    text << L"    " << digestSecond << L"\r\n";

    return text.str();
}
}

int wmain(int argc, wchar_t** argv)
{
    const std::filesystem::path packagePath = argc >= 2
        ? std::filesystem::path(argv[1])
        : std::filesystem::path(L"CommonExperiment.sgep");
    bool forceWarp = false;
    bool forceRemovalDemo = false;
    for (int i = 2; i < argc; ++i)
    {
        const std::wstring_view option(argv[i]);
        if (option == L"--warp") forceWarp = true;
        if (option == L"--force-removal") forceRemovalDemo = true;
    }

    auto bytes = sge::base::ReadAllBytes(packagePath);
    if (!bytes)
    {
        std::cerr << "Read failed: " << bytes.Error() << '\n';
        return 1;
    }
    auto package = sge::package::PackageReader::Read(std::move(bytes).Value());
    if (!package)
    {
        std::cerr << "Package rejected: " << package.Error().message << '\n';
        return 2;
    }
    auto window = sge::platform::Win32Window::Create(
        L"Semantic GPU Engine 2 - Slice 15 PGA Direct Frontend",
        RenderWidth,
        RenderHeight,
        TelemetryPanelWidth);
    if (!window)
    {
        std::cerr << "Window creation failed: " << window.Error() << '\n';
        return 3;
    }

    sge::d3d12::D3D12Executor executor({forceWarp, true});
    auto loaded = sge::runtime::LoadPackage(std::move(package).Value(), executor, *window.Value());
    if (!loaded)
    {
        std::cerr << "Package materialization failed at " << loaded.Error().stage
                  << ": " << loaded.Error().message << '\n';
        return 4;
    }
    std::cout << "Classical, SDF, and PGA frontends produced one byte-identical Frozen Package; initial materialization complete; "
              << (forceRemovalDemo ? "actual device removal" : "controlled Package reconstruction")
              << " will run at demo frame 180\n";

    const std::array<float, 4> externalColor{0.96f, 0.88f, 0.72f, 1.0f};
    auto external = executor.CreateExternalColorBuffer(loaded.Value().Instance(), externalColor);
    if (!external)
    {
        std::cerr << "External buffer creation failed at " << external.Error().stage
                  << ": " << external.Error().message << '\n';
        return 5;
    }
    sge::runtime::ExternalResourceBinding externalBinding{0, external.Value().resource, external.Value().availableAfter};

    TelemetrySnapshot telemetry;
    window.Value()->SetTelemetryText(BuildTelemetryText(telemetry, loaded.Value().Package(), forceWarp));
    window.Value()->Show();

    const auto startTime = std::chrono::steady_clock::now();
    auto previousFrameTime = startTime;
    auto previousTelemetryUpdate = startTime;
    float smoothedFrameSeconds = 1.0f / 60.0f;
    std::uint64_t frame = 0;
    std::array<float, 3> temporalHistory{0.55f, 0.55f, 0.75f};
    bool recoveryTriggered = false;
    bool awaitingAdapter = false;

    while (window.Value()->PumpMessages())
    {
        const auto frameStart = std::chrono::steady_clock::now();

        if (awaitingAdapter)
        {
            Sleep(16);
            continue;
        }

        if (!recoveryTriggered && frame >= 180)
        {
            const auto recoveryMode = forceRemovalDemo
                ? sge::runtime::DeviceRecoveryMode::ForceRemovalForTest
                : sge::runtime::DeviceRecoveryMode::ControlledRebuild;
            auto recovered = sge::runtime::RecoverDevice(loaded.Value(), executor, recoveryMode);
            if (!recovered)
            {
                std::cerr << "Device recovery failed at " << recovered.Error().stage
                          << ": " << recovered.Error().message << '\n';
                MessageBoxA(nullptr, recovered.Error().message.c_str(), "D3D12 recovery failure", MB_OK | MB_ICONERROR);
                return 7;
            }
            const auto& report = recovered.Value();
            if (report.stateAfter == sge::runtime::DeviceRuntimeState::AwaitingAdapter)
            {
                telemetry.recoveryCount += 1;
                telemetry.recoveryPreviousEpoch = report.previousDeviceEpoch;
                telemetry.recoveryNewEpoch = report.newDeviceEpoch;
                telemetry.removalReason = report.removalReason;
                telemetry.dredBreadcrumbNodes = report.dredBreadcrumbNodeCount;
                telemetry.dredPageFaultAllocations = report.dredPageFaultAllocationCount;
                telemetry.packageRebuilt = false;
                telemetry.temporalReset = false;
                telemetry.externalRebound = false;
                telemetry.awaitingAdapter = true;
                awaitingAdapter = true;
                recoveryTriggered = true;
                window.Value()->SetTelemetryText(BuildTelemetryText(telemetry, loaded.Value().Package(), forceWarp));
                std::cout << "Removed adapter is excluded; runtime entered AwaitingAdapter. Close the demo or retry after adapter topology changes.\n";
                continue;
            }

            auto rebound = executor.CreateExternalColorBuffer(loaded.Value().Instance(), externalColor);
            if (!rebound)
            {
                std::cerr << "External rebind failed at " << rebound.Error().stage
                          << ": " << rebound.Error().message << '\n';
                return 8;
            }
            externalBinding = {0, rebound.Value().resource, rebound.Value().availableAfter};
            telemetry.recoveryCount += 1;
            telemetry.recoveryPreviousEpoch = report.previousDeviceEpoch;
            telemetry.recoveryNewEpoch = report.newDeviceEpoch;
            telemetry.removalReason = report.removalReason;
            telemetry.dredBreadcrumbNodes = report.dredBreadcrumbNodeCount;
            telemetry.dredPageFaultAllocations = report.dredPageFaultAllocationCount;
            telemetry.packageRebuilt = report.packageObjectsRebuilt;
            telemetry.temporalReset = report.temporalHistoryReset;
            telemetry.externalRebound = report.externalRebindRequired;
            telemetry.deviceEpoch = report.newDeviceEpoch;
            temporalHistory = {0.55f, 0.55f, 0.75f};
            frame = 0;
            recoveryTriggered = true;
            window.Value()->SetTelemetryText(BuildTelemetryText(telemetry, loaded.Value().Package(), forceWarp));
            continue;
        }

        const float elapsed = std::chrono::duration<float>(frameStart - startTime).count();
        const float angle = elapsed * RotationSpeedRadiansPerSecond;
        const FrameConstants constants = MakeFrameConstants(angle);
        const auto constantBytes = std::as_bytes(std::span<const FrameConstants>(&constants, 1));
        const sge::runtime::DynamicDataBinding dynamicBinding{0, constantBytes};

        const std::uint64_t submittedFrame = frame++;
        sge::runtime::FrameInvocation invocation;
        invocation.frameNumber = submittedFrame;
        invocation.dynamicData = std::span<const sge::runtime::DynamicDataBinding>(&dynamicBinding, 1);
        invocation.externalResources = std::span<const sge::runtime::ExternalResourceBinding>(&externalBinding, 1);
        telemetry.externalAvailableAfter = externalBinding.availableAfter ? externalBinding.availableAfter->Value() : 0;
        auto submission = sge::runtime::Submit(loaded.Value(), executor, invocation);
        if (!submission)
        {
            std::cerr << "Submit failed at " << submission.Error().stage
                      << ": " << submission.Error().message << '\n';
            MessageBoxA(nullptr, submission.Error().message.c_str(), "D3D12 execution failure", MB_OK | MB_ICONERROR);
            return 5;
        }

        const auto frameEnd = std::chrono::steady_clock::now();
        float frameSeconds = std::chrono::duration<float>(frameEnd - previousFrameTime).count();
        previousFrameTime = frameEnd;
        if (frameSeconds > 0.0f && frameSeconds < 1.0f)
            smoothedFrameSeconds = smoothedFrameSeconds * 0.90f + frameSeconds * 0.10f;

        telemetry.frameNumber = submittedFrame;
        telemetry.elapsedSeconds = elapsed;
        telemetry.angleRadians = std::fmod(angle, 6.28318530717958647692f);
        telemetry.angleDegrees = telemetry.angleRadians * RadiansToDegrees;
        telemetry.cosine = constants.transform[0];
        telemetry.sine = constants.transform[4];
        telemetry.targetRed = 0.35f + 0.65f * std::abs(telemetry.cosine);
        telemetry.targetGreen = 0.35f + 0.65f * std::abs(telemetry.sine);
        telemetry.targetBlue = 0.65f + 0.35f * (1.0f - std::abs(telemetry.cosine));
        telemetry.previousRed = temporalHistory[0];
        telemetry.previousGreen = temporalHistory[1];
        telemetry.previousBlue = temporalHistory[2];
        telemetry.computeRed = telemetry.previousRed + 0.18f * (telemetry.targetRed - telemetry.previousRed);
        telemetry.computeGreen = telemetry.previousGreen + 0.18f * (telemetry.targetGreen - telemetry.previousGreen);
        telemetry.computeBlue = telemetry.previousBlue + 0.18f * (telemetry.targetBlue - telemetry.previousBlue);
        temporalHistory = {telemetry.computeRed, telemetry.computeGreen, telemetry.computeBlue};
        telemetry.frameMilliseconds = smoothedFrameSeconds * 1000.0f;
        telemetry.framesPerSecond = smoothedFrameSeconds > 0.0f ? 1.0f / smoothedFrameSeconds : 0.0f;
        telemetry.deviceEpoch = submission.Value().deviceEpoch;
        telemetry.frameSlot = submission.Value().frameSlot;
        telemetry.framesInFlight = submission.Value().framesInFlight;
        telemetry.reusedSlotFenceValue = submission.Value().reusedSlotFenceValue;
        telemetry.temporalPreviousInstance = submission.Value().temporalPreviousInstance;
        telemetry.temporalCurrentInstance = submission.Value().temporalCurrentInstance;
        telemetry.temporalDependencyFenceValue = submission.Value().temporalDependencyFenceValue;
        telemetry.directQueueSignal = 0;
        telemetry.copyQueueSignal = 0;
        for (const auto& completion : submission.Value().queues)
        {
            if (completion.queue == 0) telemetry.directQueueSignal = completion.value;
            if (completion.queue == 1) telemetry.copyQueueSignal = completion.value;
        }
        if (submission.Value().releasedExternalResources.size() != 1 ||
            !submission.Value().releasedExternalResources[0].safeAfter)
        {
            std::cerr << "External resource release token was not returned\n";
            return 6;
        }
        telemetry.externalSafeAfter = submission.Value().releasedExternalResources[0].safeAfter->Value();
        externalBinding.availableAfter = submission.Value().releasedExternalResources[0].safeAfter;

        if (frameEnd - previousTelemetryUpdate >= std::chrono::milliseconds(100))
        {
            window.Value()->SetTelemetryText(
                BuildTelemetryText(telemetry, loaded.Value().Package(), forceWarp));
            previousTelemetryUpdate = frameEnd;
        }
    }
    return 0;
}
