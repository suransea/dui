#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace dui {

inline constexpr double infinity = std::numeric_limits<double>::infinity();

struct Offset {
  double x{};
  double y{};

  friend constexpr bool operator==(Offset, Offset) = default;
};

[[nodiscard]] constexpr Offset operator+(Offset left, Offset right) {
  return {left.x + right.x, left.y + right.y};
}

[[nodiscard]] constexpr Offset operator-(Offset left, Offset right) {
  return {left.x - right.x, left.y - right.y};
}

struct Size {
  double width{};
  double height{};

  [[nodiscard]] constexpr bool contains(Offset point) const {
    return point.x >= 0.0 && point.y >= 0.0 && point.x < width && point.y < height;
  }

  friend constexpr bool operator==(Size, Size) = default;
};

struct Rect {
  Offset origin;
  Size size;

  [[nodiscard]] constexpr bool contains(Offset point) const {
    return size.contains(point - origin);
  }

  friend constexpr bool operator==(Rect, Rect) = default;
};

struct Insets {
  double left{};
  double top{};
  double right{};
  double bottom{};

  [[nodiscard]] static constexpr Insets all(double value) { return {value, value, value, value}; }

  [[nodiscard]] constexpr double horizontal() const { return left + right; }
  [[nodiscard]] constexpr double vertical() const { return top + bottom; }

  friend constexpr bool operator==(Insets, Insets) = default;
};

class BoxConstraints {
public:
  constexpr BoxConstraints(double min_width = 0.0, double max_width = infinity,
                           double min_height = 0.0, double max_height = infinity)
    : min_width_(min_width), max_width_(max_width), min_height_(min_height),
      max_height_(max_height) {
    if (!valid()) {
      throw std::invalid_argument("BoxConstraints must be normalized and non-negative");
    }
  }

  [[nodiscard]] static constexpr BoxConstraints tight(Size size) {
    return {size.width, size.width, size.height, size.height};
  }

  [[nodiscard]] static constexpr BoxConstraints loose(Size size) {
    return {0.0, size.width, 0.0, size.height};
  }

  [[nodiscard]] constexpr double min_width() const { return min_width_; }
  [[nodiscard]] constexpr double max_width() const { return max_width_; }
  [[nodiscard]] constexpr double min_height() const { return min_height_; }
  [[nodiscard]] constexpr double max_height() const { return max_height_; }

  [[nodiscard]] constexpr bool has_bounded_width() const { return std::isfinite(max_width_); }

  [[nodiscard]] constexpr bool has_bounded_height() const { return std::isfinite(max_height_); }

  [[nodiscard]] constexpr Size constrain(Size size) const {
    if (!std::isfinite(size.width) || !std::isfinite(size.height) || size.width < 0.0 ||
        size.height < 0.0) {
      throw std::invalid_argument("A constrained Size must be finite and non-negative");
    }
    return {std::clamp(size.width, min_width_, max_width_),
            std::clamp(size.height, min_height_, max_height_)};
  }

  [[nodiscard]] constexpr BoxConstraints loosen() const {
    return {0.0, max_width_, 0.0, max_height_};
  }

  [[nodiscard]] constexpr Size biggest() const { return {max_width_, max_height_}; }

  friend constexpr bool operator==(const BoxConstraints&, const BoxConstraints&) = default;

private:
  [[nodiscard]] constexpr bool valid() const {
    return !std::isnan(min_width_) && !std::isnan(max_width_) && !std::isnan(min_height_) &&
           !std::isnan(max_height_) && min_width_ >= 0.0 && min_height_ >= 0.0 &&
           min_width_ <= max_width_ && min_height_ <= max_height_;
  }

  double min_width_;
  double max_width_;
  double min_height_;
  double max_height_;
};

class SliverConstraints {
public:
  constexpr SliverConstraints(double scroll_offset, double preceding_scroll_extent,
                              double remaining_paint_extent, double cross_axis_extent,
                              double viewport_main_axis_extent)
    : scroll_offset_(scroll_offset), preceding_scroll_extent_(preceding_scroll_extent),
      remaining_paint_extent_(remaining_paint_extent), cross_axis_extent_(cross_axis_extent),
      viewport_main_axis_extent_(viewport_main_axis_extent) {
    if (!valid()) {
      throw std::invalid_argument("SliverConstraints values must be finite and non-negative");
    }
  }

  [[nodiscard]] constexpr double scroll_offset() const { return scroll_offset_; }
  [[nodiscard]] constexpr double preceding_scroll_extent() const {
    return preceding_scroll_extent_;
  }
  [[nodiscard]] constexpr double remaining_paint_extent() const { return remaining_paint_extent_; }
  [[nodiscard]] constexpr double cross_axis_extent() const { return cross_axis_extent_; }
  [[nodiscard]] constexpr double viewport_main_axis_extent() const {
    return viewport_main_axis_extent_;
  }
  [[nodiscard]] constexpr BoxConstraints as_box_constraints() const {
    return {cross_axis_extent_, cross_axis_extent_, 0.0, infinity};
  }

  friend constexpr bool operator==(const SliverConstraints&, const SliverConstraints&) = default;

private:
  [[nodiscard]] constexpr bool valid() const {
    return std::isfinite(scroll_offset_) && std::isfinite(preceding_scroll_extent_) &&
           std::isfinite(remaining_paint_extent_) && std::isfinite(cross_axis_extent_) &&
           std::isfinite(viewport_main_axis_extent_) && scroll_offset_ >= 0.0 &&
           preceding_scroll_extent_ >= 0.0 && remaining_paint_extent_ >= 0.0 &&
           cross_axis_extent_ >= 0.0 && viewport_main_axis_extent_ >= 0.0;
  }

  double scroll_offset_;
  double preceding_scroll_extent_;
  double remaining_paint_extent_;
  double cross_axis_extent_;
  double viewport_main_axis_extent_;
};

class SliverGeometry {
public:
  constexpr SliverGeometry(double scroll_extent = 0.0, double paint_extent = 0.0,
                           double max_paint_extent = 0.0, double hit_test_extent = 0.0,
                           bool has_visual_overflow = false)
    : scroll_extent_(scroll_extent), paint_extent_(paint_extent),
      max_paint_extent_(max_paint_extent), hit_test_extent_(hit_test_extent),
      has_visual_overflow_(has_visual_overflow) {
    if (!valid()) {
      throw std::invalid_argument(
        "SliverGeometry extents must be finite, non-negative, and normalized");
    }
  }

  [[nodiscard]] constexpr double scroll_extent() const { return scroll_extent_; }
  [[nodiscard]] constexpr double paint_extent() const { return paint_extent_; }
  [[nodiscard]] constexpr double max_paint_extent() const { return max_paint_extent_; }
  [[nodiscard]] constexpr double hit_test_extent() const { return hit_test_extent_; }
  [[nodiscard]] constexpr bool has_visual_overflow() const { return has_visual_overflow_; }

  friend constexpr bool operator==(const SliverGeometry&, const SliverGeometry&) = default;

private:
  [[nodiscard]] constexpr bool valid() const {
    return std::isfinite(scroll_extent_) && std::isfinite(paint_extent_) &&
           std::isfinite(max_paint_extent_) && std::isfinite(hit_test_extent_) &&
           scroll_extent_ >= 0.0 && paint_extent_ >= 0.0 && max_paint_extent_ >= paint_extent_ &&
           hit_test_extent_ >= 0.0 && hit_test_extent_ <= paint_extent_;
  }

  double scroll_extent_;
  double paint_extent_;
  double max_paint_extent_;
  double hit_test_extent_;
  bool has_visual_overflow_;
};

} // namespace dui
