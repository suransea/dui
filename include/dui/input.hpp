#pragma once

#include "dui/geometry.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dui {

using PointerId = std::int64_t;

enum class PointerPhase { down, move, up, cancel };

struct PointerEvent {
  PointerId pointer{};
  PointerPhase phase{};
  Offset position{};

  friend constexpr bool operator==(PointerEvent, PointerEvent) = default;
};

class GestureArenaMember {
public:
  virtual ~GestureArenaMember() = default;
  virtual void accept_gesture(PointerId pointer) = 0;
  virtual void reject_gesture(PointerId pointer) = 0;
};

class GestureArena {
public:
  void add(PointerId pointer, std::shared_ptr<GestureArenaMember> member);
  void close(PointerId pointer);
  void accept(PointerId pointer, const GestureArenaMember& member);
  void reject(PointerId pointer, const GestureArenaMember& member);
  void sweep(PointerId pointer);
  void cancel(PointerId pointer);

  [[nodiscard]] std::size_t member_count(PointerId pointer) const;
  [[nodiscard]] bool contains(PointerId pointer) const;

private:
  struct ArenaState {
    bool closed{};
    std::vector<std::shared_ptr<GestureArenaMember>> members;
    std::shared_ptr<GestureArenaMember> eager_winner;
  };

  struct Storage {
    std::unordered_map<PointerId, ArenaState> arenas;
    std::unordered_set<PointerId> resolving;
  };

  void resolve_winner(PointerId pointer, std::shared_ptr<GestureArenaMember> winner);

  std::shared_ptr<Storage> storage_{std::make_shared<Storage>()};
};

enum class KeyPhase { down, repeat, up };

struct KeyEvent {
  std::string logical_key;
  KeyPhase phase{};
  bool shift{};
  bool control{};
  bool alt{};
  bool meta{};
};

enum class KeyEventResult { ignored, handled };

class FocusManager;

struct FocusLifetime {
  FocusManager* manager{};
};

class FocusNode {
public:
  using Id = std::uint64_t;
  using KeyHandler = std::function<KeyEventResult(const KeyEvent&)>;

  ~FocusNode();

  FocusNode(const FocusNode&) = delete;
  FocusNode& operator=(const FocusNode&) = delete;

  [[nodiscard]] Id id() const { return id_; }
  [[nodiscard]] std::optional<Id> parent_id() const { return parent_; }
  [[nodiscard]] bool can_focus() const { return can_focus_; }
  [[nodiscard]] bool is_focused() const;

  void request_focus();
  void set_can_focus(bool value);
  void set_key_handler(KeyHandler handler) { handler_ = std::move(handler); }

private:
  friend class FocusManager;

  FocusNode(std::weak_ptr<FocusLifetime> lifetime, Id id, std::optional<Id> parent, bool can_focus,
            KeyHandler handler)
    : lifetime_(std::move(lifetime)), id_(id), parent_(parent), can_focus_(can_focus),
      handler_(std::move(handler)) {}

  [[nodiscard]] FocusManager& manager() const;

  std::weak_ptr<FocusLifetime> lifetime_;
  Id id_{};
  std::optional<Id> parent_;
  bool can_focus_{true};
  KeyHandler handler_;
};

class FocusManager {
public:
  FocusManager();
  ~FocusManager();

  FocusManager(const FocusManager&) = delete;
  FocusManager& operator=(const FocusManager&) = delete;

  [[nodiscard]] std::shared_ptr<FocusNode> create_node(std::shared_ptr<FocusNode> parent = {},
                                                       FocusNode::KeyHandler handler = {},
                                                       bool can_focus = true);

  void request_focus(const FocusNode& node);
  void clear_focus();
  [[nodiscard]] std::shared_ptr<FocusNode> focused_node() const;
  [[nodiscard]] KeyEventResult dispatch_key(const KeyEvent& event);

private:
  friend class FocusNode;

  void detach(FocusNode::Id id, std::optional<FocusNode::Id> parent);
  [[nodiscard]] std::shared_ptr<FocusNode> resolve(FocusNode::Id id) const;

  std::shared_ptr<FocusLifetime> lifetime_;
  std::unordered_map<FocusNode::Id, std::weak_ptr<FocusNode>> nodes_;
  FocusNode::Id next_id_{1};
  std::optional<FocusNode::Id> focused_;
};

struct TextRange {
  std::size_t start{};
  std::size_t end{};

  [[nodiscard]] bool collapsed() const { return start == end; }
  [[nodiscard]] bool valid_for(std::string_view utf8_text) const;
  friend constexpr bool operator==(TextRange, TextRange) = default;
};

[[nodiscard]] bool is_valid_utf8(std::string_view text);

struct TextEditingValue {
  std::string text;
  TextRange selection;
  std::optional<TextRange> composing;

  // Ranges use UTF-8 byte offsets and must lie on code-point boundaries.
  [[nodiscard]] bool valid() const;
};

enum class TextInputAction { none, done, next, search, send };

struct TextInputConfiguration {
  bool multiline{};
  bool obscure_text{};
  bool autocorrect{true};
  TextInputAction action{TextInputAction::done};
};

class TextInputClient {
public:
  virtual ~TextInputClient() = default;
  virtual void update_editing_value(TextEditingValue value) = 0;
  virtual void perform_action(TextInputAction action) = 0;
};

using TextInputSessionId = std::uint64_t;

class TextInputBackend {
public:
  virtual ~TextInputBackend() = default;
  [[nodiscard]] virtual TextInputSessionId start_text_input(std::weak_ptr<TextInputClient> client,
                                                            TextInputConfiguration configuration,
                                                            TextEditingValue initial_value) = 0;
  virtual void update_editing_state(TextInputSessionId session, TextEditingValue value) = 0;
  virtual void stop_text_input(TextInputSessionId session) = 0;
  virtual void set_editable_rect(TextInputSessionId session, Rect rect) = 0;
};

} // namespace dui
