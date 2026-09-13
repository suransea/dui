#include "dui/input.hpp"

#include <algorithm>
#include <exception>
#include <ranges>
#include <stdexcept>

namespace dui {

void GestureArena::add(PointerId pointer, std::shared_ptr<GestureArenaMember> member) {
  const auto storage = storage_;
  if (member == nullptr) {
    throw std::invalid_argument("GestureArena member cannot be null");
  }
  if (storage->resolving.contains(pointer)) {
    throw std::logic_error("Cannot recreate a GestureArena during resolution callbacks");
  }
  ArenaState& arena = storage->arenas[pointer];
  if (arena.closed) {
    throw std::logic_error("Cannot add a member to a closed GestureArena");
  }
  if (std::ranges::find(arena.members, member) != arena.members.end()) {
    throw std::logic_error("GestureArena member was added twice");
  }
  arena.members.push_back(std::move(member));
}

void GestureArena::close(PointerId pointer) {
  const auto storage = storage_;
  if (storage->resolving.contains(pointer)) {
    return;
  }
  const auto found = storage->arenas.find(pointer);
  if (found == storage->arenas.end()) {
    return;
  }
  found->second.closed = true;
  if (found->second.eager_winner != nullptr) {
    resolve_winner(pointer, found->second.eager_winner);
  } else if (found->second.members.size() == 1) {
    resolve_winner(pointer, found->second.members.front());
  } else if (found->second.members.empty()) {
    storage->arenas.erase(found);
  }
}

void GestureArena::accept(PointerId pointer, const GestureArenaMember& member) {
  const auto storage = storage_;
  if (storage->resolving.contains(pointer)) {
    return;
  }
  const auto found = storage->arenas.find(pointer);
  if (found == storage->arenas.end()) {
    return;
  }
  const auto candidate = std::ranges::find_if(
    found->second.members, [&](const auto& value) { return value.get() == &member; });
  if (candidate == found->second.members.end()) {
    return;
  }
  if (!found->second.closed) {
    if (found->second.eager_winner == nullptr) {
      found->second.eager_winner = *candidate;
    }
    return;
  }
  resolve_winner(pointer, *candidate);
}

void GestureArena::reject(PointerId pointer, const GestureArenaMember& member) {
  const auto storage = storage_;
  if (storage->resolving.contains(pointer)) {
    return;
  }
  const auto found = storage->arenas.find(pointer);
  if (found == storage->arenas.end()) {
    return;
  }

  ArenaState& arena = found->second;
  const auto candidate =
    std::ranges::find_if(arena.members, [&](const auto& value) { return value.get() == &member; });
  if (candidate == arena.members.end()) {
    return;
  }
  std::shared_ptr<GestureArenaMember> rejected = *candidate;
  arena.members.erase(candidate);
  if (arena.eager_winner == rejected) {
    arena.eager_winner.reset();
  }

  std::shared_ptr<GestureArenaMember> winner;
  if (arena.closed && arena.members.size() == 1) {
    winner = arena.members.front();
    storage->arenas.erase(found);
  } else if (arena.members.empty()) {
    storage->arenas.erase(found);
  }

  storage->resolving.insert(pointer);
  std::exception_ptr failure;
  try {
    rejected->reject_gesture(pointer);
  } catch (...) {
    failure = std::current_exception();
  }
  if (winner != nullptr) {
    try {
      winner->accept_gesture(pointer);
    } catch (...) {
      if (failure == nullptr) {
        failure = std::current_exception();
      }
    }
  }
  storage->resolving.erase(pointer);
  if (failure != nullptr) {
    std::rethrow_exception(failure);
  }
}

void GestureArena::sweep(PointerId pointer) {
  const auto storage = storage_;
  if (storage->resolving.contains(pointer)) {
    return;
  }
  const auto found = storage->arenas.find(pointer);
  if (found == storage->arenas.end()) {
    return;
  }
  if (found->second.members.empty()) {
    storage->arenas.erase(found);
    return;
  }
  resolve_winner(pointer, found->second.members.front());
}

void GestureArena::cancel(PointerId pointer) {
  const auto storage = storage_;
  if (storage->resolving.contains(pointer)) {
    return;
  }
  const auto found = storage->arenas.find(pointer);
  if (found == storage->arenas.end()) {
    return;
  }
  auto members = std::move(found->second.members);
  storage->arenas.erase(found);
  storage->resolving.insert(pointer);
  std::exception_ptr failure;
  for (const auto& member : members) {
    try {
      member->reject_gesture(pointer);
    } catch (...) {
      if (failure == nullptr) {
        failure = std::current_exception();
      }
    }
  }
  storage->resolving.erase(pointer);
  if (failure != nullptr) {
    std::rethrow_exception(failure);
  }
}

std::size_t GestureArena::member_count(PointerId pointer) const {
  const auto storage = storage_;
  const auto found = storage->arenas.find(pointer);
  return found == storage->arenas.end() ? 0 : found->second.members.size();
}

bool GestureArena::contains(PointerId pointer) const { return storage_->arenas.contains(pointer); }

void GestureArena::resolve_winner(PointerId pointer, std::shared_ptr<GestureArenaMember> winner) {
  const auto storage = storage_;
  const auto found = storage->arenas.find(pointer);
  if (found == storage->arenas.end()) {
    return;
  }
  auto members = std::move(found->second.members);
  storage->arenas.erase(found);

  storage->resolving.insert(pointer);
  std::exception_ptr failure;
  try {
    winner->accept_gesture(pointer);
  } catch (...) {
    failure = std::current_exception();
  }
  for (const auto& member : members) {
    if (member != winner) {
      try {
        member->reject_gesture(pointer);
      } catch (...) {
        if (failure == nullptr) {
          failure = std::current_exception();
        }
      }
    }
  }
  storage->resolving.erase(pointer);
  if (failure != nullptr) {
    std::rethrow_exception(failure);
  }
}

FocusManager& FocusNode::manager() const {
  const auto lifetime = lifetime_.lock();
  if (lifetime == nullptr || lifetime->manager == nullptr) {
    throw std::logic_error("FocusNode manager no longer exists");
  }
  return *lifetime->manager;
}

FocusNode::~FocusNode() {
  const auto lifetime = lifetime_.lock();
  if (lifetime != nullptr && lifetime->manager != nullptr) {
    lifetime->manager->detach(id_, parent_);
  }
}

void FocusNode::request_focus() { manager().request_focus(*this); }

bool FocusNode::is_focused() const { return manager().focused_node().get() == this; }

void FocusNode::set_can_focus(bool value) {
  can_focus_ = value;
  if (!can_focus_) {
    FocusManager& focus_manager = manager();
    if (const auto focused = focus_manager.focused_node(); focused.get() == this) {
      focus_manager.clear_focus();
    }
  }
}

FocusManager::FocusManager() : lifetime_(std::make_shared<FocusLifetime>()) {
  lifetime_->manager = this;
}

FocusManager::~FocusManager() {
  lifetime_->manager = nullptr;
  nodes_.clear();
}

std::shared_ptr<FocusNode> FocusManager::create_node(std::shared_ptr<FocusNode> parent,
                                                     FocusNode::KeyHandler handler,
                                                     bool can_focus) {
  std::optional<FocusNode::Id> parent_id;
  if (parent != nullptr) {
    const auto parent_lifetime = parent->lifetime_.lock();
    if (parent_lifetime != lifetime_) {
      throw std::logic_error("Focus parent belongs to another FocusManager");
    }
    parent_id = parent->id_;
  }

  const FocusNode::Id id = next_id_++;
  auto node = std::shared_ptr<FocusNode>{
    new FocusNode{lifetime_, id, parent_id, can_focus, std::move(handler)}};
  nodes_.emplace(id, node);
  return node;
}

void FocusManager::request_focus(const FocusNode& node) {
  const auto lifetime = node.lifetime_.lock();
  if (lifetime != lifetime_) {
    throw std::logic_error("FocusNode belongs to another FocusManager");
  }
  if (!node.can_focus_) {
    throw std::logic_error("FocusNode cannot receive focus");
  }
  focused_ = node.id_;
}

void FocusManager::clear_focus() { focused_.reset(); }

std::shared_ptr<FocusNode> FocusManager::focused_node() const {
  return focused_.has_value() ? resolve(*focused_) : nullptr;
}

KeyEventResult FocusManager::dispatch_key(const KeyEvent& event) {
  std::vector<std::shared_ptr<FocusNode>> route;
  std::shared_ptr<FocusNode> node = focused_node();
  while (node != nullptr) {
    route.push_back(node);
    node = node->parent_.has_value() ? resolve(*node->parent_) : nullptr;
  }

  for (const auto& route_node : route) {
    FocusNode::KeyHandler handler = route_node->handler_;
    if (handler && handler(event) == KeyEventResult::handled) {
      return KeyEventResult::handled;
    }
  }
  return KeyEventResult::ignored;
}

void FocusManager::detach(FocusNode::Id id, std::optional<FocusNode::Id> parent) {
  nodes_.erase(id);
  for (auto& [node_id, weak_node] : nodes_) {
    static_cast<void>(node_id);
    if (const auto node = weak_node.lock(); node != nullptr && node->parent_ == id) {
      node->parent_ = parent;
    }
  }
  if (focused_ != id) {
    return;
  }
  focused_.reset();
  while (parent.has_value()) {
    const auto candidate = resolve(*parent);
    if (candidate == nullptr) {
      break;
    }
    if (candidate->can_focus_) {
      focused_ = candidate->id_;
      break;
    }
    parent = candidate->parent_;
  }
}

std::shared_ptr<FocusNode> FocusManager::resolve(FocusNode::Id id) const {
  const auto found = nodes_.find(id);
  return found == nodes_.end() ? nullptr : found->second.lock();
}

bool TextRange::valid_for(std::string_view utf8_text) const {
  if (!is_valid_utf8(utf8_text)) {
    return false;
  }
  const auto is_boundary = [&](std::size_t offset) {
    if (offset > utf8_text.size()) {
      return false;
    }
    if (offset == utf8_text.size()) {
      return true;
    }
    const auto byte = static_cast<unsigned char>(utf8_text[offset]);
    return (byte & 0xc0u) != 0x80u;
  };
  return start <= end && is_boundary(start) && is_boundary(end);
}

bool is_valid_utf8(std::string_view text) {
  std::size_t index = 0;
  while (index < text.size()) {
    const auto first = static_cast<unsigned char>(text[index]);
    std::size_t length = 0;
    std::uint32_t code_point = 0;
    std::uint32_t minimum = 0;
    if (first <= 0x7fu) {
      length = 1;
      code_point = first;
    } else if ((first & 0xe0u) == 0xc0u) {
      length = 2;
      code_point = first & 0x1fu;
      minimum = 0x80u;
    } else if ((first & 0xf0u) == 0xe0u) {
      length = 3;
      code_point = first & 0x0fu;
      minimum = 0x800u;
    } else if ((first & 0xf8u) == 0xf0u) {
      length = 4;
      code_point = first & 0x07u;
      minimum = 0x10000u;
    } else {
      return false;
    }
    if (length > text.size() - index) {
      return false;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto continuation = static_cast<unsigned char>(text[index + offset]);
      if ((continuation & 0xc0u) != 0x80u) {
        return false;
      }
      code_point = (code_point << 6u) | (continuation & 0x3fu);
    }
    if (code_point < minimum || code_point > 0x10ffffu ||
        (code_point >= 0xd800u && code_point <= 0xdfffu)) {
      return false;
    }
    index += length;
  }
  return true;
}

bool TextEditingValue::valid() const {
  return selection.valid_for(text) && (!composing.has_value() || composing->valid_for(text));
}

} // namespace dui
