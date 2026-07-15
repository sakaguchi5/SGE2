#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "D3D12Executor.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

// Windows SDK compatibility: keep legacy min/max macros from rewriting
// qualified C++ calls such as std::max(...).
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <climits>
#include <cstring>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

namespace sge::d3d12
{
using Microsoft::WRL::ComPtr;
namespace pkg = package::d3d12_v13;

namespace
{
runtime::RuntimeError Error(std::string stage, std::string message)
{
    return {std::move(stage), std::move(message)};
}

std::string HResultText(HRESULT hr)
{
    std::ostringstream stream;
    stream << "HRESULT 0x" << std::hex << std::uppercase << static_cast<std::uint32_t>(hr);
    return stream.str();
}

runtime::RuntimeError HResultError(std::string stage, HRESULT hr, ID3D12Device* device = nullptr)
{
    std::string message = HResultText(hr);
    if (device && (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET))
        message += ", removal reason " + HResultText(device->GetDeviceRemovedReason());
    return Error(std::move(stage), std::move(message));
}

struct ProcessDiagnosticsState final
{
    bool debugLayerEnabled = false;
};

const ProcessDiagnosticsState& ConfigureD3D12DiagnosticsOnce(bool requestDebugLayer)
{
    static std::once_flag configureOnce;
    static ProcessDiagnosticsState state;

    std::call_once(configureOnce, [requestDebugLayer]()
    {
        // DRED and the D3D12 debug layer are process-wide runtime settings.
        // They must be configured before the first device is created and must
        // not be repeated while an old device may still be alive during recovery.
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dredSettings;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings))))
        {
            dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        }

#if defined(_DEBUG)
        if (requestDebugLayer)
        {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            {
                debug->EnableDebugLayer();
                state.debugLayerEnabled = true;
            }
        }
#else
        (void)requestDebugLayer;
#endif
    });

    return state;
}


bool SameLuid(const LUID& left, const LUID& right) noexcept
{
    return left.LowPart == right.LowPart && left.HighPart == right.HighPart;
}

DXGI_FORMAT ToDxgi(pkg::Format format)
{
    switch (format)
    {
    case pkg::Format::R32G32Float: return DXGI_FORMAT_R32G32_FLOAT;
    case pkg::Format::R32G32B32Float: return DXGI_FORMAT_R32G32B32_FLOAT;
    case pkg::Format::R32G32B32A32Float: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case pkg::Format::B8G8R8A8Unorm: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case pkg::Format::D32Float: return DXGI_FORMAT_D32_FLOAT;
    case pkg::Format::Unknown: return DXGI_FORMAT_UNKNOWN;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

bool IsCopyQueueState(const pkg::ResourceState& state) noexcept
{
    if (state.reserved != 0) return false;
    if (state.stateClass == pkg::StateClass::Common) return state.explicitBits == 0;
    if (state.stateClass != pkg::StateClass::Explicit) return false;
    const auto copySource = static_cast<std::uint32_t>(pkg::ExplicitStateBits::CopySource);
    const auto copyDestination = static_cast<std::uint32_t>(pkg::ExplicitStateBits::CopyDestination);
    return state.explicitBits == copySource || state.explicitBits == copyDestination;
}

D3D12_RESOURCE_STATES ToNativeState(
    const pkg::ResourceState& state,
    D3D12_COMMAND_LIST_TYPE queueType = D3D12_COMMAND_LIST_TYPE_DIRECT)
{
    if (state.stateClass == pkg::StateClass::Common) return D3D12_RESOURCE_STATE_COMMON;
    if (state.stateClass == pkg::StateClass::Present) return D3D12_RESOURCE_STATE_PRESENT;
    D3D12_RESOURCE_STATES result = static_cast<D3D12_RESOURCE_STATES>(0);
    const auto bits = state.explicitBits;
    if (bits & static_cast<std::uint32_t>(pkg::ExplicitStateBits::VertexBuffer)) result |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    if (bits & static_cast<std::uint32_t>(pkg::ExplicitStateBits::ConstantBuffer)) result |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    if (bits & static_cast<std::uint32_t>(pkg::ExplicitStateBits::IndexBuffer)) result |= D3D12_RESOURCE_STATE_INDEX_BUFFER;
    if (bits & static_cast<std::uint32_t>(pkg::ExplicitStateBits::RenderTarget)) result |= D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (bits & static_cast<std::uint32_t>(pkg::ExplicitStateBits::DepthWrite)) result |= D3D12_RESOURCE_STATE_DEPTH_WRITE;
    if (bits & static_cast<std::uint32_t>(pkg::ExplicitStateBits::DepthRead)) result |= D3D12_RESOURCE_STATE_DEPTH_READ;
    if (bits & static_cast<std::uint32_t>(pkg::ExplicitStateBits::ShaderRead))
    {
        // Legacy v13 Package state. New v14 Packages use explicit stage scope.
        result |= queueType == D3D12_COMMAND_LIST_TYPE_COMPUTE
            ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
            : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
    if (bits & static_cast<std::uint32_t>(pkg::ExplicitStateBits::PixelShaderRead))
        result |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (bits & static_cast<std::uint32_t>(pkg::ExplicitStateBits::NonPixelShaderRead))
        result |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (bits & static_cast<std::uint32_t>(pkg::ExplicitStateBits::UnorderedWrite)) result |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (bits & static_cast<std::uint32_t>(pkg::ExplicitStateBits::CopySource)) result |= D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (bits & static_cast<std::uint32_t>(pkg::ExplicitStateBits::CopyDestination)) result |= D3D12_RESOURCE_STATE_COPY_DEST;
    if (bits & static_cast<std::uint32_t>(pkg::ExplicitStateBits::IndirectArgument)) result |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    return result;
}

D3D12_RESOURCE_BARRIER TransitionBarrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

const char* SemanticName(pkg::VertexMeaning meaning)
{
    switch (meaning)
    {
    case pkg::VertexMeaning::Position: return "POSITION";
    case pkg::VertexMeaning::Color: return "COLOR";
    case pkg::VertexMeaning::TexCoord: return "TEXCOORD";
    default: return nullptr;
    }
}

class ExternalBufferResource final : public runtime::IExternalResource
{
public:
    ExternalBufferResource(ComPtr<ID3D12Resource> resource, std::uint64_t epoch, std::uint64_t bytes)
        : resource_(std::move(resource)), epoch_(epoch), bytes_(bytes) {}
    [[nodiscard]] std::uint64_t DeviceEpoch() const noexcept override { return epoch_; }
    [[nodiscard]] std::uint64_t SizeBytes() const noexcept override { return bytes_; }
    [[nodiscard]] ID3D12Resource* Native() const noexcept { return resource_.Get(); }
private:
    ComPtr<ID3D12Resource> resource_;
    std::uint64_t epoch_ = 0;
    std::uint64_t bytes_ = 0;
};

class CompletionToken final : public runtime::ICompletionToken
{
public:
    CompletionToken(ComPtr<ID3D12Fence> fence, std::uint64_t value, std::uint64_t epoch)
        : fence_(std::move(fence)), value_(value), epoch_(epoch) {}
    [[nodiscard]] std::uint64_t DeviceEpoch() const noexcept override { return epoch_; }
    [[nodiscard]] std::uint64_t Value() const noexcept override { return value_; }
    [[nodiscard]] ID3D12Fence* NativeFence() const noexcept { return fence_.Get(); }
private:
    ComPtr<ID3D12Fence> fence_;
    std::uint64_t value_ = 0;
    std::uint64_t epoch_ = 0;
};

class Instance final : public runtime::IPackageInstance
{
public:
    Instance(std::shared_ptr<const package::FrozenExecutablePackage> package, pkg::D3D12PackageView view, runtime::ISurfaceHost& surface, ExecutorOptions options)
        : package_(std::move(package)), view_(std::move(view)), surface_(&surface), options_(options) {}

    ~Instance() override
    {
        if (runtimeState_ == runtime::DeviceRuntimeState::Active)
        {
            WaitForQueueAtDestruction(DirectQueueId(), lastFenceValue_, fenceEvent_);
            if (HasComputeQueue()) WaitForQueueAtDestruction(ComputeQueueId(), computeLastFenceValue_, computeFenceEvent_);
            if (HasCopyQueue()) WaitForQueueAtDestruction(CopyQueueId(), copyLastFenceValue_, copyFenceEvent_);
        }
        ReleaseDeviceObjectsForRecovery();
    }

    base::Result<void, runtime::RuntimeError> Initialize()
    {
        auto baseObjects = CreateBaseObjects();
        if (!baseObjects) return baseObjects;
        resourceStates_.resize(view_.Resources().size());
        resources_.resize(view_.Resources().size());
        externalNativeResources_.resize(view_.Resources().size());
        externalBindings_.resize(view_.ExternalSlots().size());
        externalAcquired_.resize(view_.ExternalSlots().size());
        externalWaited_.resize(view_.ExternalSlots().size());
        externalReleased_.resize(view_.ExternalSlots().size());
        externalConsumerQueues_.assign(view_.ExternalSlots().size(), package::InvalidIndex);
        surfaceAcquired_.resize(view_.SurfaceSlots().size());
        surfacePresented_.resize(view_.SurfaceSlots().size());
        temporalWaitedResources_.resize(view_.Resources().size());
        placedHeaps_.resize(view_.Allocations().size());
        std::uint32_t aliasGroupCount = 0;
        for (const auto& allocation : view_.Allocations())
            if (allocation.aliasGroup != package::InvalidIndex) aliasGroupCount = std::max(aliasGroupCount, allocation.aliasGroup + 1u);
        activeAliasResources_.assign(aliasGroupCount, package::InvalidIndex);
        for (const auto& resource : view_.Resources())
        {
            const std::uint32_t stateCount = resource.origin == pkg::ResourceOrigin::Surface
                ? view_.Profile().surfaceImageCount
                : std::max(1u, resource.physicalInstanceCount);
            resourceStates_[resource.id.value].assign(stateCount, resource.initialState);
            if (resource.origin == pkg::ResourceOrigin::PackageOwned)
                resources_[resource.id.value].resize(resource.physicalInstanceCount);
        }
        frameSlotFenceValues_.assign(view_.Profile().framesInFlight, 0);
        computeFrameSlotFenceValues_.assign(view_.Profile().framesInFlight, 0);
        copyFrameSlotFenceValues_.assign(view_.Profile().framesInFlight, 0);
        rootSignatures_.resize(view_.BindingLayouts().size());
        pipelineStates_.resize(view_.Executables().size());
        computePipelineStates_.resize(view_.ComputeExecutables().size());
        dynamicBindings_.resize(view_.DynamicSlots().size());
        dynamicApplied_.resize(view_.DynamicSlots().size(), false);

        for (const auto& operation : view_.LoadOperations())
        {
            auto result = ExecuteLoadOperation(operation);
            if (!result) return result;
        }
        if (!descriptorHeapsCreated_)
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load", "CreateDescriptorHeaps operation was not executed"));
        if (!loadQueueCompleted_)
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load", "Package-declared load queue batch did not complete"));
        auto verified = CompleteBufferVerifications();
        if (!verified) return verified;
        auto textureVerified = CompleteTextureVerifications();
        if (!textureVerified) return textureVerified;
        uploadResources_.clear();
        return base::Result<void, runtime::RuntimeError>::Success();
    }

    base::Result<ExternalBufferBinding, runtime::RuntimeError> CreateExternalColorBuffer(const std::array<float, 4>& color)
    {
        if (runtimeState_ != runtime::DeviceRuntimeState::Active || !device_)
            return base::Result<ExternalBufferBinding, runtime::RuntimeError>::Failure(
                Error("external/device-state", "External resources can be created only while the Package instance is Active"));
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = sizeof(color);
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        D3D12_HEAP_PROPERTIES defaultHeap{};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        defaultHeap.CreationNodeMask = 1;
        defaultHeap.VisibleNodeMask = 1;
        ComPtr<ID3D12Resource> resource;
        HRESULT hr = device_->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource));
        if (FAILED(hr)) return base::Result<ExternalBufferBinding, runtime::RuntimeError>::Failure(HResultError("external/create-buffer", hr, device_.Get()));

        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        uploadHeap.CreationNodeMask = 1;
        uploadHeap.VisibleNodeMask = 1;
        ComPtr<ID3D12Resource> upload;
        hr = device_->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
        if (FAILED(hr)) return base::Result<ExternalBufferBinding, runtime::RuntimeError>::Failure(HResultError("external/create-upload", hr, device_.Get()));
        void* mapped = nullptr;
        D3D12_RANGE noRead{0, 0};
        hr = upload->Map(0, &noRead, &mapped);
        if (FAILED(hr)) return base::Result<ExternalBufferBinding, runtime::RuntimeError>::Failure(HResultError("external/map-upload", hr, device_.Get()));
        std::memcpy(mapped, color.data(), sizeof(color));
        D3D12_RANGE written{0, sizeof(color)};
        upload->Unmap(0, &written);

        ComPtr<ID3D12CommandAllocator> producerAllocator;
        ComPtr<ID3D12GraphicsCommandList> producerList;
        hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&producerAllocator));
        if (FAILED(hr)) return base::Result<ExternalBufferBinding, runtime::RuntimeError>::Failure(HResultError("external/create-allocator", hr, device_.Get()));
        hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, producerAllocator.Get(), nullptr, IID_PPV_ARGS(&producerList));
        if (FAILED(hr)) return base::Result<ExternalBufferBinding, runtime::RuntimeError>::Failure(HResultError("external/create-command-list", hr, device_.Get()));
        const auto toCopyDestination = TransitionBarrier(resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        producerList->ResourceBarrier(1, &toCopyDestination);
        producerList->CopyBufferRegion(resource.Get(), 0, upload.Get(), 0, sizeof(color));
        const auto incomingState = view_.ExternalSlots().empty()
            ? pkg::ResourceState{pkg::StateClass::Explicit, 0,
                static_cast<std::uint32_t>(pkg::ExplicitStateBits::PixelShaderRead)}
            : view_.ExternalSlots().front().requiredIncomingState;
        const auto shaderRead = ToNativeState(incomingState, D3D12_COMMAND_LIST_TYPE_DIRECT);
        if (shaderRead == static_cast<D3D12_RESOURCE_STATES>(0))
            return base::Result<ExternalBufferBinding, runtime::RuntimeError>::Failure(
                Error("external/incoming-state", "external slot has no executable shader-read state"));
        const auto toShaderRead = TransitionBarrier(resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, shaderRead);
        producerList->ResourceBarrier(1, &toShaderRead);
        hr = producerList->Close();
        if (FAILED(hr)) return base::Result<ExternalBufferBinding, runtime::RuntimeError>::Failure(HResultError("external/close-command-list", hr, device_.Get()));
        ID3D12CommandList* lists[] = {producerList.Get()};
        queue_->ExecuteCommandLists(1, lists);

        ComPtr<ID3D12Fence> producerFence;
        hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&producerFence));
        if (FAILED(hr)) return base::Result<ExternalBufferBinding, runtime::RuntimeError>::Failure(HResultError("external/create-producer-fence", hr, device_.Get()));
        hr = queue_->Signal(producerFence.Get(), 1);
        if (FAILED(hr)) return base::Result<ExternalBufferBinding, runtime::RuntimeError>::Failure(HResultError("external/signal-producer", hr, device_.Get()));
        HANDLE producerEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!producerEvent) return base::Result<ExternalBufferBinding, runtime::RuntimeError>::Failure(Error("external/create-event", "CreateEventW failed"));
        if (producerFence->GetCompletedValue() < 1)
        {
            hr = producerFence->SetEventOnCompletion(1, producerEvent);
            if (FAILED(hr))
            {
                CloseHandle(producerEvent);
                return base::Result<ExternalBufferBinding, runtime::RuntimeError>::Failure(HResultError("external/set-producer-event", hr, device_.Get()));
            }
            WaitForSingleObject(producerEvent, INFINITE);
        }
        CloseHandle(producerEvent);

        ExternalBufferBinding result;
        result.resource = std::make_shared<ExternalBufferResource>(resource, deviceEpoch_, sizeof(color));
        result.availableAfter = std::make_shared<CompletionToken>(producerFence, 1, deviceEpoch_);
        return base::Result<ExternalBufferBinding, runtime::RuntimeError>::Success(std::move(result));
    }

    base::Result<runtime::DeviceRecoveryReport, runtime::RuntimeError> RecoverDevice(runtime::DeviceRecoveryMode mode)
    {
        runtime::DeviceRecoveryReport report;
        report.previousDeviceEpoch = deviceEpoch_;
        report.newDeviceEpoch = deviceEpoch_;
        report.mode = mode;
        report.stateBefore = runtimeState_;
        report.stateAfter = runtimeState_;
        report.forcedRemoval = mode == runtime::DeviceRecoveryMode::ForceRemovalForTest;
        report.externalRebindRequired = !view_.ExternalSlots().empty();

        if (mode == runtime::DeviceRecoveryMode::RetryAdapterReacquisition)
        {
            if (runtimeState_ != runtime::DeviceRuntimeState::AwaitingAdapter)
                return base::Result<runtime::DeviceRecoveryReport, runtime::RuntimeError>::Failure(
                    Error("recovery/retry", "adapter reacquisition is valid only while AwaitingAdapter"));

            auto rebuilt = Initialize();
            if (!rebuilt)
            {
                if (rebuilt.Error().stage == "device/no-eligible-adapter")
                {
                    report.stateAfter = runtime::DeviceRuntimeState::AwaitingAdapter;
                    return base::Result<runtime::DeviceRecoveryReport, runtime::RuntimeError>::Success(report);
                }
                return base::Result<runtime::DeviceRecoveryReport, runtime::RuntimeError>::Failure(
                    Error("recovery/retry", rebuilt.Error().stage + ": " + rebuilt.Error().message));
            }

            ++deviceEpoch_;
            runtimeState_ = runtime::DeviceRuntimeState::Active;
            report.newDeviceEpoch = deviceEpoch_;
            report.stateAfter = runtimeState_;
            report.adapterReacquired = true;
            report.packageObjectsRebuilt = true;
            report.temporalHistoryReset = true;
            return base::Result<runtime::DeviceRecoveryReport, runtime::RuntimeError>::Success(report);
        }

        if (runtimeState_ != runtime::DeviceRuntimeState::Active || !device_)
            return base::Result<runtime::DeviceRecoveryReport, runtime::RuntimeError>::Failure(
                Error("recovery", "recovery requires an Active materialized device"));

        if (mode == runtime::DeviceRecoveryMode::ControlledRebuild)
        {
            auto waited = WaitForQueueFence(DirectQueueId(), lastFenceValue_);
            if (!waited) return base::Result<runtime::DeviceRecoveryReport, runtime::RuntimeError>::Failure(waited.Error());
            if (HasComputeQueue())
            {
                waited = WaitForQueueFence(ComputeQueueId(), computeLastFenceValue_);
                if (!waited) return base::Result<runtime::DeviceRecoveryReport, runtime::RuntimeError>::Failure(waited.Error());
            }
            if (HasCopyQueue())
            {
                waited = WaitForQueueFence(CopyQueueId(), copyLastFenceValue_);
                if (!waited) return base::Result<runtime::DeviceRecoveryReport, runtime::RuntimeError>::Failure(waited.Error());
            }
            const HRESULT sourceDeviceReason = device_->GetDeviceRemovedReason();
            if (FAILED(sourceDeviceReason))
                return base::Result<runtime::DeviceRecoveryReport, runtime::RuntimeError>::Failure(
                    Error("recovery/controlled-source-device",
                        "controlled reconstruction cannot begin because prior Package execution removed the device: " +
                        HResultText(sourceDeviceReason)));

            ReleaseDeviceObjectsForRecovery();
            auto rebuilt = Initialize();
            if (!rebuilt)
                return base::Result<runtime::DeviceRecoveryReport, runtime::RuntimeError>::Failure(
                    Error("recovery/controlled-rebuild", rebuilt.Error().stage + ": " + rebuilt.Error().message));

            ++deviceEpoch_;
            runtimeState_ = runtime::DeviceRuntimeState::Active;
            report.newDeviceEpoch = deviceEpoch_;
            report.stateAfter = runtimeState_;
            report.adapterReacquired = true;
            report.packageObjectsRebuilt = true;
            report.temporalHistoryReset = true;
            return base::Result<runtime::DeviceRecoveryReport, runtime::RuntimeError>::Success(report);
        }

        const bool forceRemoval = mode == runtime::DeviceRecoveryMode::ForceRemovalForTest;
        const bool recoverDetectedLoss = mode == runtime::DeviceRecoveryMode::RecoverDetectedLoss;
        if (!forceRemoval && !recoverDetectedLoss)
            return base::Result<runtime::DeviceRecoveryReport, runtime::RuntimeError>::Failure(
                Error("recovery", "unknown recovery mode"));

        if (forceRemoval)
        {
            ComPtr<ID3D12Device5> removable;
            const HRESULT query = device_.As(&removable);
            if (FAILED(query))
                return base::Result<runtime::DeviceRecoveryReport, runtime::RuntimeError>::Failure(
                    HResultError("recovery/query-device5", query, device_.Get()));
            removable->RemoveDevice();
        }

        const HRESULT reason = device_->GetDeviceRemovedReason();
        report.removalReason = static_cast<std::int64_t>(reason);
        if (SUCCEEDED(reason))
            return base::Result<runtime::DeviceRecoveryReport, runtime::RuntimeError>::Failure(
                Error(forceRemoval ? "recovery/remove-device" : "recovery/detected-loss",
                    forceRemoval
                        ? "RemoveDevice did not transition the device to a removed state"
                        : "RecoverDetectedLoss was requested while the device was not removed"));
        runtimeState_ = runtime::DeviceRuntimeState::Lost;
        if (hasActiveAdapterLuid_)
        {
            excludedAdapterLuid_ = activeAdapterLuid_;
            hasExcludedAdapterLuid_ = true;
            report.removedAdapterLuidLow = activeAdapterLuid_.LowPart;
            report.removedAdapterLuidHigh = activeAdapterLuid_.HighPart;
        }

        ComPtr<ID3D12DeviceRemovedExtendedData> dred;
        if (SUCCEEDED(device_.As(&dred)))
        {
            D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs{};
            if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs)))
                for (auto* node = breadcrumbs.pHeadAutoBreadcrumbNode; node; node = node->pNext)
                    ++report.dredBreadcrumbNodeCount;

            D3D12_DRED_PAGE_FAULT_OUTPUT pageFault{};
            if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pageFault)))
            {
                for (auto* node = pageFault.pHeadExistingAllocationNode; node; node = node->pNext)
                    ++report.dredPageFaultAllocationCount;
                for (auto* node = pageFault.pHeadRecentFreedAllocationNode; node; node = node->pNext)
                    ++report.dredPageFaultAllocationCount;
            }
        }

        ReleaseDeviceObjectsForRecovery();
        runtimeState_ = runtime::DeviceRuntimeState::AwaitingAdapter;

        auto rebuilt = Initialize();
        if (!rebuilt)
        {
            if (rebuilt.Error().stage == "device/no-eligible-adapter")
            {
                report.stateAfter = runtimeState_;
                return base::Result<runtime::DeviceRecoveryReport, runtime::RuntimeError>::Success(report);
            }
            return base::Result<runtime::DeviceRecoveryReport, runtime::RuntimeError>::Failure(
                Error("recovery/reacquire-adapter", rebuilt.Error().stage + ": " + rebuilt.Error().message));
        }

        ++deviceEpoch_;
        runtimeState_ = runtime::DeviceRuntimeState::Active;
        report.newDeviceEpoch = deviceEpoch_;
        report.stateAfter = runtimeState_;
        report.adapterReacquired = true;
        report.packageObjectsRebuilt = true;
        report.temporalHistoryReset = true;
        return base::Result<runtime::DeviceRecoveryReport, runtime::RuntimeError>::Success(report);
    }

    base::Result<runtime::FrameSubmission, runtime::RuntimeError> Submit(const runtime::FrameInvocation& invocation)
    {
        if (runtimeState_ != runtime::DeviceRuntimeState::Active || !device_)
            return base::Result<runtime::FrameSubmission, runtime::RuntimeError>::Failure(
                Error("submit/device-state", "Package instance is not Active; adapter reacquisition is required"));
        const bool hasTemporalResources = std::any_of(view_.Resources().begin(), view_.Resources().end(),
            [&](const pkg::ResourceArtifact& resource) { return IsTemporal(resource); });
        if (hasTemporalResources && ((!hasSubmittedFrame_ && invocation.frameNumber != 0) ||
            (hasSubmittedFrame_ && invocation.frameNumber != lastSubmittedFrameNumber_ + 1u)))
            return base::Result<runtime::FrameSubmission, runtime::RuntimeError>::Failure(
                Error("invocation", "Temporal execution requires consecutive frame numbers beginning at zero"));

        currentFrameSlot_ = static_cast<std::uint32_t>(invocation.frameNumber % view_.Profile().framesInFlight);
        const std::uint64_t reusedSlotFenceValue = frameSlotFenceValues_[currentFrameSlot_];
        const std::uint64_t reusedComputeSlotFenceValue = computeFrameSlotFenceValues_[currentFrameSlot_];
        const std::uint64_t reusedCopySlotFenceValue = copyFrameSlotFenceValues_[currentFrameSlot_];
        auto waited = WaitForQueueFence(DirectQueueId(), reusedSlotFenceValue);
        if (!waited) return base::Result<runtime::FrameSubmission, runtime::RuntimeError>::Failure(waited.Error());
        if (HasComputeQueue())
        {
            waited = WaitForQueueFence(ComputeQueueId(), reusedComputeSlotFenceValue);
            if (!waited) return base::Result<runtime::FrameSubmission, runtime::RuntimeError>::Failure(waited.Error());
        }
        if (HasCopyQueue())
        {
            waited = WaitForQueueFence(CopyQueueId(), reusedCopySlotFenceValue);
            if (!waited) return base::Result<runtime::FrameSubmission, runtime::RuntimeError>::Failure(waited.Error());
        }

        auto prepared = PrepareInvocation(invocation);
        if (!prepared) return base::Result<runtime::FrameSubmission, runtime::RuntimeError>::Failure(prepared.Error());

        temporalPreviousInstance_ = (currentFrameSlot_ + view_.Profile().framesInFlight - 1u) % view_.Profile().framesInFlight;
        temporalCurrentInstance_ = currentFrameSlot_;
        previousDirectFenceValue_ = lastFenceValue_;
        previousComputeFenceValue_ = computeLastFenceValue_;
        previousCopyFenceValue_ = copyLastFenceValue_;
        temporalDependencyFenceValue_ = 0;
        frameFenceValue_ = 0;
        computeFrameFenceValue_ = 0;
        copyFrameFenceValue_ = 0;
        directFrameBatchCursor_ = 0;
        computeFrameBatchCursor_ = 0;
        copyFrameBatchCursor_ = 0;
        directFrameSubmitted_ = false;
        computeFrameSubmitted_ = false;
        copyFrameSubmitted_ = false;
        commandOpen_ = false;
        activeCommandQueue_ = package::InvalidIndex;
        copyFrameCommandOpen_ = false;
        std::fill(externalAcquired_.begin(), externalAcquired_.end(), false);
        std::fill(externalWaited_.begin(), externalWaited_.end(), false);
        std::fill(externalReleased_.begin(), externalReleased_.end(), false);
        std::fill(externalConsumerQueues_.begin(), externalConsumerQueues_.end(), package::InvalidIndex);
        std::fill(surfaceAcquired_.begin(), surfaceAcquired_.end(), false);
        std::fill(surfacePresented_.begin(), surfacePresented_.end(), false);
        std::fill(temporalWaitedResources_.begin(), temporalWaitedResources_.end(), false);
        frameExternalReleases_.clear();
        std::fill(dynamicApplied_.begin(), dynamicApplied_.end(), false);

        for (const auto& operation : view_.FrameOperations())
        {
            auto result = ExecuteFrameOperation(operation);
            if (!result) return base::Result<runtime::FrameSubmission, runtime::RuntimeError>::Failure(result.Error());
        }

        if (commandOpen_ || copyFrameCommandOpen_)
            return base::Result<runtime::FrameSubmission, runtime::RuntimeError>::Failure(
                Error("frame", "frame stream ended with an open queue batch"));
        if (directFrameSubmitted_ || computeFrameSubmitted_ || copyFrameSubmitted_)
            return base::Result<runtime::FrameSubmission, runtime::RuntimeError>::Failure(
                Error("frame", "a queue submitted work without a Package SignalQueue operation"));
        if (std::find(externalReleased_.begin(), externalReleased_.end(), false) != externalReleased_.end())
            return base::Result<runtime::FrameSubmission, runtime::RuntimeError>::Failure(
                Error("frame", "frame stream did not release every acquired external resource"));
        if (std::find(surfacePresented_.begin(), surfacePresented_.end(), false) != surfacePresented_.end())
            return base::Result<runtime::FrameSubmission, runtime::RuntimeError>::Failure(
                Error("frame", "frame stream did not present every acquired surface slot"));

        frameSlotFenceValues_[currentFrameSlot_] = frameFenceValue_;
        computeFrameSlotFenceValues_[currentFrameSlot_] = computeFrameFenceValue_;
        copyFrameSlotFenceValues_[currentFrameSlot_] = copyFrameFenceValue_;
        runtime::FrameSubmission submission;
        submission.deviceEpoch = deviceEpoch_;
        submission.frameSlot = currentFrameSlot_;
        submission.framesInFlight = view_.Profile().framesInFlight;
        submission.reusedSlotFenceValue = reusedSlotFenceValue;
        submission.temporalPreviousInstance = temporalPreviousInstance_;
        submission.temporalCurrentInstance = temporalCurrentInstance_;
        submission.temporalDependencyFenceValue = temporalDependencyFenceValue_;
        if (frameFenceValue_ != 0) submission.queues.push_back({DirectQueueId().value, frameFenceValue_});
        if (computeFrameFenceValue_ != 0) submission.queues.push_back({ComputeQueueId().value, computeFrameFenceValue_});
        if (copyFrameFenceValue_ != 0) submission.queues.push_back({CopyQueueId().value, copyFrameFenceValue_});
        submission.releasedExternalResources = frameExternalReleases_;
        hasSubmittedFrame_ = true;
        lastSubmittedFrameNumber_ = invocation.frameNumber;
        return base::Result<runtime::FrameSubmission, runtime::RuntimeError>::Success(std::move(submission));
    }

