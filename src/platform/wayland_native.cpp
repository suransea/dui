#include "dui/platform/wayland_native.hpp"

#include "dui/platform/wayland.hpp"

#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <wayland-client.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <linux/memfd.h>
#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace dui::platform {

namespace {

enum class GlobalKind { compositor, shm, seat, wm_base, viewporter, fractional_scale };

struct GlobalOffer {
  std::uint32_t name{};
  std::uint32_t version{};
  GlobalKind kind{};
};

class WaylandTaskRunner final : public TaskRunner {
public:
  WaylandTaskRunner() : owner_(std::this_thread::get_id()) {}

  void post(Task task) override {
    std::lock_guard lock{mutex_};
    tasks_.push_back(std::move(task));
  }

  [[nodiscard]] bool runs_tasks_on_current_thread() const noexcept override {
    return std::this_thread::get_id() == owner_;
  }

  void run_pending() {
    if (!runs_tasks_on_current_thread()) {
      throw std::logic_error("Wayland tasks run only on the connection owner thread");
    }
    while (true) {
      Task task;
      {
        std::lock_guard lock{mutex_};
        if (tasks_.empty()) {
          return;
        }
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      task();
    }
  }

  void drain_shutdown_batch() noexcept {
    std::size_t remaining{};
    {
      std::lock_guard lock{mutex_};
      remaining = tasks_.size();
    }
    while (remaining-- != 0) {
      Task task;
      {
        std::lock_guard lock{mutex_};
        if (tasks_.empty()) {
          return;
        }
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      try {
        task();
      } catch (...) {
        // Teardown must continue until the host control is retired.
      }
    }
  }

private:
  std::thread::id owner_;
  std::mutex mutex_;
  std::deque<Task> tasks_;
};

std::optional<GlobalKind> global_kind(const char* interface) noexcept {
  if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
    return GlobalKind::compositor;
  }
  if (std::strcmp(interface, wl_shm_interface.name) == 0) {
    return GlobalKind::shm;
  }
  if (std::strcmp(interface, wl_seat_interface.name) == 0) {
    return GlobalKind::seat;
  }
  if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
    return GlobalKind::wm_base;
  }
  if (std::strcmp(interface, wp_viewporter_interface.name) == 0) {
    return GlobalKind::viewporter;
  }
  if (std::strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
    return GlobalKind::fractional_scale;
  }
  return std::nullopt;
}

const char* interface_name(GlobalKind kind) noexcept {
  switch (kind) {
  case GlobalKind::compositor:
    return wl_compositor_interface.name;
  case GlobalKind::shm:
    return wl_shm_interface.name;
  case GlobalKind::seat:
    return wl_seat_interface.name;
  case GlobalKind::wm_base:
    return xdg_wm_base_interface.name;
  case GlobalKind::viewporter:
    return wp_viewporter_interface.name;
  case GlobalKind::fractional_scale:
    return wp_fractional_scale_manager_v1_interface.name;
  }
  std::terminate();
}

} // namespace

struct WaylandConnection::Impl {
  struct Output {
    Impl* owner{};
    wl_output* proxy{};
    std::uint32_t name{};
    std::uint32_t version{};
    std::uint32_t scale{1};

    ~Output() {
      if (proxy == nullptr) {
        return;
      }
      if (version >= WL_OUTPUT_RELEASE_SINCE_VERSION) {
        wl_output_release(proxy);
      } else {
        wl_output_destroy(proxy);
      }
    }

    static void handle_geometry(void*, wl_output*, std::int32_t, std::int32_t, std::int32_t,
                                std::int32_t, std::int32_t, const char*, const char*,
                                std::int32_t) noexcept {}
    static void handle_mode(void*, wl_output*, std::uint32_t, std::int32_t, std::int32_t,
                            std::int32_t) noexcept {}
    static void handle_done(void*, wl_output*) noexcept {}
    static void handle_scale(void* data, wl_output*, std::int32_t factor) noexcept {
      auto& self = *static_cast<Output*>(data);
      if (factor <= 0 ||
          static_cast<std::uint32_t>(factor) >
            std::numeric_limits<std::uint32_t>::max() / WaylandSurfaceState::scale_denominator) {
        self.owner->callback_failed = true;
        return;
      }
      self.scale = static_cast<std::uint32_t>(factor);
      self.owner->notify_output(self.proxy, false);
    }
    static void handle_name(void*, wl_output*, const char*) noexcept {}
    static void handle_description(void*, wl_output*, const char*) noexcept {}
  };

  wl_display* display{};
  wl_registry* registry{};
  wl_compositor* compositor{};
  wl_shm* shm{};
  wl_seat* seat{};
  xdg_wm_base* wm_base{};
  wp_viewporter* viewporter{};
  wp_fractional_scale_manager_v1* fractional_scale{};
  WaylandGlobals globals;
  std::thread::id owner{std::this_thread::get_id()};
  std::uint32_t compositor_name{};
  std::uint32_t shm_name{};
  std::uint32_t seat_name{};
  std::uint32_t wm_base_name{};
  std::uint32_t viewporter_name{};
  std::uint32_t fractional_scale_name{};
  std::vector<GlobalOffer> offers;
  bool callback_failed{};
  std::shared_ptr<WaylandTaskRunner> runner{std::make_shared<WaylandTaskRunner>()};
  std::size_t active_windows{};
  void* active_window{};
  void (*fail_active_window)(void*) noexcept {};
  std::array<std::uint32_t, 6> deferred_removals{};
  std::vector<std::unique_ptr<Output>> outputs;
  bool transport_failed{};
  void (*active_output_changed)(void*, wl_output*, bool) noexcept {};
  void (*active_seat_changed)(void*, std::uint32_t) noexcept {};
  bool active_pointer_child{};

