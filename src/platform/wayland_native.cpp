#include "dui/platform/wayland_native.hpp"

#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <wayland-client.h>

#include <algorithm>
#include <cstring>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace dui::platform {

namespace {

enum class GlobalKind { compositor, shm, seat, wm_base, viewporter, fractional_scale };

struct GlobalOffer {
  std::uint32_t name{};
  std::uint32_t version{};
  GlobalKind kind{};
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

  ~Impl() {
    if (std::this_thread::get_id() != owner) {
      std::terminate();
    }
    if (fractional_scale != nullptr) {
      wp_fractional_scale_manager_v1_destroy(fractional_scale);
    }
    if (viewporter != nullptr) {
      wp_viewporter_destroy(viewporter);
    }
    if (seat != nullptr) {
      wl_seat_destroy(seat);
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
        globals.seat_version = negotiated;
        seat_name = name;
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
    if (name == fractional_scale_name && fractional_scale != nullptr) {
      wp_fractional_scale_manager_v1_destroy(fractional_scale);
      fractional_scale = nullptr;
      globals.fractional_scale_version = 0;
    } else if (name == viewporter_name && viewporter != nullptr) {
      wp_viewporter_destroy(viewporter);
      viewporter = nullptr;
      globals.viewporter_version = 0;
    } else if (name == seat_name && seat != nullptr) {
      wl_seat_destroy(seat);
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

void WaylandConnection::dispatch() {
  impl_->require_owner();
  if (wl_display_dispatch(impl_->display) < 0 || impl_->callback_failed) {
    throw std::runtime_error("Wayland display dispatch failed");
  }
}

void WaylandConnection::roundtrip() {
  impl_->require_owner();
  if (wl_display_roundtrip(impl_->display) < 0 || impl_->callback_failed) {
    throw std::runtime_error("Wayland display roundtrip failed");
  }
}

WaylandGlobals WaylandConnection::globals() const {
  impl_->require_owner();
  return impl_->globals;
}

} // namespace dui::platform
