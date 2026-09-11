#pragma once

#include "dui/rendering.hpp"

#include <cstdint>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <span>
#include <string_view>

namespace dui {

using NativeViewId = std::uint64_t;

struct ViewMetrics {
    Size physical_size;
    double device_pixel_ratio{1.0};

    friend constexpr bool operator==(ViewMetrics, ViewMetrics) = default;
};

enum class SurfacePixelFormat {
    rgba8888_srgb_premultiplied
};

struct SurfaceRequest {
    Size logical_size;
    std::uint32_t physical_width{};
    std::uint32_t physical_height{};
    double device_pixel_ratio{1.0};
    std::uint64_t generation{};

    [[nodiscard]] bool valid() const;
};

struct SurfaceDescriptor {
    std::uint32_t width{};
    std::uint32_t height{};
    std::size_t row_bytes{};
    SurfacePixelFormat format{SurfacePixelFormat::rgba8888_srgb_premultiplied};
    std::uint64_t generation{};
};

class SurfaceFrameDelegate {
public:
    virtual ~SurfaceFrameDelegate() = default;
    [[nodiscard]] virtual std::span<std::byte> pixels() = 0;
    enum class PresentStatus {
        presented,
        out_of_date,
        lost
    };
    [[nodiscard]] virtual PresentStatus present() noexcept = 0;
    virtual void abandon() noexcept = 0;
};

class SurfaceFrame final {
public:
    // Construction maps once and validates exactly row_bytes * height bytes.
    // The returned pixel span is valid only while this transaction is active.
    SurfaceFrame(SurfaceDescriptor descriptor, std::unique_ptr<SurfaceFrameDelegate> delegate);
    ~SurfaceFrame();

    SurfaceFrame(const SurfaceFrame&) = delete;
    SurfaceFrame& operator=(const SurfaceFrame&) = delete;

    [[nodiscard]] const SurfaceDescriptor& descriptor() const { return descriptor_; }
    [[nodiscard]] std::span<std::byte> pixels();
    [[nodiscard]] bool active() const { return active_; }
    // Present and abandon are exactly-once terminal operations. A throwing
    // delegate present remains terminal and is not followed by abandon.
    [[nodiscard]] SurfaceFrameDelegate::PresentStatus present();
    void abandon() noexcept;

private:
    SurfaceDescriptor descriptor_;
    std::unique_ptr<SurfaceFrameDelegate> delegate_;
    std::span<std::byte> pixels_;
    bool active_{true};
};

enum class SurfaceAcquireStatus {
    ready,
    unavailable,
    out_of_date,
    lost,
    consumed
};

class SurfaceAcquisition final {
public:
    // Move and take consume the source. `consumed` is observational state and
    // cannot be returned by RasterSurface as an acquisition failure.
    SurfaceAcquisition(const SurfaceAcquisition&) = delete;
    SurfaceAcquisition& operator=(const SurfaceAcquisition&) = delete;
    SurfaceAcquisition(SurfaceAcquisition&& other) noexcept;
    SurfaceAcquisition& operator=(SurfaceAcquisition&& other) noexcept;

    [[nodiscard]] static SurfaceAcquisition ready(std::unique_ptr<SurfaceFrame> frame);
    [[nodiscard]] static SurfaceAcquisition failed(SurfaceAcquireStatus status);

    [[nodiscard]] SurfaceAcquireStatus status() const { return status_; }
    [[nodiscard]] const SurfaceFrame* frame() const { return frame_.get(); }
    [[nodiscard]] std::unique_ptr<SurfaceFrame> take_frame();

private:
    SurfaceAcquisition(SurfaceAcquireStatus status, std::unique_ptr<SurfaceFrame> frame) :
        status_(status),
        frame_(std::move(frame)) {}

    SurfaceAcquireStatus status_;
    std::unique_ptr<SurfaceFrame> frame_;
};

class RasterSurface {
public:
    // RasterSurface is a thread-safe platform bridge whose lifetime is not
    // thread-affine. Acquired frame delegates are used only on the raster thread.
    virtual ~RasterSurface() = default;
    // Validates requests and enforces that ready frames match physical extent
    // and generation. Zero-sized requests are valid but cannot produce a ready
    // SurfaceFrame. Physical extent may differ from logical_size * DPR by at
    // most one pixel to permit platform rounding.
    [[nodiscard]] SurfaceAcquisition acquire(const SurfaceRequest& request);

protected:
    // Return one matching active frame, or a frame-less unavailable,
    // out_of_date, or lost result.
    [[nodiscard]] virtual SurfaceAcquisition do_acquire(const SurfaceRequest& request) = 0;
};

class NativeView {
public:
    virtual ~NativeView() = default;

    [[nodiscard]] virtual NativeViewId id() const = 0;
    [[nodiscard]] virtual ViewMetrics metrics() const = 0;
    virtual void present(DisplayList display_list) = 0;
};

class Backend {
public:
    virtual ~Backend() = default;

    virtual void request_frame(NativeViewId view) = 0;
    virtual void post_task(std::function<void()> task) = 0;
    virtual void set_title(NativeViewId view, std::string_view title) = 0;
};

class Renderer {
public:
    virtual ~Renderer() = default;
    virtual void render(NativeView& view, LayerTree layer_tree) = 0;
};

class DisplayListRenderer final : public Renderer {
public:
    void render(NativeView& view, LayerTree layer_tree) override {
        view.present(layer_tree.flatten());
    }
};

class SurfaceRenderer {
public:
    virtual ~SurfaceRenderer() = default;
    virtual void render(
        SurfaceFrame& frame,
        const LayerTree& layer_tree,
        const SurfaceRequest& request
    ) = 0;
};

class RasterThread final {
public:
    using FrameId = std::uint64_t;
    using RendererFactory = std::function<std::unique_ptr<SurfaceRenderer>()>;

    enum class FrameOutcome {
        presented,
        unavailable,
        out_of_date,
        canceled,
        failed
    };

    class FrameTicket final {
    public:
        [[nodiscard]] FrameId id() const { return id_; }
        // Copyable shared completion: repeated and concurrent waits observe the
        // same outcome or terminal exception.
        [[nodiscard]] FrameOutcome wait() const { return completion_.get(); }

    private:
        friend class RasterThread;
        FrameTicket(FrameId id, std::shared_future<FrameOutcome> completion) :
            id_(id),
            completion_(std::move(completion)) {}

        FrameId id_{};
        std::shared_future<FrameOutcome> completion_;
    };

    // Member calls may be concurrent, but destruction must be externally
    // serialized. The worker exclusively owns SurfaceRenderer execution and
    // retains shared ownership of RasterSurface.
    RasterThread(
        std::shared_ptr<RasterSurface> surface,
        RendererFactory renderer_factory
    );
    ~RasterThread();

    RasterThread(const RasterThread&) = delete;
    RasterThread& operator=(const RasterThread&) = delete;

    [[nodiscard]] FrameTicket submit(LayerTree layer_tree, SurfaceRequest request);
    // Non-blocking and callback-safe. Queued frames are canceled, the active
    // render may finish, and subsequent submissions are rejected.
    void request_stop();
    // Renderer failure is terminal and is rethrown here, by the affected
    // FrameTickets, and by later submit. Calling from render is rejected.
    void wait_idle();
    // Includes queued frames and the frame currently being rendered.
    [[nodiscard]] std::size_t pending_count() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dui