  ~Impl() {
    if (std::this_thread::get_id() != owner || active_windows != 0) {
      std::terminate();
    }
    outputs.clear();
    if (fractional_scale != nullptr) {
      wp_fractional_scale_manager_v1_destroy(fractional_scale);
    }
    if (viewporter != nullptr) {
      wp_viewporter_destroy(viewporter);
    }
    if (seat != nullptr) {
      if (globals.seat_version >= WL_SEAT_RELEASE_SINCE_VERSION) {
        wl_seat_release(seat);
      } else {
        wl_seat_destroy(seat);
      }
    }
    if (wm_base != nullptr) {
      xdg_wm_base_destroy(wm_base);
    }
    if (shm != nullptr) {
      wl_shm_destroy(shm);
    }
    if (compositor != nullptr) {
      wl_compositor_destroy(compositor);
    }
    if (registry != nullptr) {
      wl_registry_destroy(registry);
    }
    if (display != nullptr) {
      wl_display_disconnect(display);
    }
  }

  void require_owner() const {
    if (std::this_thread::get_id() != owner) {
      throw std::logic_error("Wayland connection used from a non-owner thread");
    }
  }

  void fail_transport() noexcept {
    if (transport_failed) {
      return;
    }
    transport_failed = true;
    if (active_window != nullptr && fail_active_window != nullptr) {
      fail_active_window(active_window);
    }
  }

  void notify_output(wl_output* output, bool removed) noexcept {
    if (active_window != nullptr && active_output_changed != nullptr) {
      active_output_changed(active_window, output, removed);
    }
  }

  void notify_seat(std::uint32_t capabilities) noexcept {
    globals.seat_capabilities = capabilities;
    if (active_window != nullptr && active_seat_changed != nullptr) {
      active_seat_changed(active_window, capabilities);
    }
  }

  void bind_output(std::uint32_t name, std::uint32_t version) noexcept {
    try {
      auto output = std::make_unique<Output>();
      output->owner = this;
      output->name = name;
      output->version = std::min(version, 4U);
      output->proxy = static_cast<wl_output*>(
        wl_registry_bind(registry, name, &wl_output_interface, output->version));
      if (output->proxy == nullptr) {
        callback_failed = true;
        return;
      }
      static constexpr wl_output_listener listener{
        Output::handle_geometry, Output::handle_mode, Output::handle_done,
        Output::handle_scale,    Output::handle_name, Output::handle_description};
      if (wl_output_add_listener(output->proxy, &listener, output.get()) != 0) {
        callback_failed = true;
        return;
      }
      outputs.push_back(std::move(output));
      globals.output_count = static_cast<std::uint32_t>(outputs.size());
    } catch (...) {
      callback_failed = true;
    }
  }

  std::uint32_t output_scale(wl_output* proxy) const noexcept {
    const auto output =
      std::ranges::find(outputs, proxy, [](const auto& value) { return value->proxy; });
    return output == outputs.end() ? 1U : (*output)->scale;
  }

