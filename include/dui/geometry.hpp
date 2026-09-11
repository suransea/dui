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
        return point.x >= 0.0 && point.y >= 0.0
            && point.x < width && point.y < height;
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

    [[nodiscard]] static constexpr Insets all(double value) {
        return {value, value, value, value};
    }

    [[nodiscard]] constexpr double horizontal() const { return left + right; }
    [[nodiscard]] constexpr double vertical() const { return top + bottom; }

    friend constexpr bool operator==(Insets, Insets) = default;
};

class BoxConstraints {
public:
    constexpr BoxConstraints(
        double min_width = 0.0,
        double max_width = infinity,
        double min_height = 0.0,
        double max_height = infinity
    ) :
        min_width_(min_width),
        max_width_(max_width),
        min_height_(min_height),
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

    [[nodiscard]] constexpr bool has_bounded_width() const {
        return std::isfinite(max_width_);
    }

    [[nodiscard]] constexpr bool has_bounded_height() const {
        return std::isfinite(max_height_);
    }

    [[nodiscard]] constexpr Size constrain(Size size) const {
        if (!std::isfinite(size.width) || !std::isfinite(size.height)
            || size.width < 0.0 || size.height < 0.0) {
            throw std::invalid_argument("A constrained Size must be finite and non-negative");
        }
        return {
            std::clamp(size.width, min_width_, max_width_),
            std::clamp(size.height, min_height_, max_height_)
        };
    }

    [[nodiscard]] constexpr BoxConstraints loosen() const {
        return {0.0, max_width_, 0.0, max_height_};
    }

    [[nodiscard]] constexpr Size biggest() const {
        return {max_width_, max_height_};
    }

    friend constexpr bool operator==(const BoxConstraints&, const BoxConstraints&) = default;

private:
    [[nodiscard]] constexpr bool valid() const {
        return !std::isnan(min_width_) && !std::isnan(max_width_)
            && !std::isnan(min_height_) && !std::isnan(max_height_)
            && min_width_ >= 0.0 && min_height_ >= 0.0
            && min_width_ <= max_width_ && min_height_ <= max_height_;
    }

    double min_width_;
    double max_width_;
    double min_height_;
    double max_height_;
};

} // namespace dui
