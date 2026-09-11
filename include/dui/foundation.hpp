#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace dui {

template <std::size_t N> struct fixed_string {
  char value[N]{};

  constexpr fixed_string(const char (&text)[N]) {
    for (std::size_t index = 0; index < N; ++index) {
      value[index] = text[index];
    }
  }

  [[nodiscard]] constexpr std::string_view view() const { return {value, N - 1}; }
};

template <std::size_t N> fixed_string(const char (&)[N]) -> fixed_string<N>;

consteval std::uint64_t hash_name(std::string_view value) {
  std::uint64_t hash = 14695981039346656037ull;
  for (const char character : value) {
    hash ^= static_cast<std::uint8_t>(character);
    hash *= 1099511628211ull;
  }
  return hash;
}

struct TypeToken {
  const void* value{};

  friend constexpr bool operator==(TypeToken, TypeToken) = default;
};

namespace detail {
template <class T> inline constexpr unsigned char type_token_storage{};
}

template <class T> [[nodiscard]] constexpr TypeToken type_token() {
  return {&detail::type_token_storage<std::remove_cvref_t<T>>};
}

class Key {
public:
  using Value = std::variant<std::monostate, std::int64_t, std::uint64_t, std::string>;

  Key() = default;
  Key(std::int64_t value) : value_(value) {}
  Key(std::uint64_t value) : value_(value) {}
  Key(std::string value) : value_(std::move(value)) {}
  Key(std::string_view value) : value_(std::string(value)) {}
  Key(const char* value) : value_(std::string(value)) {}

  [[nodiscard]] bool empty() const { return std::holds_alternative<std::monostate>(value_); }

  [[nodiscard]] const Value& value() const { return value_; }

  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const Key&, const Key&) = default;

private:
  Value value_;
};

template <class T> [[nodiscard]] Key make_key(T&& value) {
  using U = std::remove_cvref_t<T>;
  if constexpr (std::same_as<U, Key>) {
    return std::forward<T>(value);
  } else if constexpr (std::signed_integral<U>) {
    return Key{static_cast<std::int64_t>(value)};
  } else if constexpr (std::unsigned_integral<U>) {
    return Key{static_cast<std::uint64_t>(value)};
  } else if constexpr (std::convertible_to<T, std::string_view>) {
    return Key{std::string_view{std::forward<T>(value)}};
  } else {
    static_assert(std::is_void_v<U>, "A DUI key must be an integer, string, or dui::Key");
  }
}

template <auto Member> struct MemberKey {
  template <class T> [[nodiscard]] decltype(auto) operator()(const T& value) const {
    return value.*Member;
  }
};

template <auto Member> inline constexpr MemberKey<Member> key{};

} // namespace dui