private:
    [[nodiscard]] pkg::QueueId DirectQueueId() const noexcept { return pkg::QueueId{0}; }
    [[nodiscard]] pkg::QueueId ComputeQueueId() const noexcept
    {
        return pkg::QueueId{view_.Profile().directQueueCount};
    }
    [[nodiscard]] pkg::QueueId CopyQueueId() const noexcept
    {
        return pkg::QueueId{view_.Profile().directQueueCount + view_.Profile().computeQueueCount};
    }
    [[nodiscard]] pkg::QueueId LoadQueueId() const noexcept
    {
        return HasCopyQueue() ? CopyQueueId() : DirectQueueId();
    }
    [[nodiscard]] bool HasComputeQueue() const noexcept { return view_.Profile().computeQueueCount != 0; }
    [[nodiscard]] bool HasCopyQueue() const noexcept { return view_.Profile().copyQueueCount != 0; }
    [[nodiscard]] bool IsDirectQueue(pkg::QueueId queue) const noexcept
    {
        return queue.IsValid() && queue == DirectQueueId();
    }
    [[nodiscard]] bool IsComputeQueue(pkg::QueueId queue) const noexcept
    {
        return HasComputeQueue() && queue.IsValid() && queue == ComputeQueueId();
    }
    [[nodiscard]] bool IsCopyQueue(pkg::QueueId queue) const noexcept
    {
        return HasCopyQueue() && queue.IsValid() && queue == CopyQueueId();
    }
    [[nodiscard]] bool IsSupportedQueue(pkg::QueueId queue) const noexcept
    {
        return IsDirectQueue(queue) || IsComputeQueue(queue) || IsCopyQueue(queue);
    }
    [[nodiscard]] D3D12_COMMAND_LIST_TYPE NativeCommandListType(pkg::QueueId queue) const noexcept
    {
        if (IsDirectQueue(queue)) return D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (IsComputeQueue(queue)) return D3D12_COMMAND_LIST_TYPE_COMPUTE;
        return D3D12_COMMAND_LIST_TYPE_COPY;
    }
    [[nodiscard]] ID3D12CommandQueue* NativeQueue(pkg::QueueId queue) const noexcept
    {
        if (IsDirectQueue(queue)) return queue_.Get();
        if (IsComputeQueue(queue)) return computeQueue_.Get();
        if (IsCopyQueue(queue)) return copyQueue_.Get();
        return nullptr;
    }
    [[nodiscard]] ID3D12Fence* NativeFence(pkg::QueueId queue) const noexcept
    {
        if (IsDirectQueue(queue)) return fence_.Get();
        if (IsComputeQueue(queue)) return computeFence_.Get();
        if (IsCopyQueue(queue)) return copyFence_.Get();
        return nullptr;
    }
    [[nodiscard]] HANDLE NativeFenceEvent(pkg::QueueId queue) const noexcept
    {
        if (IsDirectQueue(queue)) return fenceEvent_;
        if (IsComputeQueue(queue)) return computeFenceEvent_;
        if (IsCopyQueue(queue)) return copyFenceEvent_;
        return nullptr;
    }
    [[nodiscard]] ComPtr<ID3D12Fence> FenceReference(pkg::QueueId queue) const noexcept
    {
        if (IsDirectQueue(queue)) return fence_;
        if (IsComputeQueue(queue)) return computeFence_;
        if (IsCopyQueue(queue)) return copyFence_;
        return {};
    }
    [[nodiscard]] std::uint64_t FrameFenceValue(pkg::QueueId queue) const noexcept
    {
        if (IsDirectQueue(queue)) return frameFenceValue_;
        if (IsComputeQueue(queue)) return computeFrameFenceValue_;
        if (IsCopyQueue(queue)) return copyFrameFenceValue_;
        return 0;
    }
    [[nodiscard]] std::uint64_t PreviousFrameFenceValue(pkg::QueueId queue) const noexcept
    {
        if (IsDirectQueue(queue)) return previousDirectFenceValue_;
        if (IsComputeQueue(queue)) return previousComputeFenceValue_;
        if (IsCopyQueue(queue)) return previousCopyFenceValue_;
        return 0;
    }
    [[nodiscard]] bool FrameQueueSubmitted(pkg::QueueId queue) const noexcept
    {
        if (IsDirectQueue(queue)) return directFrameSubmitted_;
        if (IsComputeQueue(queue)) return computeFrameSubmitted_;
        if (IsCopyQueue(queue)) return copyFrameSubmitted_;
        return false;
    }
    void SetFrameQueueSubmitted(pkg::QueueId queue, bool value) noexcept
    {
        if (IsDirectQueue(queue)) directFrameSubmitted_ = value;
        else if (IsComputeQueue(queue)) computeFrameSubmitted_ = value;
        else if (IsCopyQueue(queue)) copyFrameSubmitted_ = value;
    }
    void SetFrameFenceValue(pkg::QueueId queue, std::uint64_t value) noexcept
    {
        if (IsDirectQueue(queue)) { lastFenceValue_ = value; frameFenceValue_ = value; }
        else if (IsComputeQueue(queue)) { computeLastFenceValue_ = value; computeFrameFenceValue_ = value; }
        else if (IsCopyQueue(queue)) { copyLastFenceValue_ = value; copyFrameFenceValue_ = value; }
    }
    [[nodiscard]] std::uint64_t NextFenceValue(pkg::QueueId queue) noexcept
    {
        if (IsDirectQueue(queue)) return nextFenceValue_++;
        if (IsComputeQueue(queue)) return computeNextFenceValue_++;
        return copyNextFenceValue_++;
    }
    base::Result<void, runtime::RuntimeError> WaitForQueueFence(pkg::QueueId queue, std::uint64_t value)
    {
        if (value == 0) return base::Result<void, runtime::RuntimeError>::Success();
        auto* fence = NativeFence(queue);
        const auto eventHandle = NativeFenceEvent(queue);
        if (!fence || !eventHandle)
            return base::Result<void, runtime::RuntimeError>::Failure(Error("queue/wait", "Package queue fence is unavailable"));
        if (fence->GetCompletedValue() >= value) return base::Result<void, runtime::RuntimeError>::Success();
        const HRESULT hr = fence->SetEventOnCompletion(value, eventHandle);
        if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("queue/set-fence-event", hr, device_.Get()));
        WaitForSingleObject(eventHandle, INFINITE);
        return base::Result<void, runtime::RuntimeError>::Success();
    }
    void WaitForQueueAtDestruction(pkg::QueueId queue, std::uint64_t value, HANDLE eventHandle) noexcept
    {
        auto* fence = NativeFence(queue);
        if (!fence || value == 0 || !eventHandle || fence->GetCompletedValue() >= value) return;
        if (SUCCEEDED(fence->SetEventOnCompletion(value, eventHandle))) WaitForSingleObject(eventHandle, INFINITE);
    }
    [[nodiscard]] const pkg::ResourceViewArtifact* FindShaderResourceView(pkg::ResourceId resource) const noexcept
    {
        if (!resource.IsValid() || resource.value >= view_.Resources().size()) return nullptr;
        const auto& artifact = view_.Resources()[resource.value];
        const std::uint64_t end = static_cast<std::uint64_t>(artifact.firstView) + artifact.viewCount;
        if (end > view_.Views().size()) return nullptr;
        for (std::uint32_t index = artifact.firstView; index < end; ++index)
        {
            const auto& candidate = view_.Views()[index];
            if (candidate.resource == resource && candidate.viewClass == pkg::ViewClass::ShaderResource)
                return &candidate;
        }
        return nullptr;
    }

    base::Result<void, runtime::RuntimeError> PrepareInvocation(const runtime::FrameInvocation& invocation)
    {
        if (invocation.dynamicData.size() != view_.DynamicSlots().size())
            return base::Result<void, runtime::RuntimeError>::Failure(Error("invocation", "dynamic binding count does not match the package schema"));
        std::fill(dynamicBindings_.begin(), dynamicBindings_.end(), std::span<const std::byte>{});
        for (const auto& binding : invocation.dynamicData)
        {
            if (binding.slot == package::InvalidIndex || binding.slot >= view_.DynamicSlots().size())
                return base::Result<void, runtime::RuntimeError>::Failure(Error("invocation", "dynamic binding references an unknown slot"));
            if (!dynamicBindings_[binding.slot].empty())
                return base::Result<void, runtime::RuntimeError>::Failure(Error("invocation", "dynamic binding slot is duplicated"));
            const auto& contract = view_.DynamicSlots()[binding.slot];
            if (binding.bytes.size() != contract.requiredBytes)
                return base::Result<void, runtime::RuntimeError>::Failure(Error("invocation", "dynamic binding byte count does not exactly match the slot contract"));
            dynamicBindings_[binding.slot] = binding.bytes;
        }
        for (const auto bytes : dynamicBindings_)
            if (bytes.empty()) return base::Result<void, runtime::RuntimeError>::Failure(Error("invocation", "a required dynamic binding is missing"));

        if (invocation.externalResources.size() != view_.ExternalSlots().size())
            return base::Result<void, runtime::RuntimeError>::Failure(Error("invocation", "external binding count does not match the package schema"));
        std::fill(externalBindings_.begin(), externalBindings_.end(), runtime::ExternalResourceBinding{});
        for (const auto& binding : invocation.externalResources)
        {
            if (binding.slot == package::InvalidIndex || binding.slot >= view_.ExternalSlots().size())
                return base::Result<void, runtime::RuntimeError>::Failure(Error("invocation", "external binding references an unknown slot"));
            if (externalBindings_[binding.slot].resource)
                return base::Result<void, runtime::RuntimeError>::Failure(Error("invocation", "external binding slot is duplicated"));
            if (!binding.resource || !binding.availableAfter)
                return base::Result<void, runtime::RuntimeError>::Failure(Error("invocation", "external resource and completion token are required"));
            const auto& contract = view_.ExternalSlots()[binding.slot];
            if (binding.resource->DeviceEpoch() != deviceEpoch_ || binding.availableAfter->DeviceEpoch() != deviceEpoch_)
                return base::Result<void, runtime::RuntimeError>::Failure(Error("invocation", "external binding belongs to a different device epoch"));
            if (binding.resource->SizeBytes() < contract.minimumBytes)
                return base::Result<void, runtime::RuntimeError>::Failure(Error("invocation", "external resource is smaller than the slot contract"));
            if (!dynamic_cast<ExternalBufferResource*>(binding.resource.get()) || !dynamic_cast<CompletionToken*>(binding.availableAfter.get()))
                return base::Result<void, runtime::RuntimeError>::Failure(Error("invocation", "external binding was not created by this D3D12 executor"));
            externalBindings_[binding.slot] = binding;
        }
        for (const auto& binding : externalBindings_)
            if (!binding.resource) return base::Result<void, runtime::RuntimeError>::Failure(Error("invocation", "a required external binding is missing"));
        return base::Result<void, runtime::RuntimeError>::Success();
    }

    void ReleaseDeviceObjectsForRecovery() noexcept
    {
        externalNativeResources_.clear();
        externalBindings_.clear();
        externalAcquired_.clear();
        externalWaited_.clear();
        externalReleased_.clear();
        externalConsumerQueues_.clear();
        surfaceAcquired_.clear();
        surfacePresented_.clear();
        temporalWaitedResources_.clear();
        frameExternalReleases_.clear();

        commandList_.Reset();
        copyCommandList_.Reset();
        loadCommandList_.Reset();
        frameCommandLists_.clear();
        computeFrameCommandLists_.clear();
        copyFrameCommandLists_.clear();
        allocator_.Reset();
        copyAllocator_.Reset();
        loadAllocator_.Reset();
        frameAllocators_.clear();
        computeFrameAllocators_.clear();
        copyFrameAllocators_.clear();

        backBuffers_.clear();
        swapChain_.Reset();
        pendingBufferVerifications_.clear();
        pendingTextureVerifications_.clear();
        uploadResources_.clear();
        resources_.clear();
        placedHeaps_.clear();
        activeAliasResources_.clear();
        rootSignatures_.clear();
        pipelineStates_.clear();
        computePipelineStates_.clear();
        rtvHeap_.Reset();
        dsvHeap_.Reset();
        shaderHeap_.Reset();

        fence_.Reset();
        computeFence_.Reset();
        copyFence_.Reset();
        if (fenceEvent_) { CloseHandle(fenceEvent_); fenceEvent_ = nullptr; }
        if (computeFenceEvent_) { CloseHandle(computeFenceEvent_); computeFenceEvent_ = nullptr; }
        if (copyFenceEvent_) { CloseHandle(copyFenceEvent_); copyFenceEvent_ = nullptr; }

        queue_.Reset();
        computeQueue_.Reset();
        copyQueue_.Reset();
        device_.Reset();
        factory_.Reset();

        resourceStates_.clear();
        dynamicBindings_.clear();
        dynamicApplied_.clear();

        nextFenceValue_ = 1; lastFenceValue_ = 0;
        computeNextFenceValue_ = 1; computeLastFenceValue_ = 0;
        copyNextFenceValue_ = 1; copyLastFenceValue_ = 0;
        frameFenceValue_ = 0; computeFrameFenceValue_ = 0; copyFrameFenceValue_ = 0;
        frameSlotFenceValues_.clear();
        computeFrameSlotFenceValues_.clear();
        copyFrameSlotFenceValues_.clear();
        currentFrameSlot_ = 0;
        temporalPreviousInstance_ = 1; temporalCurrentInstance_ = 0;
        temporalDependencyFenceValue_ = 0;
        previousDirectFenceValue_ = 0; previousComputeFenceValue_ = 0; previousCopyFenceValue_ = 0;
        lastSubmittedFrameNumber_ = 0; hasSubmittedFrame_ = false;
        descriptorHeapsCreated_ = false;
        commandOpen_ = false; activeCommandQueue_ = package::InvalidIndex;
        directFrameSubmitted_ = false; computeFrameSubmitted_ = false;
        copyFrameCommandOpen_ = false; copyFrameSubmitted_ = false;
        directFrameBatchCursor_ = 0; computeFrameBatchCursor_ = 0; copyFrameBatchCursor_ = 0;
        loadBatchOpen_ = false; loadBatchClosed_ = false; loadQueueCompleted_ = false;
        rtvIncrement_ = 0; dsvIncrement_ = 0; shaderDescriptorIncrement_ = 0;
        currentBackBuffer_ = 0;
        hasActiveAdapterLuid_ = false;
        activeAdapterLuid_ = {};
    }

    base::Result<void, runtime::RuntimeError> CreateBaseObjects()
    {
        if (view_.Profile().minimumFeatureLevel != 0xb000 || view_.Profile().shaderModelMajor != 5 || view_.Profile().shaderModelMinor != 1 ||
            view_.Profile().rootSignatureMajor != 1 || view_.Profile().rootSignatureMinor != 0 ||
            view_.Profile().framesInFlight == 0 || view_.Profile().directQueueCount != 1 ||
            view_.Profile().computeQueueCount > 1 || view_.Profile().copyQueueCount > 1)
            return base::Result<void, runtime::RuntimeError>::Failure(Error("target-profile",
                "Executor supports one Direct queue and at most one Package-declared Compute and Copy queue"));

        std::uint32_t directFrameBatchCount = 0;
        std::uint32_t computeFrameBatchCount = 0;
        std::uint32_t copyFrameBatchCount = 0;
        for (const auto& operation : view_.FrameOperations())
        {
            if (operation.opcode != pkg::D3D12OperationCode::BeginQueueBatch) continue;
            if (IsDirectQueue(operation.queue)) ++directFrameBatchCount;
            else if (IsComputeQueue(operation.queue)) ++computeFrameBatchCount;
            else if (IsCopyQueue(operation.queue)) ++copyFrameBatchCount;
            else return base::Result<void, runtime::RuntimeError>::Failure(
                Error("target-profile", "frame stream references a queue that is not materialized by this Executor"));
        }
        const auto& diagnostics = ConfigureD3D12DiagnosticsOnce(options_.enableDebugLayer);

        UINT factoryFlags = 0;
#if defined(_DEBUG)
        if (options_.enableDebugLayer && diagnostics.debugLayerEnabled)
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#else
        (void)diagnostics;
#endif
        HRESULT hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory_));
        if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("device/create-factory", hr));

        ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 selectedDesc{};
        const auto isExcluded = [&](const DXGI_ADAPTER_DESC1& desc) noexcept
        {
            return hasExcludedAdapterLuid_ && SameLuid(desc.AdapterLuid, excludedAdapterLuid_);
        };
        const auto acceptCandidate = [&](ComPtr<IDXGIAdapter1> candidate) -> bool
        {
            if (!candidate) return false;
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(candidate->GetDesc1(&desc)) || isExcluded(desc)) return false;
            if (FAILED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr))) return false;
            adapter = std::move(candidate);
            selectedDesc = desc;
            return true;
        };

        if (options_.forceWarp)
        {
            ComPtr<IDXGIAdapter1> warp;
            hr = factory_->EnumWarpAdapter(IID_PPV_ARGS(&warp));
            if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("device/warp-adapter", hr));
            acceptCandidate(std::move(warp));
        }
        else
        {
            for (UINT index = 0; ; ++index)
            {
                ComPtr<IDXGIAdapter1> candidate;
                hr = factory_->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate));
                if (hr == DXGI_ERROR_NOT_FOUND) break;
                if (FAILED(hr)) continue;
                DXGI_ADAPTER_DESC1 desc{};
                if (FAILED(candidate->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
                if (acceptCandidate(std::move(candidate))) break;
            }
            if (!adapter)
            {
                ComPtr<IDXGIAdapter1> warp;
                hr = factory_->EnumWarpAdapter(IID_PPV_ARGS(&warp));
                if (SUCCEEDED(hr)) acceptCandidate(std::move(warp));
            }
        }
        if (!adapter)
            return base::Result<void, runtime::RuntimeError>::Failure(
                Error("device/no-eligible-adapter", "no D3D12 adapter is available after excluding the removed adapter LUID"));

        hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
        if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("device/create-device", hr));
        activeAdapterLuid_ = selectedDesc.AdapterLuid;
        hasActiveAdapterLuid_ = true;

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue_));
        if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("device/create-queue", hr, device_.Get()));
        if (HasComputeQueue())
        {
            D3D12_COMMAND_QUEUE_DESC computeDesc{};
            computeDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
            hr = device_->CreateCommandQueue(&computeDesc, IID_PPV_ARGS(&computeQueue_));
            if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("device/create-compute-queue", hr, device_.Get()));
        }
        if (HasCopyQueue())
        {
            D3D12_COMMAND_QUEUE_DESC copyDesc{};
            copyDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
            hr = device_->CreateCommandQueue(&copyDesc, IID_PPV_ARGS(&copyQueue_));
            if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("device/create-copy-queue", hr, device_.Get()));
        }

        const auto width = std::max(1u, surface_->ClientWidth());
        const auto height = std::max(1u, surface_->ClientHeight());
        DXGI_SWAP_CHAIN_DESC1 swapDesc{};
        swapDesc.Width = width;
        swapDesc.Height = height;
        swapDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        swapDesc.SampleDesc.Count = 1;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.BufferCount = view_.Profile().surfaceImageCount;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> swapChain1;
        hr = factory_->CreateSwapChainForHwnd(queue_.Get(), static_cast<HWND>(surface_->NativeWindowHandle()), &swapDesc, nullptr, nullptr, &swapChain1);
        if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("surface/create-swap-chain", hr, device_.Get()));
        factory_->MakeWindowAssociation(static_cast<HWND>(surface_->NativeWindowHandle()), DXGI_MWA_NO_ALT_ENTER);
        hr = swapChain1.As(&swapChain_);
        if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("surface/query-swap-chain3", hr, device_.Get()));

        const auto loadQueue = LoadQueueId();
        const auto loadType = NativeCommandListType(loadQueue);
        hr = device_->CreateCommandAllocator(loadType, IID_PPV_ARGS(&loadAllocator_));
        if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("device/create-load-command-allocator", hr, device_.Get()));
        hr = device_->CreateCommandList(0, loadType, loadAllocator_.Get(), nullptr, IID_PPV_ARGS(&loadCommandList_));
        if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("device/create-load-command-list", hr, device_.Get()));
        hr = loadCommandList_->Close();
        if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("device/close-load-command-list", hr, device_.Get()));

        frameAllocators_.resize(view_.Profile().framesInFlight);
        frameCommandLists_.resize(view_.Profile().framesInFlight);
        computeFrameAllocators_.resize(view_.Profile().framesInFlight);
        computeFrameCommandLists_.resize(view_.Profile().framesInFlight);
        copyFrameAllocators_.resize(view_.Profile().framesInFlight);
        copyFrameCommandLists_.resize(view_.Profile().framesInFlight);
        const auto createBatches = [&](std::uint32_t slot, D3D12_COMMAND_LIST_TYPE type, std::uint32_t count,
            auto& allocators, auto& lists, const char* allocatorStage, const char* listStage, const char* closeStage)
            -> base::Result<void, runtime::RuntimeError>
        {
            allocators[slot].resize(count);
            lists[slot].resize(count);
            for (std::uint32_t batch = 0; batch < count; ++batch)
            {
                HRESULT local = device_->CreateCommandAllocator(type, IID_PPV_ARGS(&allocators[slot][batch]));
                if (FAILED(local)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError(allocatorStage, local, device_.Get()));
                local = device_->CreateCommandList(0, type, allocators[slot][batch].Get(), nullptr, IID_PPV_ARGS(&lists[slot][batch]));
                if (FAILED(local)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError(listStage, local, device_.Get()));
                local = lists[slot][batch]->Close();
                if (FAILED(local)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError(closeStage, local, device_.Get()));
            }
            return base::Result<void, runtime::RuntimeError>::Success();
        };
        for (std::uint32_t slot = 0; slot < view_.Profile().framesInFlight; ++slot)
        {
            auto created = createBatches(slot, D3D12_COMMAND_LIST_TYPE_DIRECT, directFrameBatchCount,
                frameAllocators_, frameCommandLists_, "device/create-frame-command-allocator", "device/create-frame-command-list", "device/close-frame-command-list");
            if (!created) return created;
            created = createBatches(slot, D3D12_COMMAND_LIST_TYPE_COMPUTE, computeFrameBatchCount,
                computeFrameAllocators_, computeFrameCommandLists_, "device/create-compute-frame-command-allocator", "device/create-compute-frame-command-list", "device/close-compute-frame-command-list");
            if (!created) return created;
            created = createBatches(slot, D3D12_COMMAND_LIST_TYPE_COPY, copyFrameBatchCount,
                copyFrameAllocators_, copyFrameCommandLists_, "device/create-copy-frame-command-allocator", "device/create-copy-frame-command-list", "device/close-copy-frame-command-list");
            if (!created) return created;
        }

        hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
        if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("device/create-fence", hr, device_.Get()));
        fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent_) return base::Result<void, runtime::RuntimeError>::Failure(Error("device/create-fence-event", "CreateEventW failed"));
        if (HasComputeQueue())
        {
            hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&computeFence_));
            if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("device/create-compute-fence", hr, device_.Get()));
            computeFenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!computeFenceEvent_) return base::Result<void, runtime::RuntimeError>::Failure(Error("device/create-compute-fence-event", "CreateEventW failed"));
        }
        if (HasCopyQueue())
        {
            hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copyFence_));
            if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("device/create-copy-fence", hr, device_.Get()));
            copyFenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!copyFenceEvent_) return base::Result<void, runtime::RuntimeError>::Failure(Error("device/create-copy-fence-event", "CreateEventW failed"));
        }
        return base::Result<void, runtime::RuntimeError>::Success();
    }

    base::Result<void, runtime::RuntimeError> ExecuteLoadOperation(const pkg::OperationView& operation)
    {
        switch (operation.opcode)
        {
        case pkg::D3D12OperationCode::CreateDescriptorHeaps:
            if (!operation.payload.empty()) return base::Result<void, runtime::RuntimeError>::Failure(Error("load/CreateDescriptorHeaps", "payload must be empty"));
            return CreateDescriptorHeaps();
        case pkg::D3D12OperationCode::CreateResource:
        {
            auto payload = pkg::DecodeCreateResource(operation.payload);
            if (!payload) return PackageFailure("load/CreateResource", payload.Error());
            return CreateResource(payload.Value().resource);
        }
        case pkg::D3D12OperationCode::BeginQueueBatch:
        {
            if (operation.queue != LoadQueueId() || !operation.payload.empty() || loadBatchOpen_ || loadBatchClosed_)
                return base::Result<void, runtime::RuntimeError>::Failure(Error("load/BeginQueueBatch", "Package-declared load queue batch contract is invalid"));
            HRESULT hr = loadAllocator_->Reset();
            if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("load/reset-allocator", hr, device_.Get()));
            hr = loadCommandList_->Reset(loadAllocator_.Get(), nullptr);
            if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("load/reset-command-list", hr, device_.Get()));
            loadBatchOpen_ = true;
            return base::Result<void, runtime::RuntimeError>::Success();
        }
        case pkg::D3D12OperationCode::UploadBuffer:
        {
            auto payload = pkg::DecodeUploadBuffer(operation.payload);
            if (!payload) return PackageFailure("load/UploadBuffer", payload.Error());
            return UploadBuffer(payload.Value());
        }
        case pkg::D3D12OperationCode::UploadTexture:
        {
            auto payload = pkg::DecodeUploadTexture(operation.payload);
            if (!payload) return PackageFailure("load/UploadTexture", payload.Error());
            return UploadTexture(payload.Value());
        }
        case pkg::D3D12OperationCode::InitializeState:
        {
            auto payload = pkg::DecodeInitializeState(operation.payload);
            if (!payload) return PackageFailure("load/InitializeState", payload.Error());
            if (!loadBatchOpen_ || operation.queue != LoadQueueId())
                return base::Result<void, runtime::RuntimeError>::Failure(Error("load/InitializeState", "Package-declared load queue batch is not open"));
            if (IsCopyQueue(operation.queue) && (!IsCopyQueueState(payload.Value().before) || !IsCopyQueueState(payload.Value().after)))
                return base::Result<void, runtime::RuntimeError>::Failure(Error("load/InitializeState", "Copy queue transition uses a state unsupported by D3D12 Copy queues"));
            const auto& artifact = view_.Resources()[payload.Value().resource.value];
            if (IsTemporal(artifact))
            {
                for (std::uint32_t instanceIndex = 0; instanceIndex < artifact.physicalInstanceCount; ++instanceIndex)
                {
                    auto transitioned = TransitionResourceAt(payload.Value().resource, instanceIndex,
                        payload.Value().before, payload.Value().after, loadCommandList_.Get(),
                        NativeCommandListType(operation.queue));
                    if (!transitioned) return transitioned;
                }
                return base::Result<void, runtime::RuntimeError>::Success();
            }
            return TransitionResource(payload.Value().resource, payload.Value().before, payload.Value().after,
                loadCommandList_.Get(), NativeCommandListType(operation.queue));
        }
        case pkg::D3D12OperationCode::ActivateAlias:
        {
            auto payload = pkg::DecodeActivateAlias(operation.payload);
            if (!payload) return PackageFailure("load/ActivateAlias", payload.Error());
            if (!loadBatchOpen_ || operation.queue != LoadQueueId())
                return base::Result<void, runtime::RuntimeError>::Failure(Error("load/ActivateAlias", "Package-declared load queue batch is not open"));
            return ActivateAlias(payload.Value(), loadCommandList_.Get());
        }
        case pkg::D3D12OperationCode::VerifyBufferContents:
        {
            auto payload = pkg::DecodeVerifyBufferContents(operation.payload);
            if (!payload) return PackageFailure("load/VerifyBufferContents", payload.Error());
            return ScheduleBufferVerification(payload.Value());
        }
        case pkg::D3D12OperationCode::VerifyTextureContents:
        {
            auto payload = pkg::DecodeVerifyTextureContents(operation.payload);
            if (!payload) return PackageFailure("load/VerifyTextureContents", payload.Error());
            return ScheduleTextureVerification(payload.Value());
        }
        case pkg::D3D12OperationCode::ExecuteCopy:
        {
            auto payload = pkg::DecodeCopyBuffer(operation.payload);
            if (!payload) return PackageFailure("load/ExecuteCopy", payload.Error());
            return ExecuteCopy(payload.Value());
        }
        case pkg::D3D12OperationCode::EndQueueBatch:
        {
            if (!loadBatchOpen_ || operation.queue != LoadQueueId() || !operation.payload.empty())
                return base::Result<void, runtime::RuntimeError>::Failure(Error("load/EndQueueBatch", "Package-declared load queue batch is not open"));
            const HRESULT hr = loadCommandList_->Close();
            if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("load/close-command-list", hr, device_.Get()));
            loadBatchOpen_ = false;
            loadBatchClosed_ = true;
            return base::Result<void, runtime::RuntimeError>::Success();
        }
        case pkg::D3D12OperationCode::SignalQueue:
        {
            const auto loadQueue = LoadQueueId();
            if (!loadBatchClosed_ || operation.queue != loadQueue || !operation.payload.empty() || loadQueueCompleted_)
                return base::Result<void, runtime::RuntimeError>::Failure(Error("load/SignalQueue", "Package-declared load queue batch has not been closed"));
            ID3D12CommandList* lists[] = {loadCommandList_.Get()};
            NativeQueue(loadQueue)->ExecuteCommandLists(1, lists);
            const auto value = NextFenceValue(loadQueue);
            HRESULT hr = NativeQueue(loadQueue)->Signal(NativeFence(loadQueue), value);
            if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("load/signal-queue", hr, device_.Get()));
            SetFrameFenceValue(loadQueue, value);
            auto waited = WaitForQueueFence(loadQueue, value);
            if (!waited) return waited;
            frameFenceValue_ = 0;
            computeFrameFenceValue_ = 0;
            copyFrameFenceValue_ = 0;
            loadQueueCompleted_ = true;
            return base::Result<void, runtime::RuntimeError>::Success();
        }
        case pkg::D3D12OperationCode::CreateRootSignature:
        {
            auto payload = pkg::DecodeCreateRootSignature(operation.payload);
            if (!payload) return PackageFailure("load/CreateRootSignature", payload.Error());
            return CreateRootSignature(payload.Value().layout);
        }
        case pkg::D3D12OperationCode::CreateGraphicsPipeline:
        {
            auto payload = pkg::DecodeCreateGraphicsPipeline(operation.payload);
            if (!payload) return PackageFailure("load/CreateGraphicsPipeline", payload.Error());
            return CreateGraphicsPipeline(payload.Value().executable);
        }
        case pkg::D3D12OperationCode::CreateComputePipeline:
        {
            auto payload = pkg::DecodeCreateComputePipeline(operation.payload);
            if (!payload) return PackageFailure("load/CreateComputePipeline", payload.Error());
            return CreateComputePipeline(payload.Value().executable);
        }
        default:
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load", "operation is unsupported in the load stream"));
        }
    }

    base::Result<void, runtime::RuntimeError> ExecuteFrameOperation(const pkg::OperationView& operation)
    {
        switch (operation.opcode)
        {
        case pkg::D3D12OperationCode::ApplyDynamicData:
        {
            auto payload = pkg::DecodeApplyDynamicData(operation.payload);
            if (!payload) return PackageFailure("frame/ApplyDynamicData", payload.Error());
            return ApplyDynamicData(payload.Value().slot);
        }
        case pkg::D3D12OperationCode::AcquireExternal:
        {
            auto payload = pkg::DecodeAcquireExternal(operation.payload);
            if (!payload) return PackageFailure("frame/AcquireExternal", payload.Error());
            const auto slot = payload.Value().slot.value;
            if (!payload.Value().slot.IsValid() || slot >= view_.ExternalSlots().size() || externalAcquired_[slot])
                return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/AcquireExternal", "external slot is invalid or already acquired"));
            const auto& contract = view_.ExternalSlots()[slot];
            auto* native = dynamic_cast<ExternalBufferResource*>(externalBindings_[slot].resource.get());
            if (!native || !contract.resource.IsValid() || contract.resource.value >= externalNativeResources_.size())
                return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/AcquireExternal", "external resource implementation or package resource is invalid"));
            if (!(TrackedState(contract.resource) == contract.requiredIncomingState))
                return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/AcquireExternal", "external resource incoming state does not match the package contract"));
            const auto* externalView = FindShaderResourceView(contract.resource);
            if (!externalView || externalView->strideBytes == 0 || externalView->byteSize == 0 ||
                externalView->byteOffset % externalView->strideBytes != 0 || externalView->byteSize % externalView->strideBytes != 0)
                return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/AcquireExternal", "Package does not provide a valid ShaderResource view for the external buffer"));
            externalNativeResources_[contract.resource.value] = externalBindings_[slot].resource;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = ToDxgi(externalView->format);
            srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Buffer.FirstElement = externalView->byteOffset / externalView->strideBytes;
            srv.Buffer.NumElements = static_cast<UINT>(externalView->byteSize / externalView->strideBytes);
            srv.Buffer.StructureByteStride = externalView->strideBytes;
            auto cpu = shaderHeap_->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += static_cast<SIZE_T>(DescriptorIndex(*externalView)) * shaderDescriptorIncrement_;
            device_->CreateShaderResourceView(native->Native(), &srv, cpu);
            externalAcquired_[slot] = true;
            return base::Result<void, runtime::RuntimeError>::Success();
        }
        case pkg::D3D12OperationCode::WaitExternal:
        {
            auto payload = pkg::DecodeWaitExternal(operation.payload);
            if (!payload) return PackageFailure("frame/WaitExternal", payload.Error());
            const auto slot = payload.Value().slot.value;
            if (!IsSupportedQueue(operation.queue) || !payload.Value().slot.IsValid() || slot >= view_.ExternalSlots().size() ||
                !externalAcquired_[slot] || externalWaited_[slot])
                return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/WaitExternal", "external wait order, slot, or Package queue is invalid"));
            auto* token = dynamic_cast<CompletionToken*>(externalBindings_[slot].availableAfter.get());
            if (!token)
                return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/WaitExternal", "external completion token implementation is invalid"));
            const HRESULT hr = NativeQueue(operation.queue)->Wait(token->NativeFence(), token->Value());
            if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("frame/wait-external", hr, device_.Get()));
            externalWaited_[slot] = true;
            externalConsumerQueues_[slot] = operation.queue.value;
            return base::Result<void, runtime::RuntimeError>::Success();
        }
        case pkg::D3D12OperationCode::AcquireSurfaceImage:
        {
            auto payload = pkg::DecodeAcquireSurfaceImage(operation.payload);
            if (!payload) return PackageFailure("frame/AcquireSurfaceImage", payload.Error());
            const auto slot = payload.Value().slot.value;
            if (!payload.Value().slot.IsValid() || slot >= view_.SurfaceSlots().size() || surfaceAcquired_[slot])
                return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/AcquireSurfaceImage", "surface slot is invalid or already acquired"));
            currentBackBuffer_ = swapChain_->GetCurrentBackBufferIndex();
            surfaceAcquired_[slot] = true;
            return base::Result<void, runtime::RuntimeError>::Success();
        }
        case pkg::D3D12OperationCode::BeginQueueBatch:
        {
            if (!operation.payload.empty() || !IsSupportedQueue(operation.queue))
                return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/BeginQueueBatch", "payload or Package queue is invalid"));
            if (IsCopyQueue(operation.queue))
            {
                if (copyFrameCommandOpen_ || copyFrameBatchCursor_ >= copyFrameAllocators_[currentFrameSlot_].size())
                    return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/BeginQueueBatch", "Copy queue batch state exceeds the Package-declared frame stream"));
                copyAllocator_ = copyFrameAllocators_[currentFrameSlot_][copyFrameBatchCursor_];
                copyCommandList_ = copyFrameCommandLists_[currentFrameSlot_][copyFrameBatchCursor_++];
                HRESULT hr = copyAllocator_->Reset();
                if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("frame/reset-copy-allocator", hr, device_.Get()));
                hr = copyCommandList_->Reset(copyAllocator_.Get(), nullptr);
                if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("frame/reset-copy-command-list", hr, device_.Get()));
                copyFrameCommandOpen_ = true;
                return base::Result<void, runtime::RuntimeError>::Success();
            }
            if (commandOpen_)
                return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/BeginQueueBatch", "another Direct/Compute queue batch is already open"));
            if (IsDirectQueue(operation.queue))
            {
                if (directFrameBatchCursor_ >= frameAllocators_[currentFrameSlot_].size())
                    return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/BeginQueueBatch", "Direct queue batch state exceeds the Package-declared frame stream"));
                allocator_ = frameAllocators_[currentFrameSlot_][directFrameBatchCursor_];
                commandList_ = frameCommandLists_[currentFrameSlot_][directFrameBatchCursor_++];
            }
            else
            {
                if (computeFrameBatchCursor_ >= computeFrameAllocators_[currentFrameSlot_].size())
                    return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/BeginQueueBatch", "Compute queue batch state exceeds the Package-declared frame stream"));
                allocator_ = computeFrameAllocators_[currentFrameSlot_][computeFrameBatchCursor_];
                commandList_ = computeFrameCommandLists_[currentFrameSlot_][computeFrameBatchCursor_++];
            }
            HRESULT hr = allocator_->Reset();
            if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("frame/reset-allocator", hr, device_.Get()));
            hr = commandList_->Reset(allocator_.Get(), nullptr);
            if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("frame/reset-command-list", hr, device_.Get()));
            commandOpen_ = true;
            activeCommandQueue_ = operation.queue.value;
            return base::Result<void, runtime::RuntimeError>::Success();
        }
        case pkg::D3D12OperationCode::Transition:
        {
            auto payload = pkg::DecodeTransition(operation.payload);
            if (!payload) return PackageFailure("frame/Transition", payload.Error());
            if (!payload.Value().view.IsValid() || payload.Value().view.value >= view_.Views().size())
                return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/Transition", "view is invalid"));
            const auto& resourceView = view_.Views()[payload.Value().view.value];
            if (IsCopyQueue(operation.queue) && copyFrameCommandOpen_)
            {
                if (!IsCopyQueueState(payload.Value().before) || !IsCopyQueueState(payload.Value().after))
                    return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/Transition", "Copy queue transition uses a state unsupported by D3D12 Copy queues"));
                return TransitionResource(resourceView, payload.Value().before, payload.Value().after,
                    copyCommandList_.Get(), NativeCommandListType(operation.queue));
            }
            if (commandOpen_ && activeCommandQueue_ == operation.queue.value && (IsDirectQueue(operation.queue) || IsComputeQueue(operation.queue)))
                return TransitionResource(resourceView, payload.Value().before, payload.Value().after,
                    commandList_.Get(), NativeCommandListType(operation.queue));
            return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/Transition", "operation is outside its Package-declared queue batch"));
        }
        case pkg::D3D12OperationCode::ExecuteCopy:
        {
            const bool copyBatch = IsCopyQueue(operation.queue) && copyFrameCommandOpen_;
            const bool directBatch = IsDirectQueue(operation.queue) && commandOpen_ && activeCommandQueue_ == operation.queue.value;
            if (!copyBatch && !directBatch)
                return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteCopy", "Package-declared Copy-capable queue batch is not open"));
            auto payload = pkg::DecodeCopyBuffer(operation.payload);
            if (!payload) return PackageFailure("frame/ExecuteCopy", payload.Error());
            return ExecuteCopy(payload.Value(), "frame/ExecuteCopy");
        }
        case pkg::D3D12OperationCode::ExecuteCompute:
        {
            if ((!IsDirectQueue(operation.queue) && !IsComputeQueue(operation.queue)) || !commandOpen_ || activeCommandQueue_ != operation.queue.value)
                return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteCompute", "Package-declared Direct/Compute queue batch is not open"));
            auto payload = pkg::DecodeExecuteCompute(operation.payload);
            if (!payload) return PackageFailure("frame/ExecuteCompute", payload.Error());
            return ExecuteCompute(payload.Value().command);
        }
        case pkg::D3D12OperationCode::ExecuteRaster:
        {
            if (!IsDirectQueue(operation.queue) || !commandOpen_ || activeCommandQueue_ != operation.queue.value)
                return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteRaster", "Package-declared Direct queue batch is not open"));
            auto payload = pkg::DecodeExecuteRaster(operation.payload);
            if (!payload) return PackageFailure("frame/ExecuteRaster", payload.Error());
            return ExecuteRaster(payload.Value().command);
        }
        case pkg::D3D12OperationCode::EndQueueBatch:
        {
            if (!operation.payload.empty() || !IsSupportedQueue(operation.queue))
                return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/EndQueueBatch", "payload or Package queue is invalid"));
            if (IsCopyQueue(operation.queue))
            {
                if (!copyFrameCommandOpen_)
                    return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/EndQueueBatch", "Package-declared Copy queue batch is not open"));
                const HRESULT hr = copyCommandList_->Close();
                if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("frame/close-copy-command-list", hr, device_.Get()));
                copyFrameCommandOpen_ = false;
                ID3D12CommandList* lists[] = {copyCommandList_.Get()};
                copyQueue_->ExecuteCommandLists(1, lists);
                copyFrameSubmitted_ = true;
                return base::Result<void, runtime::RuntimeError>::Success();
            }
            if (!commandOpen_ || activeCommandQueue_ != operation.queue.value)
                return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/EndQueueBatch", "Package-declared Direct/Compute queue batch is not open"));
            const HRESULT hr = commandList_->Close();
            if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("frame/close-command-list", hr, device_.Get()));
            commandOpen_ = false;
            activeCommandQueue_ = package::InvalidIndex;
            ID3D12CommandList* lists[] = {commandList_.Get()};
            NativeQueue(operation.queue)->ExecuteCommandLists(1, lists);
            SetFrameQueueSubmitted(operation.queue, true);
            return base::Result<void, runtime::RuntimeError>::Success();
        }
        case pkg::D3D12OperationCode::SignalQueue:
        {
            if (!operation.payload.empty() || !IsSupportedQueue(operation.queue))
                return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/SignalQueue", "payload or Package queue is invalid"));
            if ((IsCopyQueue(operation.queue) && copyFrameCommandOpen_) ||
                ((!IsCopyQueue(operation.queue)) && commandOpen_ && activeCommandQueue_ == operation.queue.value) ||
                !FrameQueueSubmitted(operation.queue))
                return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/SignalQueue", "Package queue has no closed submitted batch to order"));
            const auto value = NextFenceValue(operation.queue);
            const HRESULT hr = NativeQueue(operation.queue)->Signal(NativeFence(operation.queue), value);
            if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("frame/signal-queue", hr, device_.Get()));
            SetFrameFenceValue(operation.queue, value);
            SetFrameQueueSubmitted(operation.queue, false);
            return base::Result<void, runtime::RuntimeError>::Success();
        }
        case pkg::D3D12OperationCode::WaitQueue:
        {
            auto payload = pkg::DecodeWaitQueue(operation.payload);
            if (!payload) return PackageFailure("frame/WaitQueue", payload.Error());
            const auto producer = payload.Value().producerQueue;
            if (!IsSupportedQueue(operation.queue) || !IsSupportedQueue(producer) || operation.queue == producer)
                return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/WaitQueue", "consumer or producer Package queue is invalid"));
            const auto value = FrameFenceValue(producer);
            if (value == 0)
                return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/WaitQueue", "producer queue has not executed a preceding Package SignalQueue"));
            const HRESULT hr = NativeQueue(operation.queue)->Wait(NativeFence(producer), value);
            if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("frame/wait-queue", hr, device_.Get()));
            return base::Result<void, runtime::RuntimeError>::Success();
        }
        case pkg::D3D12OperationCode::WaitTemporal:
        {
            auto payload = pkg::DecodeWaitTemporal(operation.payload);
            if (!payload) return PackageFailure("frame/WaitTemporal", payload.Error());
            const auto resource = payload.Value().resource;
            if (!IsSupportedQueue(operation.queue) || !resource.IsValid() || resource.value >= view_.Resources().size() ||
                !IsTemporal(view_.Resources()[resource.value]) || temporalWaitedResources_[resource.value])
                return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/WaitTemporal", "Package queue or Temporal resource contract is invalid"));
            const auto dependency = PreviousFrameFenceValue(operation.queue);
            if (dependency != 0)
            {
                const HRESULT hr = NativeQueue(operation.queue)->Wait(NativeFence(operation.queue), dependency);
                if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("frame/wait-temporal", hr, device_.Get()));
            }
            temporalWaitedResources_[resource.value] = true;
            temporalDependencyFenceValue_ = dependency;
            return base::Result<void, runtime::RuntimeError>::Success();
        }
        case pkg::D3D12OperationCode::ReleaseExternal:
        {
            auto payload = pkg::DecodeReleaseExternal(operation.payload);
            if (!payload) return PackageFailure("frame/ReleaseExternal", payload.Error());
            const auto slot = payload.Value().slot.value;
            if (!payload.Value().slot.IsValid() || slot >= view_.ExternalSlots().size() || !externalWaited_[slot] || externalReleased_[slot])
                return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ReleaseExternal", "external release order or slot is invalid"));
            const auto queueId = pkg::QueueId{externalConsumerQueues_[slot]};
            const auto fenceValue = FrameFenceValue(queueId);
            if (!IsSupportedQueue(queueId) || fenceValue == 0)
                return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ReleaseExternal", "external consumer queue has not been signaled"));
            const auto& contract = view_.ExternalSlots()[slot];
            if (!(TrackedState(contract.resource) == contract.guaranteedOutgoingState))
                return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ReleaseExternal", "external outgoing state does not match the package contract"));
            frameExternalReleases_.push_back({slot, std::make_shared<CompletionToken>(FenceReference(queueId), fenceValue, deviceEpoch_)});
            externalReleased_[slot] = true;
            return base::Result<void, runtime::RuntimeError>::Success();
        }
        case pkg::D3D12OperationCode::PresentSurface:
        {
            auto payload = pkg::DecodePresentSurface(operation.payload);
            if (!payload) return PackageFailure("frame/PresentSurface", payload.Error());
            const auto slot = payload.Value().slot.value;
            if (!payload.Value().slot.IsValid() || slot >= view_.SurfaceSlots().size() ||
                !surfaceAcquired_[slot] || surfacePresented_[slot])
                return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/PresentSurface", "surface slot was not acquired or was already presented"));
            const auto& contract = view_.SurfaceSlots()[slot];
            if (!(TrackedState(contract.imageResource) == contract.presentedState))
                return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/PresentSurface", "surface image state does not match the Package presented-state contract"));
            const HRESULT hr = swapChain_->Present(1, 0);
            if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("frame/present", hr, device_.Get()));
            surfacePresented_[slot] = true;
            // DXGI Present is ordered on the Direct queue.  The following Package
            // SignalQueue must therefore be allowed to fence presentation work even
            // though no new command-list batch was closed after the preceding signal.
            SetFrameQueueSubmitted(DirectQueueId(), true);
            return base::Result<void, runtime::RuntimeError>::Success();
        }
        default:
            return base::Result<void, runtime::RuntimeError>::Failure(Error("frame", "operation is unsupported in the frame stream"));
        }
    }

    base::Result<void, runtime::RuntimeError> CreateDescriptorHeaps()
    {
        if (descriptorHeapsCreated_) return base::Result<void, runtime::RuntimeError>::Failure(Error("load/CreateDescriptorHeaps", "descriptor heaps were already created"));
        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDesc.NumDescriptors = std::max(1u, view_.Profile().rtvDescriptorCount);
        rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        HRESULT hr = device_->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap_));
        if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("load/create-rtv-heap", hr, device_.Get()));
        rtvIncrement_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
        dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvDesc.NumDescriptors = std::max(1u, view_.Profile().dsvDescriptorCount);
        dsvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        hr = device_->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&dsvHeap_));
        if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("load/create-dsv-heap", hr, device_.Get()));
        dsvIncrement_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

        D3D12_DESCRIPTOR_HEAP_DESC shaderDesc{};
        shaderDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        shaderDesc.NumDescriptors = std::max(1u, view_.Profile().shaderDescriptorCount);
        shaderDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device_->CreateDescriptorHeap(&shaderDesc, IID_PPV_ARGS(&shaderHeap_));
        if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("load/create-shader-descriptor-heap", hr, device_.Get()));
        shaderDescriptorIncrement_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        backBuffers_.resize(view_.Profile().surfaceImageCount);
        auto handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < backBuffers_.size(); ++i)
        {
            hr = swapChain_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i]));
            if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("load/get-surface-image", hr, device_.Get()));
            device_->CreateRenderTargetView(backBuffers_[i].Get(), nullptr, handle);
            handle.ptr += rtvIncrement_;
        }
        descriptorHeapsCreated_ = true;
        return base::Result<void, runtime::RuntimeError>::Success();
    }

    [[nodiscard]] bool IsFrameLocal(const pkg::ResourceArtifact& artifact) const noexcept
    {
        return (artifact.flags & static_cast<std::uint32_t>(pkg::ResourceFlags::FrameLocal)) != 0;
    }

    [[nodiscard]] bool IsTemporal(const pkg::ResourceArtifact& artifact) const noexcept
    {
        return (artifact.flags & static_cast<std::uint32_t>(pkg::ResourceFlags::Temporal)) != 0;
    }

    [[nodiscard]] std::uint32_t PhysicalInstanceIndex(pkg::ResourceId id) const noexcept
    {
        const auto& artifact = view_.Resources()[id.value];
        if (artifact.origin == pkg::ResourceOrigin::Surface) return currentBackBuffer_;
        if (IsFrameLocal(artifact) || IsTemporal(artifact)) return temporalCurrentInstance_;
        return 0u;
    }

    [[nodiscard]] std::uint32_t PhysicalInstanceIndex(const pkg::ResourceViewArtifact& resourceView) const noexcept
    {
        const auto& artifact = view_.Resources()[resourceView.resource.value];
        if (artifact.origin == pkg::ResourceOrigin::Surface) return currentBackBuffer_;
        if ((resourceView.flags & static_cast<std::uint32_t>(pkg::ResourceViewFlags::TemporalPrevious)) != 0)
            return temporalPreviousInstance_;
        if ((resourceView.flags & static_cast<std::uint32_t>(pkg::ResourceViewFlags::TemporalCurrent)) != 0)
            return temporalCurrentInstance_;
        if (IsFrameLocal(artifact) || IsTemporal(artifact)) return currentFrameSlot_;
        return 0u;
    }

    [[nodiscard]] std::uint32_t DescriptorIndex(const pkg::ResourceViewArtifact& resourceView) const noexcept
    {
        const auto& artifact = view_.Resources()[resourceView.resource.value];
        const std::uint32_t instance = artifact.origin == pkg::ResourceOrigin::External
            ? currentFrameSlot_
            : PhysicalInstanceIndex(resourceView);
        return resourceView.descriptorIndex + resourceView.descriptorInstanceStride * instance;
    }

    pkg::ResourceState& TrackedState(pkg::ResourceId id)
    {
        return resourceStates_[id.value][PhysicalInstanceIndex(id)];
    }

    const pkg::ResourceState& TrackedState(pkg::ResourceId id) const
    {
        return resourceStates_[id.value][PhysicalInstanceIndex(id)];
    }

    pkg::ResourceState& TrackedState(const pkg::ResourceViewArtifact& resourceView)
    {
        return resourceStates_[resourceView.resource.value][PhysicalInstanceIndex(resourceView)];
    }

    const pkg::ResourceState& TrackedState(const pkg::ResourceViewArtifact& resourceView) const
    {
        return resourceStates_[resourceView.resource.value][PhysicalInstanceIndex(resourceView)];
    }

    ID3D12Resource* ResourceObjectAt(pkg::ResourceId id, std::uint32_t instanceIndex)
    {
        if (!id.IsValid() || id.value >= view_.Resources().size()) return nullptr;
        const auto& artifact = view_.Resources()[id.value];
        if (artifact.origin == pkg::ResourceOrigin::Surface)
            return instanceIndex < backBuffers_.size() ? backBuffers_[instanceIndex].Get() : nullptr;
        if (artifact.origin == pkg::ResourceOrigin::External)
        {
            if (id.value >= externalNativeResources_.size()) return nullptr;
            auto* native = dynamic_cast<ExternalBufferResource*>(externalNativeResources_[id.value].get());
            return native ? native->Native() : nullptr;
        }
        return instanceIndex < resources_[id.value].size() ? resources_[id.value][instanceIndex].Get() : nullptr;
    }

    ID3D12Resource* ResourceObject(const pkg::ResourceViewArtifact& resourceView)
    {
        return ResourceObjectAt(resourceView.resource, PhysicalInstanceIndex(resourceView));
    }

    base::Result<void, runtime::RuntimeError> CreateResource(pkg::ResourceId id)
    {
        if (!id.IsValid() || id.value >= view_.Resources().size())
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/CreateResource", "resource ID is invalid"));
        const auto& artifact = view_.Resources()[id.value];
        if (artifact.origin != pkg::ResourceOrigin::PackageOwned)
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/CreateResource", "resource is not package-owned"));
        if (!artifact.allocation.IsValid() || artifact.allocation.value >= view_.Allocations().size())
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/CreateResource", "resource allocation reference is invalid"));
        if (resources_[id.value].size() != artifact.physicalInstanceCount ||
            std::any_of(resources_[id.value].begin(), resources_[id.value].end(), [](const auto& value) { return value.Get() != nullptr; }))
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/CreateResource", "resource instance storage is invalid or was already created"));

        const auto& allocation = view_.Allocations()[artifact.allocation.value];
        if ((allocation.kind != pkg::AllocationKind::Committed && allocation.kind != pkg::AllocationKind::Placed) ||
            allocation.physicalInstanceCount != artifact.physicalInstanceCount ||
            (artifact.physicalInstanceCount != 1 && artifact.physicalInstanceCount != view_.Profile().framesInFlight))
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/CreateResource", "allocation artifact is outside the fixed Slice-13 contract"));

        D3D12_HEAP_PROPERTIES heap{};
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc{};
        D3D12_CLEAR_VALUE clearValue{};
        const D3D12_CLEAR_VALUE* optimizedClearValue = nullptr;
        D3D12_RESOURCE_STATES initialState = ToNativeState(artifact.initialState);

        if (artifact.resourceKind == pkg::ResourceKind::Buffer)
        {
            if ((allocation.kind == pkg::AllocationKind::Committed && allocation.sizeBytes != artifact.sizeBytes) ||
                (allocation.kind == pkg::AllocationKind::Placed && allocation.sizeBytes < artifact.sizeBytes) ||
                (allocation.heapClass != pkg::HeapClass::DefaultBuffer && allocation.heapClass != pkg::HeapClass::Upload))
                return base::Result<void, runtime::RuntimeError>::Failure(Error("load/CreateResource", "buffer allocation artifact is invalid"));
            heap.Type = allocation.heapClass == pkg::HeapClass::Upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = artifact.sizeBytes;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_UNKNOWN;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            for (std::uint32_t i = 0; i < artifact.viewCount; ++i)
            {
                const auto& resourceView = view_.Views()[artifact.firstView + i];
                if (resourceView.viewClass == pkg::ViewClass::UnorderedAccess)
                    desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            }
            initialState = allocation.heapClass == pkg::HeapClass::Upload
                ? D3D12_RESOURCE_STATE_GENERIC_READ
                : D3D12_RESOURCE_STATE_COMMON;
        }
        else if (artifact.resourceKind == pkg::ResourceKind::Texture2D)
        {
            if (artifact.physicalInstanceCount != 1 || artifact.depthOrArraySize != 1 || artifact.mipLevels != 1 || artifact.sampleCount != 1 || artifact.planeCount != 1)
                return base::Result<void, runtime::RuntimeError>::Failure(Error("load/CreateResource", "Texture2D common artifact is outside the fixed Slice-13 contract"));
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.DepthOrArraySize = artifact.depthOrArraySize;
            desc.MipLevels = artifact.mipLevels;
            desc.Format = ToDxgi(artifact.format);
            desc.SampleDesc.Count = artifact.sampleCount;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            if (artifact.format == pkg::Format::B8G8R8A8Unorm)
            {
                if (allocation.heapClass != pkg::HeapClass::DefaultTexture || artifact.extentMode != pkg::ExtentMode::Fixed ||
                    artifact.width == 0 || artifact.height == 0 || artifact.initialDataSize == 0)
                    return base::Result<void, runtime::RuntimeError>::Failure(Error("load/CreateResource", "sampled Texture2D artifact is outside the fixed Slice-13 contract"));
                desc.Width = artifact.width;
                desc.Height = artifact.height;
            }
            else if (artifact.format == pkg::Format::D32Float)
            {
                if (allocation.heapClass != pkg::HeapClass::RenderTargetOrDepth || artifact.extentMode != pkg::ExtentMode::SurfaceRelative ||
                    artifact.width != 0 || artifact.height != 0 || artifact.initialDataSize != 0)
                    return base::Result<void, runtime::RuntimeError>::Failure(Error("load/CreateResource", "depth attachment artifact is outside the fixed Slice-13 contract"));
                desc.Width = std::max(1u, surface_->ClientWidth());
                desc.Height = std::max(1u, surface_->ClientHeight());
                desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
                clearValue.Format = DXGI_FORMAT_D32_FLOAT;
                clearValue.DepthStencil.Depth = 1.0f;
                clearValue.DepthStencil.Stencil = 0;
                optimizedClearValue = &clearValue;
            }
            else
            {
                return base::Result<void, runtime::RuntimeError>::Failure(Error("load/CreateResource", "Texture2D format is unsupported"));
            }
        }
        else
        {
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/CreateResource", "unsupported package-owned resource kind"));
        }

        for (std::uint32_t instanceIndex = 0; instanceIndex < artifact.physicalInstanceCount; ++instanceIndex)
        {
            HRESULT hr = S_OK;
            if (allocation.kind == pkg::AllocationKind::Committed)
            {
                hr = device_->CreateCommittedResource(
                    &heap, D3D12_HEAP_FLAG_NONE, &desc, initialState, optimizedClearValue,
                    IID_PPV_ARGS(&resources_[id.value][instanceIndex]));
            }
            else
            {
                if (allocation.heapClass != pkg::HeapClass::DefaultBuffer || allocation.aliasGroup == package::InvalidIndex)
                    return base::Result<void, runtime::RuntimeError>::Failure(Error("load/CreateResource", "placed allocation is not a valid alias buffer heap"));
                if (placedHeaps_[artifact.allocation.value].empty())
                    placedHeaps_[artifact.allocation.value].resize(allocation.physicalInstanceCount);
                if (!placedHeaps_[artifact.allocation.value][instanceIndex])
                {
                    D3D12_HEAP_DESC heapDesc{};
                    heapDesc.SizeInBytes = allocation.sizeBytes;
                    heapDesc.Alignment = allocation.alignment;
                    heapDesc.Properties = heap;
                    heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
                    hr = device_->CreateHeap(&heapDesc, IID_PPV_ARGS(&placedHeaps_[artifact.allocation.value][instanceIndex]));
                    if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("load/create-alias-heap", hr, device_.Get()));
                }
                hr = device_->CreatePlacedResource(
                    placedHeaps_[artifact.allocation.value][instanceIndex].Get(), 0, &desc, initialState, optimizedClearValue,
                    IID_PPV_ARGS(&resources_[id.value][instanceIndex]));
            }
            if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("load/create-resource", hr, device_.Get()));

            for (std::uint32_t i = 0; i < artifact.viewCount; ++i)
            {
                const auto& resourceView = view_.Views()[artifact.firstView + i];
                const std::uint32_t descriptorIndex = resourceView.descriptorIndex + resourceView.descriptorInstanceStride * instanceIndex;
                if (resourceView.viewClass == pkg::ViewClass::ShaderResource)
                {
                    if (!shaderHeap_)
                        return base::Result<void, runtime::RuntimeError>::Failure(Error("load/CreateResource", "shader descriptor heap is unavailable"));
                    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    if (artifact.resourceKind == pkg::ResourceKind::Texture2D)
                    {
                        srv.Format = ToDxgi(resourceView.format);
                        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                        srv.Texture2D.MostDetailedMip = resourceView.firstMip;
                        srv.Texture2D.MipLevels = resourceView.mipCount;
                        srv.Texture2D.PlaneSlice = resourceView.firstPlane;
                        srv.Texture2D.ResourceMinLODClamp = 0.0f;
                    }
                    else if (artifact.resourceKind == pkg::ResourceKind::Buffer)
                    {
                        if (resourceView.strideBytes == 0 || resourceView.byteSize == 0 || resourceView.byteSize % resourceView.strideBytes != 0)
                            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/CreateResource", "structured-buffer SRV contract is invalid"));
                        srv.Format = DXGI_FORMAT_UNKNOWN;
                        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                        srv.Buffer.FirstElement = resourceView.byteOffset / resourceView.strideBytes;
                        srv.Buffer.NumElements = static_cast<UINT>(resourceView.byteSize / resourceView.strideBytes);
                        srv.Buffer.StructureByteStride = resourceView.strideBytes;
                        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
                    }
                    else
                    {
                        return base::Result<void, runtime::RuntimeError>::Failure(Error("load/CreateResource", "SRV resource kind is unsupported"));
                    }
                    auto handle = shaderHeap_->GetCPUDescriptorHandleForHeapStart();
                    handle.ptr += static_cast<SIZE_T>(descriptorIndex) * shaderDescriptorIncrement_;
                    device_->CreateShaderResourceView(resources_[id.value][instanceIndex].Get(), &srv, handle);
                }
                else if (resourceView.viewClass == pkg::ViewClass::UnorderedAccess)
                {
                    if (!shaderHeap_ || artifact.resourceKind != pkg::ResourceKind::Buffer || resourceView.strideBytes == 0 ||
                        resourceView.byteSize == 0 || resourceView.byteSize % resourceView.strideBytes != 0)
                        return base::Result<void, runtime::RuntimeError>::Failure(Error("load/CreateResource", "structured-buffer UAV contract is invalid"));
                    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
                    uav.Format = DXGI_FORMAT_UNKNOWN;
                    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                    uav.Buffer.FirstElement = resourceView.byteOffset / resourceView.strideBytes;
                    uav.Buffer.NumElements = static_cast<UINT>(resourceView.byteSize / resourceView.strideBytes);
                    uav.Buffer.StructureByteStride = resourceView.strideBytes;
                    uav.Buffer.CounterOffsetInBytes = 0;
                    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
                    auto handle = shaderHeap_->GetCPUDescriptorHandleForHeapStart();
                    handle.ptr += static_cast<SIZE_T>(descriptorIndex) * shaderDescriptorIncrement_;
                    device_->CreateUnorderedAccessView(resources_[id.value][instanceIndex].Get(), nullptr, &uav, handle);
                }
                else if (resourceView.viewClass == pkg::ViewClass::DepthStencil)
                {
                    if (!dsvHeap_ || artifact.physicalInstanceCount != 1)
                        return base::Result<void, runtime::RuntimeError>::Failure(Error("load/CreateResource", "DSV descriptor heap or instance contract is invalid"));
                    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
                    dsv.Format = ToDxgi(resourceView.format);
                    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                    dsv.Flags = D3D12_DSV_FLAG_NONE;
                    dsv.Texture2D.MipSlice = resourceView.firstMip;
                    auto handle = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
                    handle.ptr += static_cast<SIZE_T>(descriptorIndex) * dsvIncrement_;
                    device_->CreateDepthStencilView(resources_[id.value][instanceIndex].Get(), &dsv, handle);
                }
            }
        }
        return base::Result<void, runtime::RuntimeError>::Success();
    }

    base::Result<void, runtime::RuntimeError> ActivateAlias(const pkg::ActivateAliasPayload& payload, ID3D12GraphicsCommandList* list)
    {
        if (!list || !payload.after.IsValid() || payload.after.value >= view_.Resources().size())
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/ActivateAlias", "alias payload is invalid"));
        const auto& after = view_.Resources()[payload.after.value];
        if (!after.allocation.IsValid() || after.allocation.value >= view_.Allocations().size())
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/ActivateAlias", "after resource allocation is invalid"));
        const auto& allocation = view_.Allocations()[after.allocation.value];
        if (allocation.kind != pkg::AllocationKind::Placed || allocation.aliasGroup == package::InvalidIndex ||
            allocation.aliasGroup >= activeAliasResources_.size())
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/ActivateAlias", "after resource is not in an alias group"));
        ID3D12Resource* beforeObject = nullptr;
        if (payload.before.IsValid())
        {
            if (payload.before.value >= view_.Resources().size())
                return base::Result<void, runtime::RuntimeError>::Failure(Error("load/ActivateAlias", "before resource is invalid"));
            const auto& before = view_.Resources()[payload.before.value];
            if (before.allocation != after.allocation || before.flags != after.flags ||
                activeAliasResources_[allocation.aliasGroup] != payload.before.value)
                return base::Result<void, runtime::RuntimeError>::Failure(Error("load/ActivateAlias", "alias predecessor does not match the active allocation owner"));
            beforeObject = ResourceObjectAt(payload.before, 0);
        }
        else if (activeAliasResources_[allocation.aliasGroup] != package::InvalidIndex)
        {
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/ActivateAlias", "alias group already has an active resource"));
        }
        ID3D12Resource* afterObject = ResourceObjectAt(payload.after, 0);
        if (!afterObject) return base::Result<void, runtime::RuntimeError>::Failure(Error("load/ActivateAlias", "alias resource object is unavailable"));
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Aliasing.pResourceBefore = beforeObject;
        barrier.Aliasing.pResourceAfter = afterObject;
        list->ResourceBarrier(1, &barrier);
        activeAliasResources_[allocation.aliasGroup] = payload.after.value;
        return base::Result<void, runtime::RuntimeError>::Success();
    }

    base::Result<void, runtime::RuntimeError> UploadBuffer(const pkg::UploadBufferPayload& payload)
    {
        if (!loadBatchOpen_)
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/UploadBuffer", "Package-declared load queue batch is not open"));
        if (!payload.resource.IsValid() || payload.resource.value >= resources_.size())
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/UploadBuffer", "destination resource is invalid"));
        const auto& artifact = view_.Resources()[payload.resource.value];
        if (artifact.resourceKind != pkg::ResourceKind::Buffer || artifact.origin != pkg::ResourceOrigin::PackageOwned ||
            payload.bytes == 0 || payload.bytes > artifact.sizeBytes || payload.sourceOffset > view_.InitialData().size() ||
            payload.bytes > view_.InitialData().size() - payload.sourceOffset)
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/UploadBuffer", "buffer upload contract is invalid"));

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = payload.bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> upload;
        HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
        if (FAILED(hr))
            return base::Result<void, runtime::RuntimeError>::Failure(HResultError("load/create-upload-buffer", hr, device_.Get()));
        void* mapped = nullptr;
        D3D12_RANGE readRange{0, 0};
        hr = upload->Map(0, &readRange, &mapped);
        if (FAILED(hr))
            return base::Result<void, runtime::RuntimeError>::Failure(HResultError("load/map-upload-buffer", hr, device_.Get()));
        std::memcpy(mapped, view_.InitialData().data() + payload.sourceOffset, static_cast<std::size_t>(payload.bytes));
        upload->Unmap(0, nullptr);

        const std::uint32_t instances = IsTemporal(artifact) ? artifact.physicalInstanceCount : 1u;
        for (std::uint32_t instanceIndex = 0; instanceIndex < instances; ++instanceIndex)
        {
            ID3D12Resource* destination = ResourceObjectAt(payload.resource, instanceIndex);
            if (!destination)
                return base::Result<void, runtime::RuntimeError>::Failure(Error("load/UploadBuffer", "destination resource instance is unavailable"));
            loadCommandList_->CopyBufferRegion(destination, 0, upload.Get(), 0, payload.bytes);
        }
        uploadResources_.push_back(std::move(upload));
        return base::Result<void, runtime::RuntimeError>::Success();
    }

    base::Result<void, runtime::RuntimeError> ExecuteCopy(const pkg::CopyBufferPayload& payload, const char* stage = "load/ExecuteCopy")
    {
        ID3D12GraphicsCommandList* list = nullptr;
        if (loadBatchOpen_) list = loadCommandList_.Get();
        else if (copyFrameCommandOpen_) list = copyCommandList_.Get();
        else if (commandOpen_ && IsDirectQueue(pkg::QueueId{activeCommandQueue_})) list = commandList_.Get();
        if (!list)
            return base::Result<void, runtime::RuntimeError>::Failure(Error(stage, "Package-declared Copy-capable queue batch is not open"));
        if (!payload.source.IsValid() || !payload.destination.IsValid() ||
            payload.source.value >= resources_.size() || payload.destination.value >= resources_.size() ||
            ResourceObject(payload.source) == nullptr || ResourceObject(payload.destination) == nullptr || payload.bytes == 0)
            return base::Result<void, runtime::RuntimeError>::Failure(Error(stage, "copy resource reference is invalid"));
        const auto& sourceArtifact = view_.Resources()[payload.source.value];
        const auto& destinationArtifact = view_.Resources()[payload.destination.value];
        if (sourceArtifact.resourceKind != pkg::ResourceKind::Buffer || destinationArtifact.resourceKind != pkg::ResourceKind::Buffer ||
            payload.sourceOffset + payload.bytes > sourceArtifact.sizeBytes ||
            payload.destinationOffset + payload.bytes > destinationArtifact.sizeBytes)
            return base::Result<void, runtime::RuntimeError>::Failure(Error(stage, "copy range is invalid"));
        const pkg::ResourceState copySourceState{pkg::StateClass::Explicit, 0, static_cast<std::uint32_t>(pkg::ExplicitStateBits::CopySource)};
        const pkg::ResourceState copyDestinationState{pkg::StateClass::Explicit, 0, static_cast<std::uint32_t>(pkg::ExplicitStateBits::CopyDestination)};
        if (TrackedState(payload.source) != copySourceState || TrackedState(payload.destination) != copyDestinationState)
            return base::Result<void, runtime::RuntimeError>::Failure(Error(stage, "copy resources are not in Package-planned states"));
        list->CopyBufferRegion(ResourceObject(payload.destination), payload.destinationOffset,
            ResourceObject(payload.source), payload.sourceOffset, payload.bytes);
        return base::Result<void, runtime::RuntimeError>::Success();
    }


    base::Result<void, runtime::RuntimeError> UploadTexture(const pkg::UploadTexturePayload& payload)
    {
        if (!loadBatchOpen_)
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/UploadTexture", "Package-declared load queue batch is not open"));
        if (!payload.resource.IsValid() || payload.resource.value >= resources_.size() || ResourceObject(payload.resource) == nullptr)
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/UploadTexture", "destination texture is not materialized"));
        const auto& artifact = view_.Resources()[payload.resource.value];
        if (artifact.resourceKind != pkg::ResourceKind::Texture2D || artifact.origin != pkg::ResourceOrigin::PackageOwned)
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/UploadTexture", "destination is not a package-owned Texture2D"));
        if (payload.sourceOffset > view_.InitialData().size() || payload.sourceSliceBytes > view_.InitialData().size() - payload.sourceOffset ||
            payload.sourceRowBytes != artifact.width * 4u || payload.sourceSliceBytes != payload.sourceRowBytes * artifact.height ||
            payload.mipLevel != 0 || payload.arrayLayer != 0 || payload.plane != 0)
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/UploadTexture", "texture source layout is invalid"));

        const auto textureDesc = ResourceObject(payload.resource)->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rows = 0;
        UINT64 rowBytes = 0;
        UINT64 uploadBytes = 0;
        device_->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, &rows, &rowBytes, &uploadBytes);
        if (rows != artifact.height || rowBytes != payload.sourceRowBytes || uploadBytes == 0)
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/UploadTexture", "D3D12 footprint does not match the Package Texture2D contract"));

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC uploadDesc{};
        uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDesc.Width = uploadBytes;
        uploadDesc.Height = 1;
        uploadDesc.DepthOrArraySize = 1;
        uploadDesc.MipLevels = 1;
        uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
        uploadDesc.SampleDesc.Count = 1;
        uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> upload;
        HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
        if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("load/create-texture-upload", hr, device_.Get()));

        void* mapped = nullptr;
        D3D12_RANGE readRange{0, 0};
        hr = upload->Map(0, &readRange, &mapped);
        if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("load/map-texture-upload", hr, device_.Get()));
        const auto* source = view_.InitialData().data() + payload.sourceOffset;
        auto* destination = static_cast<std::byte*>(mapped) + footprint.Offset;
        for (UINT row = 0; row < rows; ++row)
        {
            std::memcpy(destination + static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
                source + static_cast<std::size_t>(row) * payload.sourceRowBytes,
                payload.sourceRowBytes);
        }
        upload->Unmap(0, nullptr);

        D3D12_TEXTURE_COPY_LOCATION destinationLocation{};
        destinationLocation.pResource = ResourceObject(payload.resource);
        destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destinationLocation.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
        sourceLocation.pResource = upload.Get();
        sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sourceLocation.PlacedFootprint = footprint;
        loadCommandList_->CopyTextureRegion(&destinationLocation, 0, 0, 0, &sourceLocation, nullptr);
        uploadResources_.push_back(std::move(upload));
        return base::Result<void, runtime::RuntimeError>::Success();
    }

    base::Result<void, runtime::RuntimeError> ScheduleBufferVerification(const pkg::VerifyBufferContentsPayload& payload)
    {
        if (!payload.resource.IsValid() || payload.resource.value >= resources_.size())
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/VerifyBufferContents", "source buffer is invalid"));
        if (payload.bytes == 0)
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/VerifyBufferContents", "verification byte count must be positive"));
        const auto& artifact = view_.Resources()[payload.resource.value];
        if (artifact.resourceKind != pkg::ResourceKind::Buffer || artifact.origin != pkg::ResourceOrigin::PackageOwned ||
            payload.resourceOffset > artifact.sizeBytes || payload.bytes > artifact.sizeBytes - payload.resourceOffset ||
            payload.expectedDataOffset > view_.InitialData().size() || payload.bytes > view_.InitialData().size() - payload.expectedDataOffset)
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/VerifyBufferContents", "buffer verification range is invalid"));
        const pkg::ResourceState requiredState{
            pkg::StateClass::Explicit, 0, static_cast<std::uint32_t>(pkg::ExplicitStateBits::CopySource)};

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = payload.bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        const std::uint32_t instances = IsTemporal(artifact) ? artifact.physicalInstanceCount : 1u;
        for (std::uint32_t instanceIndex = 0; instanceIndex < instances; ++instanceIndex)
        {
            if (resourceStates_[payload.resource.value][instanceIndex] != requiredState)
                return base::Result<void, runtime::RuntimeError>::Failure(Error("load/VerifyBufferContents", "source buffer instance is not in CopySource state"));
            ID3D12Resource* source = ResourceObjectAt(payload.resource, instanceIndex);
            if (!source)
                return base::Result<void, runtime::RuntimeError>::Failure(Error("load/VerifyBufferContents", "source buffer instance is unavailable"));
            PendingBufferVerification pending;
            pending.resource = payload.resource;
            pending.instanceIndex = instanceIndex;
            pending.expectedDataOffset = payload.expectedDataOffset;
            pending.bytes = payload.bytes;
            HRESULT hr = device_->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&pending.readback));
            if (FAILED(hr))
                return base::Result<void, runtime::RuntimeError>::Failure(HResultError("load/create-readback-buffer", hr, device_.Get()));
            loadCommandList_->CopyBufferRegion(pending.readback.Get(), 0, source, payload.resourceOffset, payload.bytes);
            pendingBufferVerifications_.push_back(std::move(pending));
        }
        return base::Result<void, runtime::RuntimeError>::Success();
    }

    base::Result<void, runtime::RuntimeError> CompleteBufferVerifications()
    {
        for (auto& pending : pendingBufferVerifications_)
        {
            void* mapped = nullptr;
            const D3D12_RANGE readRange{0, static_cast<SIZE_T>(pending.bytes)};
            const HRESULT hr = pending.readback->Map(0, &readRange, &mapped);
            if (FAILED(hr))
                return base::Result<void, runtime::RuntimeError>::Failure(HResultError("load/map-readback-buffer", hr, device_.Get()));

            const auto expected = view_.InitialData().subspan(
                static_cast<std::size_t>(pending.expectedDataOffset),
                static_cast<std::size_t>(pending.bytes));
            const auto* actual = static_cast<const std::byte*>(mapped);
            std::size_t firstMismatch = expected.size();
            for (std::size_t i = 0; i < expected.size(); ++i)
            {
                if (actual[i] != expected[i])
                {
                    firstMismatch = i;
                    break;
                }
            }
            unsigned int expectedValue = 0;
            unsigned int actualValue = 0;
            if (firstMismatch != expected.size())
            {
                expectedValue = std::to_integer<unsigned int>(expected[firstMismatch]);
                actualValue = std::to_integer<unsigned int>(actual[firstMismatch]);
            }
            const D3D12_RANGE noWrite{0, 0};
            pending.readback->Unmap(0, &noWrite);
            if (firstMismatch != expected.size())
            {
                std::ostringstream message;
                message << "GPU readback differs from Package InitialData for resource "
                        << pending.resource.value << " instance " << pending.instanceIndex << " at byte " << firstMismatch
                        << " (expected " << expectedValue
                        << ", actual " << actualValue << ')';
                return base::Result<void, runtime::RuntimeError>::Failure(
                    Error("load/VerifyBufferContents", message.str()));
            }
        }
        pendingBufferVerifications_.clear();
        return base::Result<void, runtime::RuntimeError>::Success();
    }

    base::Result<void, runtime::RuntimeError> ScheduleTextureVerification(const pkg::VerifyTextureContentsPayload& payload)
    {
        if (!payload.resource.IsValid() || payload.resource.value >= resources_.size() || ResourceObject(payload.resource) == nullptr)
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/VerifyTextureContents", "source texture is not materialized"));
        const auto& artifact = view_.Resources()[payload.resource.value];
        if (artifact.resourceKind != pkg::ResourceKind::Texture2D || artifact.origin != pkg::ResourceOrigin::PackageOwned ||
            payload.expectedRowBytes != artifact.width * 4u || payload.width != artifact.width || payload.height != artifact.height ||
            payload.expectedDataOffset > view_.InitialData().size() ||
            static_cast<std::uint64_t>(payload.expectedRowBytes) * payload.height > view_.InitialData().size() - payload.expectedDataOffset)
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/VerifyTextureContents", "Texture2D verification contract is invalid"));
        const pkg::ResourceState requiredState{pkg::StateClass::Explicit, 0, static_cast<std::uint32_t>(pkg::ExplicitStateBits::CopySource)};
        if (TrackedState(payload.resource) != requiredState)
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/VerifyTextureContents", "source texture is not in CopySource state"));

        const auto textureDesc = ResourceObject(payload.resource)->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rows = 0;
        UINT64 rowBytes = 0;
        UINT64 readbackBytes = 0;
        device_->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, &rows, &rowBytes, &readbackBytes);
        if (rows != payload.height || rowBytes != payload.expectedRowBytes || readbackBytes == 0)
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/VerifyTextureContents", "D3D12 readback footprint is incompatible with the Package layout"));

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = readbackBytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        PendingTextureVerification pending;
        pending.resource = payload.resource;
        pending.expectedDataOffset = payload.expectedDataOffset;
        pending.expectedRowBytes = payload.expectedRowBytes;
        pending.width = payload.width;
        pending.height = payload.height;
        pending.footprint = footprint;
        HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&pending.readback));
        if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("load/create-texture-readback", hr, device_.Get()));

        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = pending.readback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = ResourceObject(payload.resource);
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0;
        loadCommandList_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        pendingTextureVerifications_.push_back(std::move(pending));
        return base::Result<void, runtime::RuntimeError>::Success();
    }

    base::Result<void, runtime::RuntimeError> CompleteTextureVerifications()
    {
        for (auto& pending : pendingTextureVerifications_)
        {
            const auto readSize = static_cast<SIZE_T>(pending.footprint.Offset +
                static_cast<UINT64>(pending.footprint.Footprint.RowPitch) * (pending.height - 1u) +
                pending.expectedRowBytes);
            void* mapped = nullptr;
            const D3D12_RANGE readRange{0, readSize};
            const HRESULT hr = pending.readback->Map(0, &readRange, &mapped);
            if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("load/map-texture-readback", hr, device_.Get()));

            const auto* actualBase = static_cast<const std::byte*>(mapped) + pending.footprint.Offset;
            const auto* expectedBase = view_.InitialData().data() + pending.expectedDataOffset;
            std::size_t mismatch = static_cast<std::size_t>(pending.expectedRowBytes) * pending.height;
            unsigned int expectedValue = 0;
            unsigned int actualValue = 0;
            for (std::uint32_t row = 0; row < pending.height && mismatch == static_cast<std::size_t>(pending.expectedRowBytes) * pending.height; ++row)
            {
                const auto* actualRow = actualBase + static_cast<std::size_t>(row) * pending.footprint.Footprint.RowPitch;
                const auto* expectedRow = expectedBase + static_cast<std::size_t>(row) * pending.expectedRowBytes;
                for (std::uint32_t columnByte = 0; columnByte < pending.expectedRowBytes; ++columnByte)
                {
                    if (actualRow[columnByte] != expectedRow[columnByte])
                    {
                        mismatch = static_cast<std::size_t>(row) * pending.expectedRowBytes + columnByte;
                        expectedValue = std::to_integer<unsigned int>(expectedRow[columnByte]);
                        actualValue = std::to_integer<unsigned int>(actualRow[columnByte]);
                        break;
                    }
                }
            }
            const D3D12_RANGE noWrite{0, 0};
            pending.readback->Unmap(0, &noWrite);
            if (mismatch != static_cast<std::size_t>(pending.expectedRowBytes) * pending.height)
            {
                std::ostringstream message;
                message << "GPU Texture2D readback differs from Package InitialData for resource " << pending.resource.value
                        << " at tightly packed byte " << mismatch << " (expected " << expectedValue << ", actual " << actualValue << ')';
                return base::Result<void, runtime::RuntimeError>::Failure(Error("load/VerifyTextureContents", message.str()));
            }
        }
        pendingTextureVerifications_.clear();
        return base::Result<void, runtime::RuntimeError>::Success();
    }

    base::Result<void, runtime::RuntimeError> ApplyDynamicData(pkg::DynamicSlotId id)
    {
        if (!id.IsValid() || id.value >= view_.DynamicSlots().size())
            return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ApplyDynamicData", "dynamic slot ID is invalid"));
        if (dynamicApplied_[id.value])
            return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ApplyDynamicData", "dynamic slot was applied more than once"));

        const auto& slot = view_.DynamicSlots()[id.value];
        const auto bytes = dynamicBindings_[id.value];
        if (bytes.size() != slot.requiredBytes)
            return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ApplyDynamicData", "invocation bytes do not match the dynamic slot contract"));
        if (!slot.destinationResource.IsValid() || slot.destinationResource.value >= resources_.size() || ResourceObject(slot.destinationResource) == nullptr)
            return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ApplyDynamicData", "dynamic destination resource is unavailable"));

        void* mapped = nullptr;
        D3D12_RANGE readRange{0, 0};
        const HRESULT hr = ResourceObject(slot.destinationResource)->Map(0, &readRange, &mapped);
        if (FAILED(hr))
            return base::Result<void, runtime::RuntimeError>::Failure(HResultError("frame/map-dynamic-buffer", hr, device_.Get()));
        auto* destination = static_cast<std::byte*>(mapped) + slot.destinationOffset;
        std::memcpy(destination, bytes.data(), bytes.size());
        D3D12_RANGE writtenRange{
            static_cast<SIZE_T>(slot.destinationOffset),
            static_cast<SIZE_T>(slot.destinationOffset + slot.requiredBytes)};
        ResourceObject(slot.destinationResource)->Unmap(0, &writtenRange);
        dynamicApplied_[id.value] = true;
        return base::Result<void, runtime::RuntimeError>::Success();
    }

    base::Result<void, runtime::RuntimeError> TransitionResourceAt(
        pkg::ResourceId id,
        std::uint32_t instanceIndex,
        const pkg::ResourceState& before,
        const pkg::ResourceState& after,
        ID3D12GraphicsCommandList* list,
        D3D12_COMMAND_LIST_TYPE queueType)
    {
        if (!id.IsValid() || id.value >= resourceStates_.size() || instanceIndex >= resourceStates_[id.value].size())
            return base::Result<void, runtime::RuntimeError>::Failure(Error("transition", "resource instance is invalid"));
        if (resourceStates_[id.value][instanceIndex] != before)
            return base::Result<void, runtime::RuntimeError>::Failure(Error("transition", "tracked package state does not match operation before-state"));
        ID3D12Resource* resource = ResourceObjectAt(id, instanceIndex);
        if (!resource) return base::Result<void, runtime::RuntimeError>::Failure(Error("transition", "resource object is unavailable"));
        const auto nativeBefore = ToNativeState(before, queueType);
        const auto nativeAfter = ToNativeState(after, queueType);
        if (nativeBefore != nativeAfter)
        {
            auto barrier = TransitionBarrier(resource, nativeBefore, nativeAfter);
            list->ResourceBarrier(1, &barrier);
        }
        resourceStates_[id.value][instanceIndex] = after;
        return base::Result<void, runtime::RuntimeError>::Success();
    }

    base::Result<void, runtime::RuntimeError> TransitionResource(
        pkg::ResourceId id,
        const pkg::ResourceState& before,
        const pkg::ResourceState& after,
        ID3D12GraphicsCommandList* list,
        D3D12_COMMAND_LIST_TYPE queueType)
    {
        return TransitionResourceAt(id, PhysicalInstanceIndex(id), before, after, list, queueType);
    }

    base::Result<void, runtime::RuntimeError> TransitionResource(
        const pkg::ResourceViewArtifact& resourceView,
        const pkg::ResourceState& before,
        const pkg::ResourceState& after,
        ID3D12GraphicsCommandList* list,
        D3D12_COMMAND_LIST_TYPE queueType)
    {
        return TransitionResourceAt(resourceView.resource, PhysicalInstanceIndex(resourceView), before, after, list, queueType);
    }

    base::Result<void, runtime::RuntimeError> CreateRootSignature(pkg::BindingLayoutId id)
    {
        if (!id.IsValid() || id.value >= view_.BindingLayouts().size()) return base::Result<void, runtime::RuntimeError>::Failure(Error("load/CreateRootSignature", "binding layout ID is invalid"));
        const auto blob = view_.ResolveBlob(view_.BindingLayouts()[id.value].serializedRootSignature);
        if (!blob) return PackageFailure("load/CreateRootSignature", blob.Error());
        const HRESULT hr = device_->CreateRootSignature(0, blob.Value().data(), blob.Value().size(), IID_PPV_ARGS(&rootSignatures_[id.value]));
        if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("load/create-root-signature", hr, device_.Get()));
        return base::Result<void, runtime::RuntimeError>::Success();
    }

    base::Result<void, runtime::RuntimeError> CreateGraphicsPipeline(pkg::ExecutableId id)
    {
        if (!id.IsValid() || id.value >= view_.Executables().size())
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/CreateGraphicsPipeline", "executable ID is invalid"));
        const auto& executable = view_.Executables()[id.value];
        const bool hasDepth = executable.depthFormat == pkg::Format::D32Float && executable.depthStateId == 1;
        if (executable.rasterStateId != 0 || executable.blendStateId != 0 ||
            (!hasDepth && (executable.depthFormat != pkg::Format::Unknown || executable.depthStateId != 0)) ||
            executable.sampleCount != 1 || executable.sampleQuality != 0 ||
            executable.primitiveTopology != pkg::PrimitiveTopology::TriangleList ||
            executable.primitiveTopologyType != pkg::PrimitiveTopologyType::Triangle ||
            executable.colorFormat != pkg::Format::B8G8R8A8Unorm || executable.colorFormatRange.count != 1)
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/CreateGraphicsPipeline", "executable specialization is unsupported"));
        if (!executable.program.IsValid() || executable.program.value >= view_.Programs().size() ||
            !executable.bindingLayout.IsValid() || executable.bindingLayout.value >= rootSignatures_.size())
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/CreateGraphicsPipeline", "program or binding layout reference is invalid"));
        const auto& program = view_.Programs()[executable.program.value];
        if (program.kind != pkg::ProgramKind::Raster || !program.vertexShader.IsValid() || !program.pixelShader.IsValid() ||
            program.vertexShader.value >= view_.Shaders().size() || program.pixelShader.value >= view_.Shaders().size())
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/CreateGraphicsPipeline", "raster program contract is invalid"));
        const auto vs = view_.ResolveBlob(view_.Shaders()[program.vertexShader.value].bytecode);
        const auto ps = view_.ResolveBlob(view_.Shaders()[program.pixelShader.value].bytecode);
        if (!vs) return PackageFailure("load/CreateGraphicsPipeline/VS", vs.Error());
        if (!ps) return PackageFailure("load/CreateGraphicsPipeline/PS", ps.Error());
        if (!rootSignatures_[executable.bindingLayout.value])
            return base::Result<void, runtime::RuntimeError>::Failure(Error("load/CreateGraphicsPipeline", "root signature has not been materialized"));

        std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout;
        for (std::uint32_t i = 0; i < executable.vertexElementRange.count; ++i)
        {
            const auto& element = view_.VertexElements()[executable.vertexElementRange.first + i];
            const char* semantic = SemanticName(element.meaning);
            if (!semantic || element.inputSlot != 0 || element.instanceStepRate != 0 || element.flags != 0)
                return base::Result<void, runtime::RuntimeError>::Failure(Error("load/CreateGraphicsPipeline", "vertex element is unsupported"));
            D3D12_INPUT_ELEMENT_DESC native{};
            native.SemanticName = semantic;
            native.SemanticIndex = element.semanticIndex;
            native.Format = ToDxgi(element.format);
            native.InputSlot = element.inputSlot;
            native.AlignedByteOffset = element.alignedByteOffset;
            native.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            inputLayout.push_back(native);
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = rootSignatures_[executable.bindingLayout.value].Get();
        desc.VS = {vs.Value().data(), vs.Value().size()};
        desc.PS = {ps.Value().data(), ps.Value().size()};
        auto& rt = desc.BlendState.RenderTarget[0];
        rt.SrcBlend = D3D12_BLEND_ONE; rt.DestBlend = D3D12_BLEND_ZERO; rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE; rt.DestBlendAlpha = D3D12_BLEND_ZERO; rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.LogicOp = D3D12_LOGIC_OP_NOOP; rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        desc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.DepthStencilState.DepthEnable = hasDepth;
        desc.DepthStencilState.DepthWriteMask = hasDepth ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        desc.DepthStencilState.StencilEnable = FALSE;
        desc.InputLayout = {inputLayout.data(), static_cast<UINT>(inputLayout.size())};
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = ToDxgi(executable.colorFormat);
        desc.DSVFormat = hasDepth ? ToDxgi(executable.depthFormat) : DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = executable.sampleCount;
        desc.SampleDesc.Quality = executable.sampleQuality;
        const HRESULT hr = device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipelineStates_[id.value]));
        if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("load/create-graphics-pipeline", hr, device_.Get()));
        return base::Result<void, runtime::RuntimeError>::Success();
    }

    base::Result<void, runtime::RuntimeError> CreateComputePipeline(pkg::ComputeExecutableId id)
    {
        if (!id.IsValid() || id.value >= view_.ComputeExecutables().size())
            return base::Result<void, runtime::RuntimeError>::Failure(
                Error("load/CreateComputePipeline", "compute executable ID is invalid"));
        const auto& executable = view_.ComputeExecutables()[id.value];
        if (executable.flags != 0 || !executable.program.IsValid() ||
            executable.program.value >= view_.Programs().size() ||
            !executable.bindingLayout.IsValid() ||
            executable.bindingLayout.value >= rootSignatures_.size())
            return base::Result<void, runtime::RuntimeError>::Failure(
                Error("load/CreateComputePipeline", "compute executable is outside the fixed Slice-13 schema"));
        const auto& program = view_.Programs()[executable.program.value];
        if (program.kind != pkg::ProgramKind::Compute ||
            !program.computeShader.IsValid() || program.computeShader.value >= view_.Shaders().size() ||
            program.bindingLayout != executable.bindingLayout)
            return base::Result<void, runtime::RuntimeError>::Failure(
                Error("load/CreateComputePipeline", "compute program contract is invalid"));
        const auto cs = view_.ResolveBlob(view_.Shaders()[program.computeShader.value].bytecode);
        if (!cs) return PackageFailure("load/CreateComputePipeline/CS", cs.Error());
        if (!rootSignatures_[executable.bindingLayout.value])
            return base::Result<void, runtime::RuntimeError>::Failure(
                Error("load/CreateComputePipeline", "compute root signature has not been materialized"));

        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = rootSignatures_[executable.bindingLayout.value].Get();
        desc.CS = {cs.Value().data(), cs.Value().size()};
        const HRESULT hr = device_->CreateComputePipelineState(
            &desc, IID_PPV_ARGS(&computePipelineStates_[id.value]));
        if (FAILED(hr))
            return base::Result<void, runtime::RuntimeError>::Failure(
                HResultError("load/create-compute-pipeline", hr, device_.Get()));
        return base::Result<void, runtime::RuntimeError>::Success();
    }

    base::Result<void, runtime::RuntimeError> ExecuteCompute(pkg::ComputeCommandId id)
    {
        if (!id.IsValid() || id.value >= view_.ComputeCommands().size())
            return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteCompute", "compute command ID is invalid"));
        const auto& command = view_.ComputeCommands()[id.value];
        if (!command.executable.IsValid() || command.executable.value >= view_.ComputeExecutables().size() ||
            command.threadGroupCountX == 0 || command.threadGroupCountY == 0 || command.threadGroupCountZ == 0 || command.flags != 0)
            return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteCompute", "compute command contract is invalid"));
        const auto& executable = view_.ComputeExecutables()[command.executable.value];
        if (!computePipelineStates_[command.executable.value] || !rootSignatures_[executable.bindingLayout.value])
            return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteCompute", "compute executable objects are not materialized"));

        commandList_->SetPipelineState(computePipelineStates_[command.executable.value].Get());
        commandList_->SetComputeRootSignature(rootSignatures_[executable.bindingLayout.value].Get());
        const auto& layout = view_.BindingLayouts()[executable.bindingLayout.value];
        if (layout.descriptorRange.count != 0)
        {
            if (!shaderHeap_) return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteCompute", "shader descriptor heap is unavailable"));
            ID3D12DescriptorHeap* heaps[] = {shaderHeap_.Get()};
            commandList_->SetDescriptorHeaps(1, heaps);
        }
        const pkg::ResourceState nonPixelShaderReadState{
            pkg::StateClass::Explicit,
            0,
            static_cast<std::uint32_t>(pkg::ExplicitStateBits::NonPixelShaderRead)};
        const pkg::ResourceState unorderedWriteState{pkg::StateClass::Explicit, 0, static_cast<std::uint32_t>(pkg::ExplicitStateBits::UnorderedWrite)};
        for (std::uint32_t index = 0; index < layout.parameterRange.count; ++index)
        {
            const auto& parameter = view_.RootParameters()[layout.parameterRange.first + index];
            if (parameter.kind == pkg::RootParameterKind::ConstantBuffer)
            {
                if (!parameter.dynamicSlot.IsValid() || parameter.dynamicSlot.value >= dynamicApplied_.size() || !dynamicApplied_[parameter.dynamicSlot.value])
                    return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteCompute", "dynamic constant binding is invalid or was not applied"));
                const auto& slot = view_.DynamicSlots()[parameter.dynamicSlot.value];
                ID3D12Resource* resource = ResourceObject(slot.destinationResource);
                if (!resource) return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteCompute", "dynamic constant resource is unavailable"));
                commandList_->SetComputeRootConstantBufferView(parameter.rootParameterIndex, resource->GetGPUVirtualAddress() + slot.destinationOffset);
            }
            else if (parameter.kind == pkg::RootParameterKind::ShaderResourceTable)
            {
                if (!parameter.staticView.IsValid() || parameter.staticView.value >= view_.Views().size())
                    return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteCompute", "shader-resource view is invalid"));
                const auto& resourceView = view_.Views()[parameter.staticView.value];
                if (resourceView.viewClass != pkg::ViewClass::ShaderResource || TrackedState(resourceView) != nonPixelShaderReadState || !ResourceObject(resourceView))
                    return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteCompute", "shader resource is unavailable or not in NonPixelShaderRead state"));
                auto gpu = shaderHeap_->GetGPUDescriptorHandleForHeapStart();
                gpu.ptr += static_cast<UINT64>(DescriptorIndex(resourceView)) * shaderDescriptorIncrement_;
                commandList_->SetComputeRootDescriptorTable(parameter.rootParameterIndex, gpu);
            }
            else if (parameter.kind == pkg::RootParameterKind::UnorderedAccessTable)
            {
                if (!parameter.staticView.IsValid() || parameter.staticView.value >= view_.Views().size())
                    return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteCompute", "unordered-access view is invalid"));
                const auto& resourceView = view_.Views()[parameter.staticView.value];
                if (resourceView.viewClass != pkg::ViewClass::UnorderedAccess || TrackedState(resourceView) != unorderedWriteState || !ResourceObject(resourceView))
                    return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteCompute", "unordered resource is unavailable or not in UnorderedWrite state"));
                auto gpu = shaderHeap_->GetGPUDescriptorHandleForHeapStart();
                gpu.ptr += static_cast<UINT64>(DescriptorIndex(resourceView)) * shaderDescriptorIncrement_;
                commandList_->SetComputeRootDescriptorTable(parameter.rootParameterIndex, gpu);
            }
            else return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteCompute", "root parameter kind is unsupported"));
        }
        commandList_->Dispatch(command.threadGroupCountX, command.threadGroupCountY, command.threadGroupCountZ);
        return base::Result<void, runtime::RuntimeError>::Success();
    }

    base::Result<void, runtime::RuntimeError> ExecuteRaster(pkg::RasterCommandId id)
    {
        if (!id.IsValid() || id.value >= view_.RasterCommands().size())
            return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteRaster", "raster command ID is invalid"));
        const auto& command = view_.RasterCommands()[id.value];
        if (!command.executable.IsValid() || command.executable.value >= view_.Executables().size() ||
            !command.attachmentOperation.IsValid() || command.attachmentOperation.value >= view_.AttachmentOperations().size())
            return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteRaster", "raster command references are invalid"));
        const auto& executable = view_.Executables()[command.executable.value];
        const auto& attachment = view_.AttachmentOperations()[command.attachmentOperation.value];
        if (!pipelineStates_[command.executable.value] || !rootSignatures_[executable.bindingLayout.value])
            return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteRaster", "executable objects are not materialized"));
        if (command.vertexViewRange.count != 1 || command.colorAttachmentRange.count != 1 || command.indexView.IsValid() ||
            command.viewportId != 0 || command.scissorId != 0 || command.vertexCount == 0 || command.instanceCount == 0)
            return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteRaster", "raster command contract is unsupported"));
        if (attachment.colorLoad != pkg::AttachmentLoadOp::Clear || attachment.colorStore != pkg::AttachmentStoreOp::Store)
            return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteRaster", "color attachment operation is unsupported"));

        const auto& vertexView = view_.Views()[command.vertexViewRange.first];
        const auto& colorView = view_.Views()[command.colorAttachmentRange.first];
        if (vertexView.viewClass != pkg::ViewClass::VertexBuffer || colorView.viewClass != pkg::ViewClass::RenderTarget)
            return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteRaster", "command view classes are invalid"));
        const bool hasDepth = command.depthAttachment.IsValid();
        const pkg::ResourceViewArtifact* depthView = nullptr;
        if (hasDepth)
        {
            if (command.depthAttachment.value >= view_.Views().size())
                return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteRaster", "depth view is invalid"));
            depthView = &view_.Views()[command.depthAttachment.value];
            if (depthView->viewClass != pkg::ViewClass::DepthStencil ||
                attachment.depthLoad != pkg::AttachmentLoadOp::Clear || attachment.depthStore != pkg::AttachmentStoreOp::Store)
                return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteRaster", "depth attachment contract is invalid"));
        }
        else if (attachment.depthLoad != pkg::AttachmentLoadOp::Discard || attachment.depthStore != pkg::AttachmentStoreOp::Discard)
            return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteRaster", "absent depth attachment must use Discard operations"));

        const pkg::ResourceState vertexState{pkg::StateClass::Explicit, 0, static_cast<std::uint32_t>(pkg::ExplicitStateBits::VertexBuffer)};
        const pkg::ResourceState renderTargetState{pkg::StateClass::Explicit, 0, static_cast<std::uint32_t>(pkg::ExplicitStateBits::RenderTarget)};
        const pkg::ResourceState depthState{pkg::StateClass::Explicit, 0, static_cast<std::uint32_t>(pkg::ExplicitStateBits::DepthWrite)};
        if (TrackedState(vertexView) != vertexState || TrackedState(colorView) != renderTargetState)
            return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteRaster", "vertex or color resource is not in its Package-planned state"));
        if (depthView && TrackedState(*depthView) != depthState)
            return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteRaster", "depth resource is not in DepthWrite state"));
        ID3D12Resource* vertexResource = ResourceObject(vertexView);
        if (!vertexResource) return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteRaster", "vertex resource is unavailable"));

        D3D12_VERTEX_BUFFER_VIEW vbv{};
        vbv.BufferLocation = vertexResource->GetGPUVirtualAddress() + vertexView.byteOffset;
        vbv.SizeInBytes = static_cast<UINT>(vertexView.byteSize);
        vbv.StrideInBytes = vertexView.strideBytes;
        auto rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(currentBackBuffer_) * rtvIncrement_;
        D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
        if (depthView)
        {
            dsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
            dsv.ptr += static_cast<SIZE_T>(DescriptorIndex(*depthView)) * dsvIncrement_;
            commandList_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        }
        else commandList_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        commandList_->ClearRenderTargetView(rtv, attachment.clearColor.data(), 0, nullptr);
        if (depthView) commandList_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, attachment.clearDepth, static_cast<UINT8>(attachment.clearStencil), 0, nullptr);

        D3D12_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(std::max(1u, surface_->ClientWidth()));
        viewport.Height = static_cast<float>(std::max(1u, surface_->ClientHeight()));
        viewport.MinDepth = 0.0f; viewport.MaxDepth = 1.0f;
        D3D12_RECT scissor{0, 0, static_cast<LONG>(std::max(1u, surface_->ClientWidth())), static_cast<LONG>(std::max(1u, surface_->ClientHeight()))};
        commandList_->RSSetViewports(1, &viewport);
        commandList_->RSSetScissorRects(1, &scissor);
        commandList_->SetPipelineState(pipelineStates_[command.executable.value].Get());
        commandList_->SetGraphicsRootSignature(rootSignatures_[executable.bindingLayout.value].Get());
        const auto& layout = view_.BindingLayouts()[executable.bindingLayout.value];
        if (layout.descriptorRange.count != 0)
        {
            if (!shaderHeap_) return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteRaster", "shader descriptor heap is unavailable"));
            ID3D12DescriptorHeap* heaps[] = {shaderHeap_.Get()};
            commandList_->SetDescriptorHeaps(1, heaps);
        }
        const pkg::ResourceState pixelShaderReadState{
            pkg::StateClass::Explicit,
            0,
            static_cast<std::uint32_t>(pkg::ExplicitStateBits::PixelShaderRead)};
        for (std::uint32_t i = 0; i < layout.parameterRange.count; ++i)
        {
            const auto& parameter = view_.RootParameters()[layout.parameterRange.first + i];
            if (parameter.kind == pkg::RootParameterKind::ConstantBuffer)
            {
                if (!parameter.dynamicSlot.IsValid() || parameter.dynamicSlot.value >= dynamicApplied_.size() || !dynamicApplied_[parameter.dynamicSlot.value])
                    return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteRaster", "dynamic constant binding is invalid or was not applied"));
                const auto& slot = view_.DynamicSlots()[parameter.dynamicSlot.value];
                ID3D12Resource* resource = ResourceObject(slot.destinationResource);
                if (!resource) return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteRaster", "dynamic constant resource is unavailable"));
                commandList_->SetGraphicsRootConstantBufferView(parameter.rootParameterIndex, resource->GetGPUVirtualAddress() + slot.destinationOffset);
            }
            else if (parameter.kind == pkg::RootParameterKind::ShaderResourceTable)
            {
                if (!parameter.staticView.IsValid() || parameter.staticView.value >= view_.Views().size())
                    return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteRaster", "static shader-resource view is invalid"));
                const auto& resourceView = view_.Views()[parameter.staticView.value];
                if (resourceView.viewClass != pkg::ViewClass::ShaderResource || TrackedState(resourceView) != pixelShaderReadState || !ResourceObject(resourceView))
                    return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteRaster", "shader resource is unavailable or not in PixelShaderRead state"));
                auto gpu = shaderHeap_->GetGPUDescriptorHandleForHeapStart();
                gpu.ptr += static_cast<UINT64>(DescriptorIndex(resourceView)) * shaderDescriptorIncrement_;
                commandList_->SetGraphicsRootDescriptorTable(parameter.rootParameterIndex, gpu);
            }
            else return base::Result<void, runtime::RuntimeError>::Failure(Error("frame/ExecuteRaster", "root parameter kind is unsupported"));
        }
        commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList_->IASetVertexBuffers(0, 1, &vbv);
        commandList_->DrawInstanced(command.vertexCount, command.instanceCount, command.firstVertex, command.firstInstance);
        return base::Result<void, runtime::RuntimeError>::Success();
    }

    ID3D12Resource* ResourceObject(pkg::ResourceId id)
    {
        return ResourceObjectAt(id, PhysicalInstanceIndex(id));
    }

    base::Result<void, runtime::RuntimeError> SignalAndWait()
    {
        const auto value = nextFenceValue_++;
        HRESULT hr = queue_->Signal(fence_.Get(), value);
        if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("queue/signal", hr, device_.Get()));
        lastFenceValue_ = value;
        return WaitForFence(value);
    }

    base::Result<void, runtime::RuntimeError> WaitForFence(std::uint64_t value)
    {
        if (fence_->GetCompletedValue() >= value) return base::Result<void, runtime::RuntimeError>::Success();
        const HRESULT hr = fence_->SetEventOnCompletion(value, fenceEvent_);
        if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("queue/set-fence-event", hr, device_.Get()));
        WaitForSingleObject(fenceEvent_, INFINITE);
        return base::Result<void, runtime::RuntimeError>::Success();
    }

    base::Result<void, runtime::RuntimeError> WaitForCopyFence(std::uint64_t value)
    {
        if (copyFence_->GetCompletedValue() >= value) return base::Result<void, runtime::RuntimeError>::Success();
        const HRESULT hr = copyFence_->SetEventOnCompletion(value, copyFenceEvent_);
        if (FAILED(hr)) return base::Result<void, runtime::RuntimeError>::Failure(HResultError("copy-queue/set-fence-event", hr, device_.Get()));
        WaitForSingleObject(copyFenceEvent_, INFINITE);
        return base::Result<void, runtime::RuntimeError>::Success();
    }

    base::Result<void, runtime::RuntimeError> PackageFailure(std::string stage, const package::PackageError& error)
    {
        return base::Result<void, runtime::RuntimeError>::Failure(Error(std::move(stage), error.message));
    }

    struct PendingBufferVerification final
    {
        pkg::ResourceId resource;
        std::uint32_t instanceIndex = 0;
        std::uint64_t expectedDataOffset = 0;
        std::uint64_t bytes = 0;
        ComPtr<ID3D12Resource> readback;
    };

    struct PendingTextureVerification final
    {
        pkg::ResourceId resource;
        std::uint32_t instanceIndex = 0;
        std::uint64_t expectedDataOffset = 0;
        std::uint32_t expectedRowBytes = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        ComPtr<ID3D12Resource> readback;
    };

    std::shared_ptr<const package::FrozenExecutablePackage> package_;
    pkg::D3D12PackageView view_;
    runtime::ISurfaceHost* surface_ = nullptr;
    ExecutorOptions options_;
    std::uint64_t deviceEpoch_ = 1;
    runtime::DeviceRuntimeState runtimeState_ = runtime::DeviceRuntimeState::Active;
    LUID activeAdapterLuid_{};
    LUID excludedAdapterLuid_{};
    bool hasActiveAdapterLuid_ = false;
    bool hasExcludedAdapterLuid_ = false;

    ComPtr<IDXGIFactory6> factory_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12CommandQueue> computeQueue_;
    ComPtr<ID3D12CommandQueue> copyQueue_;
    ComPtr<IDXGISwapChain3> swapChain_;
    ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    ComPtr<ID3D12DescriptorHeap> dsvHeap_;
    ComPtr<ID3D12DescriptorHeap> shaderHeap_;
    std::vector<ComPtr<ID3D12Resource>> backBuffers_;
    std::vector<std::vector<ComPtr<ID3D12Resource>>> resources_;
    std::vector<std::shared_ptr<runtime::IExternalResource>> externalNativeResources_;
    std::vector<runtime::ExternalResourceBinding> externalBindings_;
    std::vector<bool> externalAcquired_;
    std::vector<bool> externalWaited_;
    std::vector<bool> externalReleased_;
    std::vector<std::uint32_t> externalConsumerQueues_;
    std::vector<bool> surfaceAcquired_;
    std::vector<bool> surfacePresented_;
    std::vector<bool> temporalWaitedResources_;
    std::vector<runtime::ExternalRelease> frameExternalReleases_;
    std::vector<std::vector<ComPtr<ID3D12Heap>>> placedHeaps_;
    std::vector<std::uint32_t> activeAliasResources_;
    std::vector<ComPtr<ID3D12Resource>> uploadResources_;
    std::vector<PendingBufferVerification> pendingBufferVerifications_;
    std::vector<PendingTextureVerification> pendingTextureVerifications_;
    std::vector<ComPtr<ID3D12RootSignature>> rootSignatures_;
    std::vector<ComPtr<ID3D12PipelineState>> pipelineStates_;
    std::vector<ComPtr<ID3D12PipelineState>> computePipelineStates_;
    std::vector<std::vector<pkg::ResourceState>> resourceStates_;
    std::vector<std::span<const std::byte>> dynamicBindings_;
    std::vector<bool> dynamicApplied_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> commandList_;
    ComPtr<ID3D12CommandAllocator> copyAllocator_;
    ComPtr<ID3D12GraphicsCommandList> copyCommandList_;
    ComPtr<ID3D12CommandAllocator> loadAllocator_;
    ComPtr<ID3D12GraphicsCommandList> loadCommandList_;
    std::vector<std::vector<ComPtr<ID3D12CommandAllocator>>> frameAllocators_;
    std::vector<std::vector<ComPtr<ID3D12GraphicsCommandList>>> frameCommandLists_;
    std::vector<std::vector<ComPtr<ID3D12CommandAllocator>>> computeFrameAllocators_;
    std::vector<std::vector<ComPtr<ID3D12GraphicsCommandList>>> computeFrameCommandLists_;
    std::vector<std::vector<ComPtr<ID3D12CommandAllocator>>> copyFrameAllocators_;
    std::vector<std::vector<ComPtr<ID3D12GraphicsCommandList>>> copyFrameCommandLists_;
    ComPtr<ID3D12Fence> fence_;
    ComPtr<ID3D12Fence> computeFence_;
    ComPtr<ID3D12Fence> copyFence_;
    HANDLE fenceEvent_ = nullptr;
    HANDLE computeFenceEvent_ = nullptr;
    HANDLE copyFenceEvent_ = nullptr;
    UINT rtvIncrement_ = 0;
    UINT dsvIncrement_ = 0;
    UINT shaderDescriptorIncrement_ = 0;
    UINT currentBackBuffer_ = 0;
    std::uint64_t nextFenceValue_ = 1;
    std::uint64_t lastFenceValue_ = 0;
    std::uint64_t computeNextFenceValue_ = 1;
    std::uint64_t computeLastFenceValue_ = 0;
    std::uint64_t copyNextFenceValue_ = 1;
    std::uint64_t copyLastFenceValue_ = 0;
    std::uint64_t frameFenceValue_ = 0;
    std::uint64_t computeFrameFenceValue_ = 0;
    std::uint64_t copyFrameFenceValue_ = 0;
    std::vector<std::uint64_t> frameSlotFenceValues_;
    std::vector<std::uint64_t> computeFrameSlotFenceValues_;
    std::vector<std::uint64_t> copyFrameSlotFenceValues_;
    std::uint32_t currentFrameSlot_ = 0;
    std::uint32_t temporalPreviousInstance_ = 1;
    std::uint32_t temporalCurrentInstance_ = 0;
    std::uint64_t temporalDependencyFenceValue_ = 0;
    std::uint64_t previousDirectFenceValue_ = 0;
    std::uint64_t previousComputeFenceValue_ = 0;
    std::uint64_t previousCopyFenceValue_ = 0;
    std::uint64_t lastSubmittedFrameNumber_ = 0;
    bool hasSubmittedFrame_ = false;
    bool descriptorHeapsCreated_ = false;
    bool commandOpen_ = false;
    std::uint32_t activeCommandQueue_ = package::InvalidIndex;
    bool directFrameSubmitted_ = false;
    bool computeFrameSubmitted_ = false;
    bool copyFrameCommandOpen_ = false;
    bool copyFrameSubmitted_ = false;
    std::uint32_t directFrameBatchCursor_ = 0;
    std::uint32_t computeFrameBatchCursor_ = 0;
    std::uint32_t copyFrameBatchCursor_ = 0;
    bool loadBatchOpen_ = false;
    bool loadBatchClosed_ = false;
    bool loadQueueCompleted_ = false;
};
}

