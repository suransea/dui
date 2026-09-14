#include "dui/platform/wayland.hpp"

#include <limits>
#include <stdexcept>

namespace dui::platform {

namespace {

std::uint32_t checked_buffer_dimension(std::uint32_t logical, std::uint64_t numerator) {
  const auto scaled = static_cast<std::uint64_t>(logical) * numerator;
  const auto rounded =
    (scaled + WaylandSurfaceState::scale_denominator - 1) / WaylandSurfaceState::scale_denominator;
  if (rounded == 0 ||
      rounded > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
    throw std::overflow_error("Wayland buffer extent overflow");
  }
  return static_cast<std::uint32_t>(rounded);
}

} // namespace

bool WaylandCommit::valid() const noexcept {
  if (generation == 0 || state_revision == 0 || !logical_extent.valid() || !buffer_extent.valid() ||
      buffer_scale == 0 ||
      buffer_scale > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
      scale_numerator == 0) {
    return false;
  }
  const std::uint64_t expected_width =
    (static_cast<std::uint64_t>(logical_extent.width) * scale_numerator +
     WaylandSurfaceState::scale_denominator - 1) /
    WaylandSurfaceState::scale_denominator;
  const std::uint64_t expected_height =
    (static_cast<std::uint64_t>(logical_extent.height) * scale_numerator +
     WaylandSurfaceState::scale_denominator - 1) /
    WaylandSurfaceState::scale_denominator;
  if (expected_width != buffer_extent.width || expected_height != buffer_extent.height) {
    return false;
  }
  if (use_viewporter) {
    return buffer_scale == 1;
  }
  return scale_numerator % WaylandSurfaceState::scale_denominator == 0 &&
         buffer_scale == scale_numerator / WaylandSurfaceState::scale_denominator;
}

WaylandSurfaceState::WaylandSurfaceState(WaylandExtent initial_extent,
                                         bool fractional_scale_supported)
  : desired_extent_(initial_extent), applied_extent_(initial_extent),
    fractional_scale_supported_(fractional_scale_supported) {
  if (!initial_extent.valid()) {
    throw std::invalid_argument("Wayland surface requires a nonzero initial extent");
  }
}

bool WaylandSurfaceState::begin_initial_commit() noexcept {
  if (initial_commit_started_) {
    return false;
  }
  initial_commit_started_ = true;
  return true;
}

void WaylandSurfaceState::receive_toplevel_configure(std::uint32_t width, std::uint32_t height) {
  constexpr auto maximum = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
  if (width > maximum || height > maximum) {
    throw std::invalid_argument("Wayland toplevel extent exceeds protocol range");
  }
  staged_width_ = width;
  staged_height_ = height;
}

void WaylandSurfaceState::receive_surface_configure(std::uint32_t serial) {
  if (!initial_commit_started_) {
    throw std::invalid_argument("Invalid Wayland surface configure");
  }
  advance_revision();
  if (staged_width_ != 0) {
    desired_extent_.width = staged_width_;
  }
  if (staged_height_ != 0) {
    desired_extent_.height = staged_height_;
  }
  staged_width_ = 0;
  staged_height_ = 0;
  pending_configure_ = serial;
  dirty_ = true;
}

void WaylandSurfaceState::set_preferred_scale(std::uint32_t numerator) {
  if (numerator == 0) {
    throw std::invalid_argument("Wayland preferred scale must be nonzero");
  }
  if (desired_scale_ == numerator) {
    return;
  }
  advance_revision();
  desired_scale_ = numerator;
  if (configured_ || pending_configure_.has_value()) {
    dirty_ = true;
  }
}

void WaylandSurfaceState::request_frame() noexcept { frame_demand_ = true; }

void WaylandSurfaceState::buffer_lost() {
  if (!configured_ && !pending_configure_.has_value()) {
    return;
  }
  advance_revision();
  dirty_ = true;
}

std::optional<WaylandCommit> WaylandSurfaceState::prepare_commit() {
  if (!initial_commit_started_ || (!configured_ && !pending_configure_.has_value()) ||
      prepared_.has_value()) {
    return std::nullopt;
  }
  const bool can_request_frame = frame_demand_ && frame_callback_generation_ == 0;
  if (!dirty_ && !pending_configure_.has_value() && !can_request_frame) {
    return std::nullopt;
  }
  if (next_commit_generation_ == std::numeric_limits<std::uint64_t>::max()) {
    throw std::overflow_error("Wayland commit generation exhausted");
  }

  const std::uint64_t effective_scale =
    fractional_scale_supported_
      ? desired_scale_
      : ((static_cast<std::uint64_t>(desired_scale_) + scale_denominator - 1) / scale_denominator) *
          scale_denominator;
  if (effective_scale > std::numeric_limits<std::uint32_t>::max()) {
    throw std::overflow_error("Wayland effective scale overflow");
  }
  const std::uint64_t integer_scale =
    (static_cast<std::uint64_t>(effective_scale) + scale_denominator - 1) / scale_denominator;
  if (integer_scale > std::numeric_limits<std::uint32_t>::max()) {
    throw std::overflow_error("Wayland integer buffer scale overflow");
  }

  WaylandCommit commit{next_commit_generation_,
                       state_revision_,
                       pending_configure_,
                       desired_extent_,
                       {checked_buffer_dimension(desired_extent_.width, effective_scale),
                        checked_buffer_dimension(desired_extent_.height, effective_scale)},
                       fractional_scale_supported_ ? 1U : static_cast<std::uint32_t>(integer_scale),
                       static_cast<std::uint32_t>(effective_scale),
                       fractional_scale_supported_,
                       can_request_frame};
  prepared_ = commit;
  ++next_commit_generation_;
  return prepared_;
}

bool WaylandSurfaceState::submit_commit(const WaylandCommit& commit) noexcept {
  if (!prepared_.has_value() || *prepared_ != commit || commit.state_revision != state_revision_) {
    return false;
  }
  applied_extent_ = desired_extent_;
  applied_scale_ = commit.scale_numerator;
  pending_configure_.reset();
  dirty_ = false;
  configured_ = true;
  if (commit.request_frame_callback) {
    frame_callback_generation_ = commit.generation;
    frame_demand_ = false;
  }
  prepared_.reset();
  return true;
}

bool WaylandSurfaceState::discard_commit(const WaylandCommit& commit) noexcept {
  if (!prepared_.has_value() || *prepared_ != commit) {
    return false;
  }
  prepared_.reset();
  dirty_ = true;
  return true;
}

bool WaylandSurfaceState::frame_done(std::uint64_t generation) noexcept {
  if (generation == 0 || frame_callback_generation_ != generation) {
    return false;
  }
  frame_callback_generation_ = 0;
  return true;
}

void WaylandSurfaceState::invalidate_prepared() noexcept { prepared_.reset(); }

void WaylandSurfaceState::advance_revision() {
  if (state_revision_ == std::numeric_limits<std::uint64_t>::max()) {
    throw std::overflow_error("Wayland surface state revision exhausted");
  }
  ++state_revision_;
  invalidate_prepared();
}

} // namespace dui::platform