  void bind(std::uint32_t name, const char* interface, std::uint32_t version) noexcept {
    if (version == 0) {
      return;
    }
    if (std::strcmp(interface, wl_compositor_interface.name) == 0 && compositor == nullptr) {
      const std::uint32_t negotiated = std::min(version, 4U);
      compositor = static_cast<wl_compositor*>(
        wl_registry_bind(registry, name, &wl_compositor_interface, negotiated));
      if (compositor != nullptr) {
        globals.compositor_version = negotiated;
        compositor_name = name;
      } else {
        callback_failed = true;
      }
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0 && shm == nullptr) {
      shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
      if (shm != nullptr) {
        globals.shm_version = 1;
        shm_name = name;
      } else {
        callback_failed = true;
      }
    } else if (std::strcmp(interface, wl_seat_interface.name) == 0 && seat == nullptr) {
      const std::uint32_t negotiated = std::min(version, 5U);
      seat =
        static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, negotiated));
      if (seat != nullptr) {
        static constexpr wl_seat_listener listener{handle_seat_capabilities, handle_seat_name};
        if (wl_seat_add_listener(seat, &listener, this) == 0) {
          globals.seat_version = negotiated;
          seat_name = name;
        } else {
          if (negotiated >= WL_SEAT_RELEASE_SINCE_VERSION) {
            wl_seat_release(seat);
          } else {
            wl_seat_destroy(seat);
          }
          seat = nullptr;
          callback_failed = true;
        }
      } else {
        callback_failed = true;
      }
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0 && wm_base == nullptr) {
      const std::uint32_t negotiated = std::min(version, 6U);
      wm_base = static_cast<xdg_wm_base*>(
        wl_registry_bind(registry, name, &xdg_wm_base_interface, negotiated));
      if (wm_base != nullptr) {
        static constexpr xdg_wm_base_listener listener{handle_ping};
        if (xdg_wm_base_add_listener(wm_base, &listener, this) == 0) {
          globals.xdg_wm_base_version = negotiated;
          wm_base_name = name;
        } else {
          xdg_wm_base_destroy(wm_base);
          wm_base = nullptr;
          callback_failed = true;
        }
      } else {
        callback_failed = true;
      }
    } else if (std::strcmp(interface, wp_viewporter_interface.name) == 0 && viewporter == nullptr) {
      const std::uint32_t negotiated = std::min(version, 1U);
      viewporter = static_cast<wp_viewporter*>(
        wl_registry_bind(registry, name, &wp_viewporter_interface, negotiated));
      if (viewporter != nullptr) {
        globals.viewporter_version = negotiated;
        viewporter_name = name;
      } else {
        callback_failed = true;
      }
    } else if (std::strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0 &&
               fractional_scale == nullptr) {
      const std::uint32_t negotiated = std::min(version, 1U);
      fractional_scale = static_cast<wp_fractional_scale_manager_v1*>(
        wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, negotiated));
      if (fractional_scale != nullptr) {
        globals.fractional_scale_version = negotiated;
        fractional_scale_name = name;
      } else {
        callback_failed = true;
      }
    }
  }

  void advertise(std::uint32_t name, const char* interface, std::uint32_t version) noexcept {
    if (std::strcmp(interface, wl_output_interface.name) == 0 && version != 0) {
      bind_output(name, version);
      return;
    }
    const std::optional<GlobalKind> kind = global_kind(interface);
    if (!kind.has_value() || version == 0) {
      return;
    }
    try {
      offers.push_back({name, version, *kind});
    } catch (...) {
      callback_failed = true;
      return;
    }
    bind(name, interface, version);
  }

  void remove(std::uint32_t name) noexcept {
    const auto output =
      std::ranges::find(outputs, name, [](const auto& value) { return value->name; });
    if (output != outputs.end()) {
      notify_output((*output)->proxy, true);
      outputs.erase(output);
      globals.output_count = static_cast<std::uint32_t>(outputs.size());
      return;
    }
    const auto offer = std::ranges::find(offers, name, &GlobalOffer::name);
    if (offer == offers.end()) {
      return;
    }
    const GlobalKind kind = offer->kind;
    const bool selected =
      (kind == GlobalKind::fractional_scale && name == fractional_scale_name &&
       fractional_scale != nullptr) ||
      (kind == GlobalKind::viewporter && name == viewporter_name && viewporter != nullptr) ||
      (kind == GlobalKind::seat && name == seat_name && seat != nullptr) ||
      (kind == GlobalKind::wm_base && name == wm_base_name && wm_base != nullptr) ||
      (kind == GlobalKind::shm && name == shm_name && shm != nullptr) ||
      (kind == GlobalKind::compositor && name == compositor_name && compositor != nullptr);
    const bool has_active_children =
      kind == GlobalKind::compositor || kind == GlobalKind::shm ||
      (kind == GlobalKind::seat && active_pointer_child) || kind == GlobalKind::wm_base ||
      kind == GlobalKind::viewporter || kind == GlobalKind::fractional_scale;
    if (selected && active_windows != 0 && has_active_children) {
      deferred_removals[static_cast<std::size_t>(kind)] = name;
      return;
    }
    if (selected && kind == GlobalKind::seat) {
      notify_seat(0);
    }
    if (name == fractional_scale_name && fractional_scale != nullptr) {
      wp_fractional_scale_manager_v1_destroy(fractional_scale);
      fractional_scale = nullptr;
      globals.fractional_scale_version = 0;
    } else if (name == viewporter_name && viewporter != nullptr) {
      wp_viewporter_destroy(viewporter);
      viewporter = nullptr;
      globals.viewporter_version = 0;
    } else if (name == seat_name && seat != nullptr) {
      if (globals.seat_version >= WL_SEAT_RELEASE_SINCE_VERSION) {
        wl_seat_release(seat);
      } else {
        wl_seat_destroy(seat);
      }
      seat = nullptr;
      globals.seat_version = 0;
    } else if (name == wm_base_name && wm_base != nullptr) {
      xdg_wm_base_destroy(wm_base);
      wm_base = nullptr;
      globals.xdg_wm_base_version = 0;
    } else if (name == shm_name && shm != nullptr) {
      wl_shm_destroy(shm);
      shm = nullptr;
      globals.shm_version = 0;
    } else if (name == compositor_name && compositor != nullptr) {
      wl_compositor_destroy(compositor);
      compositor = nullptr;
      globals.compositor_version = 0;
    }
    offers.erase(offer);
    if (selected) {
      const auto replacement = std::ranges::find(offers, kind, &GlobalOffer::kind);
      if (replacement != offers.end()) {
        bind(replacement->name, interface_name(kind), replacement->version);
      }
    }
  }

  static void handle_global(void* data, wl_registry*, std::uint32_t name, const char* interface,
                            std::uint32_t version) noexcept {
    static_cast<Impl*>(data)->advertise(name, interface, version);
  }

  static void handle_global_remove(void* data, wl_registry*, std::uint32_t name) noexcept {
    static_cast<Impl*>(data)->remove(name);
  }

  static void handle_ping(void*, xdg_wm_base* wm_base, std::uint32_t serial) noexcept {
    xdg_wm_base_pong(wm_base, serial);
  }

  static void handle_seat_capabilities(void* data, wl_seat*, std::uint32_t capabilities) noexcept {
    static_cast<Impl*>(data)->notify_seat(capabilities);
  }

  static void handle_seat_name(void*, wl_seat*, const char*) noexcept {}
};