base::Result<std::unique_ptr<runtime::IPackageInstance>, runtime::RuntimeError> D3D12Executor::Load(
    std::shared_ptr<const package::FrozenExecutablePackage> package,
    runtime::ISurfaceHost& surface)
{
    auto view = pkg::D3D12PackageView::Decode(*package);
    if (!view) return base::Result<std::unique_ptr<runtime::IPackageInstance>, runtime::RuntimeError>::Failure(Error("package/decode", view.Error().message));
    auto instance = std::make_unique<Instance>(std::move(package), std::move(view).Value(), surface, options_);
    auto initialized = instance->Initialize();
    if (!initialized) return base::Result<std::unique_ptr<runtime::IPackageInstance>, runtime::RuntimeError>::Failure(initialized.Error());
    return base::Result<std::unique_ptr<runtime::IPackageInstance>, runtime::RuntimeError>::Success(std::move(instance));
}

base::Result<runtime::FrameSubmission, runtime::RuntimeError> D3D12Executor::Submit(
    runtime::IPackageInstance& instance,
    const runtime::FrameInvocation& invocation)
{
    auto* d3dInstance = dynamic_cast<Instance*>(&instance);
    if (!d3dInstance) return base::Result<runtime::FrameSubmission, runtime::RuntimeError>::Failure(Error("submit", "package instance belongs to a different executor"));
    return d3dInstance->Submit(invocation);
}

