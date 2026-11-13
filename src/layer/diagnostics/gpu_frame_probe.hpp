#pragma once
#include <array>
#include <cstdint>
#include <d3d11.h>

namespace dvr::layer::diagnostics {
// Optional bounded pipeline-statistics capture. Query results are read later
// without flushing or waiting for the GPU. Each control change captures 24 frames.
class GpuFrameProbe {
public:
    using Logger = void (*)(const char*, ...);
    explicit GpuFrameProbe(Logger logger) : logger_(logger) {}
    ~GpuFrameProbe() {
        for (auto& slot : slots_)
            if (slot.query) slot.query->Release();
        if (context_) context_->Release();
    }
    void arm(uint32_t mode, uint32_t uniform) {
        mode_ = mode;
        uniform_ = uniform;
        remaining_ = 24;
    }
    void frame(ID3D11DeviceContext* context) {
        if (!context || failed_) return;
        if (context_ && context != context_) return;
        if (!context_ && !initialize(context)) return;
        if (active_) {
            context_->End(active_->query);
            active_->pending = true;
            active_ = nullptr;
        }
        for (auto& slot : slots_) {
            if (!slot.pending) continue;
            D3D11_QUERY_DATA_PIPELINE_STATISTICS data{};
            const auto result = context_->GetData(slot.query, &data, sizeof(data), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (result == S_FALSE) continue;
            slot.pending = false;
            if (FAILED(result)) {
                logger_("stage=gpu-frame-probe result=read-failed hresult=0x%08X", static_cast<unsigned>(result));
                continue;
            }
            logger_("stage=gpu-frame-probe result=sample mode=%u uniform=%u sample=%llu ps=%llu vs=%llu "
                    "primitives=%llu cs=%llu",
                    slot.mode, slot.uniform, static_cast<unsigned long long>(slot.sequence),
                    static_cast<unsigned long long>(data.PSInvocations),
                    static_cast<unsigned long long>(data.VSInvocations),
                    static_cast<unsigned long long>(data.IAPrimitives),
                    static_cast<unsigned long long>(data.CSInvocations));
        }
        if (!remaining_) return;
        for (auto& slot : slots_) {
            if (slot.pending) continue;
            slot.mode = mode_;
            slot.uniform = uniform_;
            slot.sequence = ++sequence_;
            context_->Begin(slot.query);
            active_ = &slot;
            --remaining_;
            break;
        }
    }

private:
    struct Slot {
        ID3D11Query* query = nullptr;
        bool pending = false;
        uint32_t mode = 0, uniform = 0;
        uint64_t sequence = 0;
    };
    bool initialize(ID3D11DeviceContext* context) {
        ID3D11Device* device = nullptr;
        context->GetDevice(&device);
        if (!device) return false;
        D3D11_QUERY_DESC description{D3D11_QUERY_PIPELINE_STATISTICS, 0};
        for (auto& slot : slots_) {
            const auto result = device->CreateQuery(&description, &slot.query);
            if (FAILED(result)) {
                logger_("stage=gpu-frame-probe result=create-failed hresult=0x%08X", static_cast<unsigned>(result));
                failed_ = true;
                device->Release();
                return false;
            }
        }
        device->Release();
        context_ = context;
        context_->AddRef();
        return true;
    }
    Logger logger_;
    ID3D11DeviceContext* context_ = nullptr;
    std::array<Slot, 8> slots_{};
    Slot* active_ = nullptr;
    uint32_t mode_ = 0, uniform_ = 0, remaining_ = 0;
    uint64_t sequence_ = 0;
    bool failed_ = false;
};
} // namespace dvr::layer::diagnostics