struct WaylandWindow::Impl {
  struct Buffer {
    wl_buffer* buffer{};
    void* mapping{MAP_FAILED};
    std::size_t size{};
    bool released{};

    ~Buffer() {
      if (buffer != nullptr) {
        wl_buffer_destroy(buffer);
      }
      if (mapping != MAP_FAILED) {
        munmap(mapping, size);
      }
    }

    static void handle_release(void* data, wl_buffer* buffer) noexcept {
      auto& self = *static_cast<Buffer*>(data);
      wl_buffer_destroy(buffer);
      self.buffer = nullptr;
      if (self.mapping != MAP_FAILED) {
        munmap(self.mapping, self.size);
        self.mapping = MAP_FAILED;
      }
      self.released = true;
    }
  };

  struct Frame {
    Impl* owner{};
    wl_callback* callback{};
    std::uint64_t state_generation{};
    std::uint64_t host_generation{};

    ~Frame() {
      if (callback != nullptr) {
        wl_callback_destroy(callback);
      }
    }

    static void handle_done(void* data, wl_callback* callback,
                            std::uint32_t milliseconds) noexcept {
      auto& self = *static_cast<Frame*>(data);
      Impl* owner = self.owner;
      const std::uint64_t state_generation = self.state_generation;
      const std::uint64_t host_generation = self.host_generation;
      wl_callback_destroy(callback);
      self.callback = nullptr;
      owner->frame.reset();
      if (!owner->native_running || !owner->surface_state.frame_done(state_generation)) {
        return;
      }
      if (milliseconds < owner->last_frame_milliseconds &&
          owner->last_frame_milliseconds - milliseconds >
            std::numeric_limits<std::uint32_t>::max() / 2U) {
        owner->frame_millisecond_epoch += std::uint64_t{1} << 32U;
      }
      owner->last_frame_milliseconds = milliseconds;
      const std::uint64_t unwrapped = owner->frame_millisecond_epoch + milliseconds;
      owner->endpoints.driver.frame_pulse(
        std::chrono::milliseconds{static_cast<std::int64_t>(unwrapped)}, host_generation);
      try {
        owner->present();
      } catch (...) {
        owner->fail_native();
      }
    }
  };

  struct Control final : HostWindowControl {
    explicit Control(Impl& owner) : owner_(&owner) {}

    void request_frame(std::uint64_t generation) override {
      Impl& owner = require_owner();
      owner.host_frame_generation = generation;
      owner.surface_state.request_frame();
      try {
        owner.present();
      } catch (...) {
        owner.fail_native();
        throw;
      }
    }

    void cancel_frame() override {
      if (owner_ != nullptr && owner_->frame != nullptr) {
        const std::uint64_t generation = owner_->frame->state_generation;
        owner_->frame.reset();
        static_cast<void>(owner_->surface_state.frame_done(generation));
      }
    }

    void set_title(std::string_view title) override {
      Impl& owner = require_owner();
      if (!owner.native_running) {
        throw std::logic_error("Wayland window is stopped");
      }
      const std::string owned_title{title};
      xdg_toplevel_set_title(owner.toplevel, owned_title.c_str());
    }

    void request_close() override {
      if (owner_ != nullptr) {
        owner_->endpoints.driver.request_framework_close();
      }
    }
    void shutdown() override {
      if (owner_ != nullptr) {
        owner_->shutdown_native();
      }
    }

    void detach() noexcept { owner_ = nullptr; }

  private:
    Impl& require_owner() const {
      if (owner_ == nullptr) {
        throw std::logic_error("Wayland window is stopped");
      }
      return *owner_;
    }

    Impl* owner_;
  };

  WaylandConnection::Impl* connection{};
  WaylandSurfaceState surface_state;
  wl_surface* surface{};
  xdg_surface* shell_surface{};
  xdg_toplevel* toplevel{};
  wp_viewport* viewport{};
  wp_fractional_scale_v1* fractional_scale{};
  wl_pointer* pointer{};
  std::shared_ptr<Control> control;
  HostWindowEndpoints endpoints;
  std::vector<std::unique_ptr<Buffer>> buffers;
  std::unique_ptr<Frame> frame;
  std::uint64_t host_frame_generation{};
  std::uint64_t committed_buffers{};
  std::uint64_t frame_millisecond_epoch{};
  std::uint32_t last_frame_milliseconds{};
  WaylandExtent committed_extent{};
  std::uint32_t committed_scale{WaylandSurfaceState::scale_denominator};
  bool committed_with_viewporter{};
  std::vector<wl_output*> entered_outputs;
  WaylandPointerState pointer_state;
  bool native_running{true};
  bool registered{};

  Impl(WaylandConnection::Impl& connection_value, WaylandExtent initial_extent)
    : connection(&connection_value),
      surface_state(initial_extent, connection_value.viewporter != nullptr &&
                                      connection_value.fractional_scale != nullptr) {}

  ~Impl() {
    if (std::this_thread::get_id() != connection->owner) {
      std::terminate();
    }
    endpoints.window.shutdown();
    if (control != nullptr) {
      control->detach();
    }
    connection->runner->drain_shutdown_batch();
    shutdown_native();
    if (registered) {
      --connection->active_windows;
      connection->active_window = nullptr;
      connection->fail_active_window = nullptr;
      connection->active_output_changed = nullptr;
      connection->active_seat_changed = nullptr;
      if (!connection->transport_failed) {
        for (std::uint32_t& name : connection->deferred_removals) {
          if (name != 0) {
            const std::uint32_t deferred = std::exchange(name, 0);
            connection->remove(deferred);
          }
        }
      }
    }
  }