base::Result<runtime::DeviceRecoveryReport, runtime::RuntimeError> D3D12Executor::RecoverDevice(
    runtime::IPackageInstance& instance,
    runtime::DeviceRecoveryMode mode)
{
    auto* d3dInstance = dynamic_cast<Instance*>(&instance);
    if (!d3dInstance) return base::Result<runtime::DeviceRecoveryReport, runtime::RuntimeError>::Failure(
        Error("recovery", "package instance belongs to a different executor"));
    return d3dInstance->RecoverDevice(mode);
}

base::Result<ExternalBufferBinding, runtime::RuntimeError> D3D12Executor::CreateExternalColorBuffer(
    runtime::IPackageInstance& instance,
    const std::array<float, 4>& color)
{
    auto* d3dInstance = dynamic_cast<Instance*>(&instance);
    if (!d3dInstance) return base::Result<ExternalBufferBinding, runtime::RuntimeError>::Failure(Error("external", "package instance belongs to a different executor"));
    return d3dInstance->CreateExternalColorBuffer(color);
}

bool D3D12Executor::SupportsOperation(pkg::D3D12OperationCode code) noexcept
{
    switch (code)
    {
    case pkg::D3D12OperationCode::CreateDescriptorHeaps:
    case pkg::D3D12OperationCode::CreateResource:
    case pkg::D3D12OperationCode::UploadBuffer:
    case pkg::D3D12OperationCode::CreateRootSignature:
    case pkg::D3D12OperationCode::CreateGraphicsPipeline:
    case pkg::D3D12OperationCode::CreateComputePipeline:
    case pkg::D3D12OperationCode::InitializeState:
    case pkg::D3D12OperationCode::VerifyBufferContents:
    case pkg::D3D12OperationCode::UploadTexture:
    case pkg::D3D12OperationCode::VerifyTextureContents:
    case pkg::D3D12OperationCode::AcquireSurfaceImage:
    case pkg::D3D12OperationCode::AcquireExternal:
    case pkg::D3D12OperationCode::WaitExternal:
    case pkg::D3D12OperationCode::ApplyDynamicData:
    case pkg::D3D12OperationCode::BeginQueueBatch:
    case pkg::D3D12OperationCode::Transition:
    case pkg::D3D12OperationCode::ActivateAlias:
    case pkg::D3D12OperationCode::ExecuteRaster:
    case pkg::D3D12OperationCode::ExecuteCompute:
    case pkg::D3D12OperationCode::ExecuteCopy:
    case pkg::D3D12OperationCode::EndQueueBatch:
    case pkg::D3D12OperationCode::SignalQueue:
    case pkg::D3D12OperationCode::WaitQueue:
    case pkg::D3D12OperationCode::WaitTemporal:
    case pkg::D3D12OperationCode::ReleaseExternal:
    case pkg::D3D12OperationCode::PresentSurface:
        return true;
    default:
        return false;
    }
}
}