  std::unique_ptr<Buffer> create_buffer(WaylandExtent extent, std::uint64_t generation) {
    const std::uint64_t stride = static_cast<std::uint64_t>(extent.width) * 4U;
    const std::uint64_t size = stride * extent.height;
    if (stride > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) ||
        size > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
      throw std::overflow_error("Wayland shared-memory buffer exceeds protocol range");
    }
    const long descriptor = syscall(SYS_memfd_create, "dui-wayland-buffer", MFD_CLOEXEC);
    if (descriptor < 0 || descriptor > std::numeric_limits<int>::max()) {
      throw std::runtime_error("Could not create Wayland shared-memory file");
    }
    const int fd = static_cast<int>(descriptor);
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
      close(fd);
      throw std::runtime_error("Could not size Wayland shared-memory file");
    }
    void* mapping =
      mmap(nullptr, static_cast<std::size_t>(size), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
      close(fd);
      throw std::runtime_error("Could not map Wayland shared-memory file");
    }
    wl_shm_pool* pool = wl_shm_create_pool(connection->shm, fd, static_cast<std::int32_t>(size));
    if (pool == nullptr) {
      munmap(mapping, static_cast<std::size_t>(size));
      close(fd);
      throw std::runtime_error("Could not create Wayland shared-memory pool");
    }
    wl_buffer* native_buffer = wl_shm_pool_create_buffer(
      pool, 0, static_cast<std::int32_t>(extent.width), static_cast<std::int32_t>(extent.height),
      static_cast<std::int32_t>(stride), WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    if (native_buffer == nullptr) {
      munmap(mapping, static_cast<std::size_t>(size));
      throw std::runtime_error("Could not create Wayland shared-memory buffer");
    }

    const std::uint32_t color =
      0xff000000U | (static_cast<std::uint32_t>(generation * 53U) & 0x00ffffffU);
    auto* pixels = static_cast<std::uint32_t*>(mapping);
    std::fill_n(pixels, static_cast<std::size_t>(size / 4U), color);
    auto result = std::make_unique<Buffer>();
    result->buffer = native_buffer;
    result->mapping = mapping;
    result->size = static_cast<std::size_t>(size);
    static constexpr wl_buffer_listener listener{Buffer::handle_release};
    if (wl_buffer_add_listener(native_buffer, &listener, result.get()) != 0) {
      throw std::runtime_error("Could not install Wayland buffer listener");
    }
    return result;
  }

  void present() {
    if (!native_running) {
      return;
    }
    std::erase_if(buffers, [](const auto& buffer) { return buffer->released; });
    const std::optional<WaylandCommit> plan = surface_state.prepare_commit();
    if (!plan.has_value()) {
      return;
    }
    std::unique_ptr<Buffer> buffer;
    try {
      buffers.reserve(buffers.size() + 1);
      buffer = create_buffer(plan->buffer_extent, plan->generation);
    } catch (...) {
      static_cast<void>(surface_state.discard_commit(*plan));
      throw;
    }
    if (!surface_state.submit_commit(*plan)) {
      throw std::runtime_error("Wayland content plan became stale before submission");
    }
    if (plan->configure_serial.has_value()) {
      xdg_surface_ack_configure(shell_surface, *plan->configure_serial);
    }
    wl_surface_set_buffer_scale(surface, static_cast<std::int32_t>(plan->buffer_scale));
    if (plan->use_viewporter) {
      wp_viewport_set_destination(viewport, static_cast<std::int32_t>(plan->logical_extent.width),
                                  static_cast<std::int32_t>(plan->logical_extent.height));
    }
    if (plan->request_frame_callback) {
      auto requested_frame = std::make_unique<Frame>();
      requested_frame->owner = this;
      requested_frame->state_generation = plan->generation;
      requested_frame->host_generation = host_frame_generation;
      requested_frame->callback = wl_surface_frame(surface);
      if (requested_frame->callback == nullptr) {
        throw std::runtime_error("Could not create Wayland frame callback");
      }
      static constexpr wl_callback_listener listener{Frame::handle_done};
      if (wl_callback_add_listener(requested_frame->callback, &listener, requested_frame.get()) !=
          0) {
        throw std::runtime_error("Could not install Wayland frame listener");
      }
      frame = std::move(requested_frame);
    }
    wl_surface_attach(surface, buffer->buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, static_cast<std::int32_t>(plan->buffer_extent.width),
                             static_cast<std::int32_t>(plan->buffer_extent.height));
    wl_surface_commit(surface);
    buffers.push_back(std::move(buffer));
    ++committed_buffers;
    committed_extent = plan->buffer_extent;
    committed_scale = plan->scale_numerator;
    committed_with_viewporter = plan->use_viewporter;
    endpoints.driver.set_metrics({static_cast<double>(plan->logical_extent.width),
                                  static_cast<double>(plan->logical_extent.height)},
                                 plan->buffer_extent.width, plan->buffer_extent.height,
                                 static_cast<double>(plan->scale_numerator) /
                                   WaylandSurfaceState::scale_denominator);
  }

  void fail_native() noexcept {
    endpoints.driver.set_surface_state(HostSurfaceState::lost);
    endpoints.driver.shutdown();
  }

  void shutdown_native() noexcept {
    if (!native_running) {
      return;
    }
    native_running = false;
    destroy_pointer();
    frame.reset();
    buffers.clear();
    if (fractional_scale != nullptr) {
      wp_fractional_scale_v1_destroy(fractional_scale);
      fractional_scale = nullptr;
    }
    if (viewport != nullptr) {
      wp_viewport_destroy(viewport);
      viewport = nullptr;
    }
    if (toplevel != nullptr) {
      xdg_toplevel_destroy(toplevel);
      toplevel = nullptr;
    }
    if (shell_surface != nullptr) {
      xdg_surface_destroy(shell_surface);
      shell_surface = nullptr;
    }
    if (surface != nullptr) {
      wl_surface_destroy(surface);
      surface = nullptr;
    }
  }

  static void handle_surface_configure(void* data, xdg_surface*, std::uint32_t serial) noexcept {
    auto& self = *static_cast<Impl*>(data);
    try {
      self.surface_state.receive_surface_configure(serial);
      self.present();
    } catch (...) {
      self.fail_native();
    }
  }

  void apply_output_scale() noexcept {
    if (fractional_scale != nullptr) {
      return;
    }
    std::uint32_t factor{1};
    for (wl_output* output : entered_outputs) {
      factor = std::max(factor, connection->output_scale(output));
    }
    try {
      surface_state.set_preferred_scale(factor * WaylandSurfaceState::scale_denominator);
      present();
    } catch (...) {
      fail_native();
    }
  }

  void output_changed(wl_output* output, bool removed) noexcept {
    if (removed) {
      std::erase(entered_outputs, output);
    }
    apply_output_scale();
  }

  static void handle_surface_enter(void* data, wl_surface*, wl_output* output) noexcept {
    auto& self = *static_cast<Impl*>(data);
    if (std::ranges::find(self.entered_outputs, output) == self.entered_outputs.end()) {
      try {
        self.entered_outputs.push_back(output);
      } catch (...) {
        self.fail_native();
        return;
      }
    }
    self.apply_output_scale();
  }

  static void handle_surface_leave(void* data, wl_surface*, wl_output* output) noexcept {
    auto& self = *static_cast<Impl*>(data);
    std::erase(self.entered_outputs, output);
    self.apply_output_scale();
  }

  static void handle_preferred_buffer_scale(void* data, wl_surface*, std::int32_t factor) noexcept {
    if (factor <= 0) {
      static_cast<Impl*>(data)->fail_native();
    }
  }

  static void handle_preferred_buffer_transform(void*, wl_surface*, std::uint32_t) noexcept {}

  static void handle_fractional_scale(void* data, wp_fractional_scale_v1*,
                                      std::uint32_t scale) noexcept {
    auto& self = *static_cast<Impl*>(data);
    try {
      self.surface_state.set_preferred_scale(scale);
      self.present();
    } catch (...) {
      self.fail_native();
    }
  }

  void send_pointer(std::optional<PointerEvent> event) noexcept {
    if (event.has_value()) {
      endpoints.driver.send_pointer(*event);
    }
  }

  void destroy_pointer() noexcept {
    send_pointer(pointer_state.capability_lost());
    connection->active_pointer_child = false;
    if (pointer == nullptr) {
      return;
    }
    if (wl_pointer_get_version(pointer) >= WL_POINTER_RELEASE_SINCE_VERSION) {
      wl_pointer_release(pointer);
    } else {
      wl_pointer_destroy(pointer);
    }
    pointer = nullptr;
  }

  void seat_capabilities_changed(std::uint32_t capabilities) noexcept {
    const bool available = (capabilities & WL_SEAT_CAPABILITY_POINTER) != 0;
    if (!available) {
      destroy_pointer();
      return;
    }
    if (pointer != nullptr || !native_running) {
      return;
    }
    pointer = wl_seat_get_pointer(connection->seat);
    if (pointer == nullptr) {
      fail_native();
      return;
    }
    connection->active_pointer_child = true;
    static const wl_pointer_listener listener = [] {
      wl_pointer_listener value{};
      value.enter = handle_pointer_enter;
      value.leave = handle_pointer_leave;
      value.motion = handle_pointer_motion;
      value.button = handle_pointer_button;
      value.axis = handle_pointer_axis;
      value.frame = handle_pointer_frame;
      value.axis_source = handle_pointer_axis_source;
      value.axis_stop = handle_pointer_axis_stop;
      value.axis_discrete = handle_pointer_axis_discrete;
      return value;
    }();
    if (wl_pointer_add_listener(pointer, &listener, this) != 0) {
      destroy_pointer();
      fail_native();
    }
  }

  static void handle_pointer_enter(void* data, wl_pointer*, std::uint32_t, wl_surface* surface,
                                   wl_fixed_t x, wl_fixed_t y) noexcept {
    auto& self = *static_cast<Impl*>(data);
    if (surface != self.surface) {
      return;
    }
    try {
      self.pointer_state.enter(wl_fixed_to_double(x), wl_fixed_to_double(y));
    } catch (...) {
      self.fail_native();
    }
  }

  static void handle_pointer_leave(void* data, wl_pointer*, std::uint32_t,
                                   wl_surface* surface) noexcept {
    auto& self = *static_cast<Impl*>(data);
    if (surface == self.surface) {
      self.send_pointer(self.pointer_state.leave());
    }
  }

  static void handle_pointer_motion(void* data, wl_pointer*, std::uint32_t, wl_fixed_t x,
                                    wl_fixed_t y) noexcept {
    auto& self = *static_cast<Impl*>(data);
    try {
      self.send_pointer(self.pointer_state.motion(wl_fixed_to_double(x), wl_fixed_to_double(y)));
    } catch (...) {
      self.fail_native();
    }
  }

  static void handle_pointer_button(void* data, wl_pointer*, std::uint32_t, std::uint32_t,
                                    std::uint32_t button, std::uint32_t state) noexcept {
    if (button != BTN_LEFT) {
      return;
    }
    auto& self = *static_cast<Impl*>(data);
    try {
      if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        self.send_pointer(self.pointer_state.primary_button(true));
      } else if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
        self.send_pointer(self.pointer_state.primary_button(false));
      } else {
        throw std::invalid_argument("Unknown Wayland pointer button state");
      }
    } catch (...) {
      self.fail_native();
    }
  }

  static void handle_pointer_axis(void*, wl_pointer*, std::uint32_t, std::uint32_t,
                                  wl_fixed_t) noexcept {}
  static void handle_pointer_frame(void*, wl_pointer*) noexcept {}
  static void handle_pointer_axis_source(void*, wl_pointer*, std::uint32_t) noexcept {}
  static void handle_pointer_axis_stop(void*, wl_pointer*, std::uint32_t, std::uint32_t) noexcept {}
  static void handle_pointer_axis_discrete(void*, wl_pointer*, std::uint32_t,
                                           std::int32_t) noexcept {}

  static void handle_toplevel_configure(void* data, xdg_toplevel*, std::int32_t width,
                                        std::int32_t height, wl_array*) noexcept {
    auto& self = *static_cast<Impl*>(data);
    if (width < 0 || height < 0) {
      self.fail_native();
      return;
    }
    try {
      self.surface_state.receive_toplevel_configure(static_cast<std::uint32_t>(width),
                                                    static_cast<std::uint32_t>(height));
    } catch (...) {
      self.fail_native();
    }
  }

  static void handle_toplevel_close(void* data, xdg_toplevel*) noexcept {
    static_cast<Impl*>(data)->endpoints.driver.request_framework_close();
  }

  static void handle_configure_bounds(void*, xdg_toplevel*, std::int32_t, std::int32_t) noexcept {}
  static void handle_wm_capabilities(void*, xdg_toplevel*, wl_array*) noexcept {}
};

std::unique_ptr<WaylandConnection> WaylandConnection::connect(std::string_view display_name) {
  auto impl = std::make_unique<Impl>();
  const std::string owned_name{display_name};
  impl->display = wl_display_connect(owned_name.empty() ? nullptr : owned_name.c_str());
  if (impl->display == nullptr) {
    throw std::runtime_error("Could not connect to the Wayland display");
  }
  impl->registry = wl_display_get_registry(impl->display);
  if (impl->registry == nullptr) {
    throw std::runtime_error("Could not create the Wayland registry proxy");
  }
  static constexpr wl_registry_listener listener{Impl::handle_global, Impl::handle_global_remove};
  if (wl_registry_add_listener(impl->registry, &listener, impl.get()) != 0) {
    throw std::runtime_error("Could not install the Wayland registry listener");
  }
  auto connection = std::unique_ptr<WaylandConnection>{new WaylandConnection{std::move(impl)}};
  connection->roundtrip();
  if (!connection->globals().window_ready()) {
    throw std::runtime_error("Wayland compositor is missing required window globals");
  }
  return connection;
}

WaylandConnection::WaylandConnection(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

WaylandConnection::~WaylandConnection() = default;

std::shared_ptr<TaskRunner> WaylandConnection::task_runner() const {
  impl_->require_owner();
  return impl_->runner;
}

void WaylandConnection::run_pending() {
  impl_->require_owner();
  impl_->runner->run_pending();
}

void WaylandConnection::dispatch() {
  impl_->require_owner();
  impl_->runner->run_pending();
  if (wl_display_dispatch(impl_->display) < 0 || impl_->callback_failed) {
    impl_->fail_transport();
    throw std::runtime_error("Wayland display dispatch failed");
  }
  impl_->runner->run_pending();
}

void WaylandConnection::roundtrip() {
  impl_->require_owner();
  impl_->runner->run_pending();
  if (wl_display_roundtrip(impl_->display) < 0 || impl_->callback_failed) {
    impl_->fail_transport();
    throw std::runtime_error("Wayland display roundtrip failed");
  }
  impl_->runner->run_pending();
}

WaylandGlobals WaylandConnection::globals() const {
  impl_->require_owner();
  return impl_->globals;
}

std::unique_ptr<WaylandWindow> WaylandWindow::create(WaylandConnection& connection, WindowId id,
                                                     WindowConfiguration configuration,
                                                     HostErrorHandler error_handler) {
  connection.impl_->require_owner();
  if (connection.impl_->transport_failed) {
    throw std::logic_error("The Wayland connection has failed");
  }
  if (connection.impl_->active_windows != 0) {
    throw std::logic_error("The reference Wayland connection supports one window");
  }
  constexpr double maximum = static_cast<double>(std::numeric_limits<std::int32_t>::max());
  if (!id.valid() || !configuration.visible || !is_valid_utf8(configuration.title) ||
      !std::isfinite(configuration.logical_size.width) ||
      !std::isfinite(configuration.logical_size.height) ||
      configuration.logical_size.width <= 0.0 || configuration.logical_size.height <= 0.0 ||
      configuration.logical_size.width > maximum || configuration.logical_size.height > maximum ||
      std::trunc(configuration.logical_size.width) != configuration.logical_size.width ||
      std::trunc(configuration.logical_size.height) != configuration.logical_size.height) {
    throw std::invalid_argument("Invalid native Wayland window configuration");
  }
  const WaylandExtent extent{static_cast<std::uint32_t>(configuration.logical_size.width),
                             static_cast<std::uint32_t>(configuration.logical_size.height)};
  auto impl = std::make_unique<Impl>(*connection.impl_, extent);
  ++connection.impl_->active_windows;
  impl->registered = true;
  connection.impl_->active_window = impl.get();
  connection.impl_->fail_active_window = [](void* window) noexcept {
    static_cast<Impl*>(window)->fail_native();
  };
  connection.impl_->active_output_changed = [](void* window, wl_output* output,
                                               bool removed) noexcept {
    static_cast<Impl*>(window)->output_changed(output, removed);
  };
  connection.impl_->active_seat_changed = [](void* window, std::uint32_t capabilities) noexcept {
    static_cast<Impl*>(window)->seat_capabilities_changed(capabilities);
  };
  impl->surface = wl_compositor_create_surface(connection.impl_->compositor);
  if (impl->surface == nullptr) {
    throw std::runtime_error("Could not create Wayland surface");
  }
  static constexpr wl_surface_listener native_surface_listener{
    Impl::handle_surface_enter, Impl::handle_surface_leave, Impl::handle_preferred_buffer_scale,
    Impl::handle_preferred_buffer_transform};
  if (wl_surface_add_listener(impl->surface, &native_surface_listener, impl.get()) != 0) {
    throw std::runtime_error("Could not install Wayland core surface listener");
  }
  if (connection.impl_->viewporter != nullptr && connection.impl_->fractional_scale != nullptr) {
    impl->viewport = wp_viewporter_get_viewport(connection.impl_->viewporter, impl->surface);
    if (impl->viewport == nullptr) {
      throw std::runtime_error("Could not create Wayland viewport");
    }
    impl->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
      connection.impl_->fractional_scale, impl->surface);
    if (impl->fractional_scale == nullptr) {
      throw std::runtime_error("Could not create Wayland fractional-scale object");
    }
    static constexpr wp_fractional_scale_v1_listener fractional_listener{
      Impl::handle_fractional_scale};
    if (wp_fractional_scale_v1_add_listener(impl->fractional_scale, &fractional_listener,
                                            impl.get()) != 0) {
      throw std::runtime_error("Could not install Wayland fractional-scale listener");
    }
  }
  impl->shell_surface = xdg_wm_base_get_xdg_surface(connection.impl_->wm_base, impl->surface);
  if (impl->shell_surface == nullptr) {
    throw std::runtime_error("Could not create xdg surface");
  }
  static constexpr xdg_surface_listener surface_listener{Impl::handle_surface_configure};
  if (xdg_surface_add_listener(impl->shell_surface, &surface_listener, impl.get()) != 0) {
    throw std::runtime_error("Could not install xdg surface listener");
  }
  impl->toplevel = xdg_surface_get_toplevel(impl->shell_surface);
  if (impl->toplevel == nullptr) {
    throw std::runtime_error("Could not create xdg toplevel");
  }
  static constexpr xdg_toplevel_listener toplevel_listener{
    Impl::handle_toplevel_configure, Impl::handle_toplevel_close, Impl::handle_configure_bounds,
    Impl::handle_wm_capabilities};
  if (xdg_toplevel_add_listener(impl->toplevel, &toplevel_listener, impl.get()) != 0) {
    throw std::runtime_error("Could not install xdg toplevel listener");
  }
  xdg_toplevel_set_title(impl->toplevel, configuration.title.c_str());

  configuration.physical_width = extent.width;
  configuration.physical_height = extent.height;
  configuration.device_pixel_ratio = 1.0;
  impl->control = std::make_shared<Impl::Control>(*impl);
  impl->endpoints =
    make_host_window(id, std::move(configuration), connection.impl_->runner,
                     connection.impl_->runner, impl->control, nullptr, std::move(error_handler));
  impl->endpoints.driver.set_surface_state(HostSurfaceState::available);
  impl->seat_capabilities_changed(connection.impl_->globals.seat_capabilities);
  if (!impl->surface_state.begin_initial_commit()) {
    throw std::logic_error("Wayland initial commit state was already consumed");
  }
  wl_surface_commit(impl->surface);
  return std::unique_ptr<WaylandWindow>{new WaylandWindow{std::move(impl)}};
}

WaylandWindow::WaylandWindow(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

WaylandWindow::~WaylandWindow() = default;

HostWindow WaylandWindow::window() const {
  impl_->connection->require_owner();
  return impl_->endpoints.window;
}

WaylandWindowStatus WaylandWindow::status() const {
  impl_->connection->require_owner();
  return {impl_->surface_state.configured(), impl_->committed_buffers,
          impl_->frame != nullptr,           impl_->committed_extent.width,
          impl_->committed_extent.height,    impl_->committed_scale,
          impl_->committed_with_viewporter,  impl_->pointer != nullptr};
}

} // namespace dui::platform
