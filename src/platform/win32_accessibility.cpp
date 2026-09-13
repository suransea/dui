#include "dui/platform/win32_accessibility.hpp"

#ifndef _WIN32
#error "win32_accessibility.cpp is only supported on Windows"
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <oleauto.h>
#include <uiautomation.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dui::win32 {
namespace {

constexpr HRESULT element_not_available = static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);
constexpr HRESULT element_not_enabled = static_cast<HRESULT>(UIA_E_ELEMENTNOTENABLED);
constexpr HRESULT not_supported = static_cast<HRESULT>(UIA_E_NOTSUPPORTED);

template <typename Function> HRESULT translate_com_call(Function&& function) noexcept {
  try {
    return function();
  } catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
  } catch (...) {
    return E_FAIL;
  }
}

struct NativeNode {
  SemanticsEntry entry;
  std::vector<std::uint64_t> children;
};

struct ProviderModel {
  std::recursive_mutex mutex;
  HWND window{};
  double device_pixel_ratio{1.0};
  AccessibilityAdapter::ActionHandler action_handler;
  std::unordered_map<std::uint64_t, NativeNode> nodes;
  std::vector<std::uint64_t> roots;
  std::unordered_map<std::uintptr_t, std::uint64_t> pending_actions;
  std::uintptr_t next_action{1};
  UINT action_message{};
  bool alive{true};
};

[[nodiscard]] bool valid_rect(Rect rect) {
  return std::isfinite(rect.origin.x) && std::isfinite(rect.origin.y) &&
         std::isfinite(rect.size.width) && std::isfinite(rect.size.height) &&
         rect.size.width >= 0.0 && rect.size.height >= 0.0;
}

void validate_scaled_bounds(const ProviderModel& model, double scale) {
  for (const auto& [id, node] : model.nodes) {
    static_cast<void>(id);
    const Rect bounds = node.entry.bounds;
    if (!std::isfinite(bounds.origin.x * scale) || !std::isfinite(bounds.origin.y * scale) ||
        !std::isfinite(bounds.size.width * scale) || !std::isfinite(bounds.size.height * scale)) {
      throw std::invalid_argument("Win32 accessibility bounds overflow at the configured DPR");
    }
  }
}

[[nodiscard]] std::wstring utf8_to_wide(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument("UIA text is too large");
  }
  const int source_size = static_cast<int>(text.size());
  const int size =
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), source_size, nullptr, 0);
  if (size <= 0) {
    throw std::invalid_argument("UIA text is not valid UTF-8");
  }
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), source_size, result.data(),
                          size) != size) {
    throw std::runtime_error("Failed to convert UIA text to UTF-16");
  }
  return result;
}

[[nodiscard]] CONTROLTYPEID control_type(SemanticsRole role) {
  switch (role) {
  case SemanticsRole::text:
    return UIA_TextControlTypeId;
  case SemanticsRole::image:
    return UIA_ImageControlTypeId;
  case SemanticsRole::button:
    return UIA_ButtonControlTypeId;
  case SemanticsRole::generic:
    return UIA_GroupControlTypeId;
  }
  return UIA_GroupControlTypeId;
}

class NodeProvider final : public IRawElementProviderSimple,
                           public IRawElementProviderFragment,
                           public IRawElementProviderFragmentRoot,
                           public IInvokeProvider,
                           public IValueProvider {
public:
  NodeProvider(std::shared_ptr<ProviderModel> model, std::uint64_t id)
    : model_(std::move(model)), id_(id) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interface_id, void** result) noexcept override {
    if (result == nullptr) {
      return E_POINTER;
    }
    *result = nullptr;
    if (IsEqualIID(interface_id, IID_IUnknown) ||
        IsEqualIID(interface_id, IID_IRawElementProviderSimple)) {
      *result = static_cast<IRawElementProviderSimple*>(this);
    } else if (IsEqualIID(interface_id, IID_IRawElementProviderFragment)) {
      *result = static_cast<IRawElementProviderFragment*>(this);
    } else if (IsEqualIID(interface_id, IID_IRawElementProviderFragmentRoot) && id_ == 0) {
      *result = static_cast<IRawElementProviderFragmentRoot*>(this);
    } else if (IsEqualIID(interface_id, IID_IInvokeProvider) && id_ != 0) {
      *result = static_cast<IInvokeProvider*>(this);
    } else if (IsEqualIID(interface_id, IID_IValueProvider) && id_ != 0) {
      *result = static_cast<IValueProvider*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++references_; }

  ULONG STDMETHODCALLTYPE Release() noexcept override {
    const ULONG remaining = --references_;
    if (remaining == 0) {
      delete this;
    }
    return remaining;
  }

  HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* result) noexcept override {
    if (result == nullptr) {
      return E_POINTER;
    }
    *result = ProviderOptions_ServerSideProvider;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern_id,
                                               IUnknown** result) noexcept override {
    if (result == nullptr) {
      return E_POINTER;
    }
    *result = nullptr;
    return translate_com_call([&] {
      const std::scoped_lock lock{model_->mutex};
      if (!available()) {
        return element_not_available;
      }
      if (pattern_id == UIA_InvokePatternId && supports_invoke()) {
        *result = static_cast<IInvokeProvider*>(this);
      } else if (pattern_id == UIA_ValuePatternId && supports_value()) {
        *result = static_cast<IValueProvider*>(this);
      }
      if (*result != nullptr) {
        AddRef();
      }
      return S_OK;
    });
  }

  HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property_id,
                                             VARIANT* result) noexcept override {
    if (result == nullptr) {
      return E_POINTER;
    }
    VariantInit(result);
    try {
      const std::scoped_lock lock{model_->mutex};
      if (!available()) {
        return element_not_available;
      }
      const SemanticsEntry* entry = find_entry();
      if (id_ == 0) {
        return S_OK;
      }
      switch (property_id) {
      case UIA_ControlTypePropertyId:
        result->vt = VT_I4;
        result->lVal = control_type(entry->role);
        break;
      case UIA_NamePropertyId:
        result->vt = VT_BSTR;
        result->bstrVal = SysAllocString(utf8_to_wide(entry->label).c_str());
        if (result->bstrVal == nullptr) {
          return E_OUTOFMEMORY;
        }
        break;
      case UIA_AutomationIdPropertyId: {
        const std::wstring automation_id = L"dui-" + std::to_wstring(id_);
        result->vt = VT_BSTR;
        result->bstrVal = SysAllocString(automation_id.c_str());
        if (result->bstrVal == nullptr) {
          return E_OUTOFMEMORY;
        }
        break;
      }
      case UIA_IsEnabledPropertyId:
        result->vt = VT_BOOL;
        result->boolVal = entry->enabled ? VARIANT_TRUE : VARIANT_FALSE;
        break;
      case UIA_ValueValuePropertyId:
        result->vt = VT_BSTR;
        result->bstrVal = SysAllocString(utf8_to_wide(entry->value).c_str());
        if (result->bstrVal == nullptr) {
          return E_OUTOFMEMORY;
        }
        break;
      case UIA_IsControlElementPropertyId:
      case UIA_IsContentElementPropertyId:
        result->vt = VT_BOOL;
        result->boolVal = VARIANT_TRUE;
        break;
      default:
        break;
      }
      return S_OK;
    } catch (...) {
      VariantClear(result);
      return E_FAIL;
    }
  }

  HRESULT STDMETHODCALLTYPE
  get_HostRawElementProvider(IRawElementProviderSimple** result) noexcept override {
    if (result == nullptr) {
      return E_POINTER;
    }
    *result = nullptr;
    return translate_com_call([&] {
      const std::scoped_lock lock{model_->mutex};
      if (!available()) {
        return element_not_available;
      }
      return id_ == 0 ? UiaHostProviderFromHwnd(model_->window, result) : S_OK;
    });
  }

  HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction,
                                     IRawElementProviderFragment** result) noexcept override {
    if (result == nullptr) {
      return E_POINTER;
    }
    *result = nullptr;
    try {
      const std::scoped_lock lock{model_->mutex};
      if (!available()) {
        return element_not_available;
      }
      std::optional<std::uint64_t> target;
      const std::vector<std::uint64_t>& children =
        id_ == 0 ? model_->roots : model_->nodes.at(id_).children;
      if (direction == NavigateDirection_FirstChild && !children.empty()) {
        target = children.front();
      } else if (direction == NavigateDirection_LastChild && !children.empty()) {
        target = children.back();
      } else if (id_ != 0) {
        const SemanticsEntry& entry = model_->nodes.at(id_).entry;
        const std::vector<std::uint64_t>& siblings =
          entry.parent_id.has_value() ? model_->nodes.at(*entry.parent_id).children : model_->roots;
        const auto position = std::find(siblings.begin(), siblings.end(), id_);
        if (direction == NavigateDirection_Parent) {
          target = entry.parent_id.value_or(0);
        } else if (direction == NavigateDirection_NextSibling && position != siblings.end() &&
                   std::next(position) != siblings.end()) {
          target = *std::next(position);
        } else if (direction == NavigateDirection_PreviousSibling && position != siblings.begin() &&
                   position != siblings.end()) {
          target = *std::prev(position);
        }
      }
      if (target.has_value()) {
        *result = static_cast<IRawElementProviderFragment*>(new NodeProvider{model_, *target});
      }
      return S_OK;
    } catch (...) {
      return E_FAIL;
    }
  }

  HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** result) noexcept override {
    if (result == nullptr) {
      return E_POINTER;
    }
    *result = nullptr;
    return translate_com_call([&] {
      const std::scoped_lock lock{model_->mutex};
      if (!available()) {
        return element_not_available;
      }
      if (id_ == 0) {
        return S_OK;
      }
      const int runtime_id[] = {UiaAppendRuntimeId, static_cast<int>(id_ & 0x7fffffffU),
                                static_cast<int>((id_ >> 31U) & 0x7fffffffU),
                                static_cast<int>(id_ >> 62U)};
      SAFEARRAY* value = SafeArrayCreateVector(VT_I4, 0, 4);
      if (value == nullptr) {
        return E_OUTOFMEMORY;
      }
      for (LONG index = 0; index < 4; ++index) {
        int part = runtime_id[index];
        if (FAILED(SafeArrayPutElement(value, &index, &part))) {
          SafeArrayDestroy(value);
          return E_FAIL;
        }
      }
      *result = value;
      return S_OK;
    });
  }

  HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* result) noexcept override {
    if (result == nullptr) {
      return E_POINTER;
    }
    *result = {};
    try {
      const std::scoped_lock lock{model_->mutex};
      if (!available()) {
        return element_not_available;
      }
      POINT origin{};
      if (ClientToScreen(model_->window, &origin) == FALSE) {
        return HRESULT_FROM_WIN32(GetLastError());
      }
      if (id_ == 0) {
        RECT client{};
        if (GetClientRect(model_->window, &client) == FALSE) {
          return HRESULT_FROM_WIN32(GetLastError());
        }
        *result = {static_cast<double>(origin.x), static_cast<double>(origin.y),
                   static_cast<double>(client.right - client.left),
                   static_cast<double>(client.bottom - client.top)};
      } else {
        const Rect bounds = model_->nodes.at(id_).entry.bounds;
        const double scale = model_->device_pixel_ratio;
        *result = {static_cast<double>(origin.x) + bounds.origin.x * scale,
                   static_cast<double>(origin.y) + bounds.origin.y * scale,
                   bounds.size.width * scale, bounds.size.height * scale};
      }
      return S_OK;
    } catch (...) {
      return E_FAIL;
    }
  }

  HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** result) noexcept override {
    if (result == nullptr) {
      return E_POINTER;
    }
    *result = nullptr;
    return translate_com_call([&] {
      const std::scoped_lock lock{model_->mutex};
      return available() ? S_OK : element_not_available;
    });
  }

  HRESULT STDMETHODCALLTYPE SetFocus() noexcept override {
    return translate_com_call([&] {
      const std::scoped_lock lock{model_->mutex};
      if (!available()) {
        return element_not_available;
      }
      if (id_ != 0) {
        return not_supported;
      }
      static_cast<void>(::SetFocus(model_->window));
      return ::GetFocus() == model_->window ? S_OK : E_FAIL;
    });
  }

  HRESULT STDMETHODCALLTYPE
  get_FragmentRoot(IRawElementProviderFragmentRoot** result) noexcept override {
    if (result == nullptr) {
      return E_POINTER;
    }
    *result = nullptr;
    try {
      const std::scoped_lock lock{model_->mutex};
      if (!available()) {
        return element_not_available;
      }
      auto* root = new NodeProvider{model_, 0};
      *result = static_cast<IRawElementProviderFragmentRoot*>(root);
      return S_OK;
    } catch (const std::bad_alloc&) {
      return E_OUTOFMEMORY;
    } catch (...) {
      return E_FAIL;
    }
  }

  HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(
    double x, double y, IRawElementProviderFragment** result) noexcept override {
    if (result == nullptr) {
      return E_POINTER;
    }
    *result = nullptr;
    try {
      const std::scoped_lock lock{model_->mutex};
      if (!available()) {
        return element_not_available;
      }
      POINT origin{};
      if (ClientToScreen(model_->window, &origin) == FALSE) {
        return HRESULT_FROM_WIN32(GetLastError());
      }
      const Offset point{(x - static_cast<double>(origin.x)) / model_->device_pixel_ratio,
                         (y - static_cast<double>(origin.y)) / model_->device_pixel_ratio};
      RECT client{};
      if (GetClientRect(model_->window, &client) == FALSE) {
        return HRESULT_FROM_WIN32(GetLastError());
      }
      if (x < static_cast<double>(origin.x) || y < static_cast<double>(origin.y) ||
          x > static_cast<double>(origin.x + client.right - client.left) ||
          y > static_cast<double>(origin.y + client.bottom - client.top)) {
        return S_OK;
      }
      std::uint64_t hit = 0;
      for (auto root = model_->roots.rbegin(); root != model_->roots.rend(); ++root) {
        if (const std::optional<std::uint64_t> candidate = hit_test(*root, point);
            candidate.has_value()) {
          hit = *candidate;
          break;
        }
      }
      *result = static_cast<IRawElementProviderFragment*>(new NodeProvider{model_, hit});
      return S_OK;
    } catch (...) {
      return E_FAIL;
    }
  }

  HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** result) noexcept override {
    if (result == nullptr) {
      return E_POINTER;
    }
    *result = nullptr;
    return translate_com_call([&] {
      const std::scoped_lock lock{model_->mutex};
      return available() ? S_OK : element_not_available;
    });
  }

  HRESULT STDMETHODCALLTYPE Invoke() noexcept override {
    try {
      const std::scoped_lock lock{model_->mutex};
      if (!available()) {
        return element_not_available;
      }
      const SemanticsEntry* entry = find_entry();
      if (entry == nullptr) {
        return element_not_available;
      }
      if (!entry->enabled) {
        return element_not_enabled;
      }
      if (!entry->supports(SemanticsAction::activate)) {
        return not_supported;
      }
      if (!model_->action_handler) {
        return not_supported;
      }
      std::uintptr_t token{};
      do {
        token = model_->next_action++;
      } while (token == 0 || model_->pending_actions.contains(token));
      model_->pending_actions.emplace(token, id_);
      if (PostMessageW(model_->window, model_->action_message, static_cast<WPARAM>(token), 0) ==
          FALSE) {
        model_->pending_actions.erase(token);
        return HRESULT_FROM_WIN32(GetLastError());
      }
      return S_OK;
    } catch (...) {
      return E_FAIL;
    }
  }

  HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR) noexcept override {
    return translate_com_call([&] {
      const std::scoped_lock lock{model_->mutex};
      return available() ? not_supported : element_not_available;
    });
  }

  HRESULT STDMETHODCALLTYPE get_Value(BSTR* result) noexcept override {
    if (result == nullptr) {
      return E_POINTER;
    }
    *result = nullptr;
    try {
      const std::scoped_lock lock{model_->mutex};
      if (!available()) {
        return element_not_available;
      }
      const SemanticsEntry* entry = find_entry();
      if (entry == nullptr) {
        return element_not_available;
      }
      const std::wstring value = utf8_to_wide(entry->value);
      *result = SysAllocString(value.c_str());
      return *result == nullptr ? E_OUTOFMEMORY : S_OK;
    } catch (...) {
      return E_FAIL;
    }
  }

  HRESULT STDMETHODCALLTYPE get_IsReadOnly(WINBOOL* result) noexcept override {
    if (result == nullptr) {
      return E_POINTER;
    }
    return translate_com_call([&] {
      const std::scoped_lock lock{model_->mutex};
      if (!available()) {
        return element_not_available;
      }
      *result = TRUE;
      return S_OK;
    });
  }

private:
  [[nodiscard]] const SemanticsEntry* find_entry() const {
    if (id_ == 0) {
      return nullptr;
    }
    const auto entry = model_->nodes.find(id_);
    return entry == model_->nodes.end() ? nullptr : &entry->second.entry;
  }

  [[nodiscard]] bool available() const {
    return model_->alive && (id_ == 0 || model_->nodes.contains(id_));
  }

  [[nodiscard]] bool supports_invoke() const {
    const SemanticsEntry* entry = find_entry();
    return entry != nullptr &&
           (entry->role == SemanticsRole::button || entry->supports(SemanticsAction::activate));
  }

  [[nodiscard]] bool supports_value() const {
    const SemanticsEntry* entry = find_entry();
    return entry != nullptr && !entry->value.empty();
  }

  [[nodiscard]] std::optional<std::uint64_t> hit_test(std::uint64_t id, Offset point) const {
    const NativeNode& node = model_->nodes.at(id);
    for (auto child = node.children.rbegin(); child != node.children.rend(); ++child) {
      if (const std::optional<std::uint64_t> candidate = hit_test(*child, point);
          candidate.has_value()) {
        return candidate;
      }
    }
    const Rect bounds = node.entry.bounds;
    if (point.x < bounds.origin.x || point.y < bounds.origin.y ||
        point.x > bounds.origin.x + bounds.size.width ||
        point.y > bounds.origin.y + bounds.size.height) {
      return std::nullopt;
    }
    return id;
  }

  std::atomic<ULONG> references_{1};
  std::shared_ptr<ProviderModel> model_;
  std::uint64_t id_{};
};

void rebuild_hierarchy(ProviderModel& model) {
  model.roots.clear();
  for (auto& [id, node] : model.nodes) {
    static_cast<void>(id);
    node.children.clear();
  }
  for (const auto& [id, node] : model.nodes) {
    if (!valid_rect(node.entry.bounds)) {
      throw std::invalid_argument("Win32 accessibility received invalid bounds");
    }
    if (node.entry.parent_id.has_value()) {
      const auto parent = model.nodes.find(*node.entry.parent_id);
      if (parent == model.nodes.end() || *node.entry.parent_id == id) {
        throw std::invalid_argument("Win32 accessibility received an invalid parent");
      }
      parent->second.children.push_back(id);
    } else {
      model.roots.push_back(id);
    }
    std::uint64_t ancestor = id;
    for (std::size_t depth = 0; depth <= model.nodes.size(); ++depth) {
      const std::optional<std::uint64_t> parent = model.nodes.at(ancestor).entry.parent_id;
      if (!parent.has_value()) {
        break;
      }
      if (depth == model.nodes.size()) {
        throw std::invalid_argument("Win32 accessibility received a parent cycle");
      }
      ancestor = *parent;
    }
  }
  const auto sort_children = [&model](std::vector<std::uint64_t>& children) {
    std::ranges::sort(children, {},
                      [&model](std::uint64_t id) { return model.nodes.at(id).entry.child_index; });
    for (std::size_t index = 0; index < children.size(); ++index) {
      if (model.nodes.at(children[index]).entry.child_index != index) {
        throw std::invalid_argument("Win32 accessibility received invalid sibling indexes");
      }
    }
  };
  sort_children(model.roots);
  for (auto& [id, node] : model.nodes) {
    static_cast<void>(id);
    sort_children(node.children);
  }
}

} // namespace

class AccessibilityAdapter::Impl {
public:
  Impl(NativeWindowHandle native_window, ActionHandler handler, double scale)
    : model_(std::make_shared<ProviderModel>()) {
    if (native_window == nullptr) {
      throw std::invalid_argument("Win32 accessibility requires a window handle");
    }
    if (!std::isfinite(scale) || scale <= 0.0) {
      throw std::invalid_argument("Win32 accessibility requires a positive finite DPR");
    }
    model_->window = static_cast<HWND>(native_window);
    model_->device_pixel_ratio = scale;
    model_->action_handler = std::move(handler);
    model_->action_message = RegisterWindowMessageW(L"dui.win32.accessibility.invoke");
    if (model_->action_message == 0) {
      throw std::runtime_error("Failed to register the Win32 accessibility action message");
    }
    root_ = new NodeProvider{model_, 0};
  }

  ~Impl() {
    {
      const std::scoped_lock lock{model_->mutex};
      model_->alive = false;
      model_->action_handler = {};
      model_->pending_actions.clear();
    }
    static_cast<void>(UiaDisconnectProvider(static_cast<IRawElementProviderSimple*>(root_)));
    root_->Release();
  }

  void apply(std::span<const SemanticsChange> changes) {
    {
      const std::scoped_lock lock{model_->mutex};
      ProviderModel next;
      next.nodes = model_->nodes;
      for (const SemanticsChange& change : changes) {
        if (change.entry.id == 0) {
          throw std::invalid_argument("Win32 accessibility requires nonzero semantic IDs");
        }
        switch (change.kind) {
        case SemanticsChangeKind::added:
          if (!next.nodes.emplace(change.entry.id, NativeNode{change.entry, {}}).second) {
            throw std::invalid_argument("Win32 accessibility added an existing node");
          }
          break;
        case SemanticsChangeKind::updated:
          if (!next.nodes.contains(change.entry.id)) {
            throw std::invalid_argument("Win32 accessibility updated a missing node");
          }
          next.nodes.at(change.entry.id).entry = change.entry;
          break;
        case SemanticsChangeKind::removed:
          if (next.nodes.erase(change.entry.id) != 1) {
            throw std::invalid_argument("Win32 accessibility removed a missing node");
          }
          break;
        }
      }
      rebuild_hierarchy(next);
      validate_scaled_bounds(next, model_->device_pixel_ratio);
      model_->nodes.swap(next.nodes);
      model_->roots.swap(next.roots);
    }
    if (!changes.empty()) {
      static_cast<void>(
        UiaRaiseStructureChangedEvent(static_cast<IRawElementProviderSimple*>(root_),
                                      StructureChangeType_ChildrenInvalidated, nullptr, 0));
    }
  }

  [[nodiscard]] std::optional<std::intptr_t> handle(std::uint32_t message, std::uintptr_t wparam,
                                                    std::intptr_t lparam) noexcept {
    const std::shared_ptr<ProviderModel> model = model_;
    if (message == model->action_message) {
      try {
        ActionHandler handler;
        std::uint64_t id{};
        {
          const std::scoped_lock lock{model->mutex};
          const auto pending = model->pending_actions.find(wparam);
          if (!model->alive || pending == model->pending_actions.end()) {
            return 0;
          }
          id = pending->second;
          model->pending_actions.erase(pending);
          if (!model->nodes.contains(id) || !model->nodes.at(id).entry.enabled ||
              !model->nodes.at(id).entry.supports(SemanticsAction::activate)) {
            return 0;
          }
          handler = model->action_handler;
        }
        if (handler) {
          static_cast<void>(handler(id, SemanticsAction::activate));
        }
      } catch (...) {
        return 0;
      }
      return 0;
    }
    if (message != WM_GETOBJECT || lparam != static_cast<std::intptr_t>(UiaRootObjectId)) {
      return std::nullopt;
    }
    try {
      {
        const std::scoped_lock lock{model->mutex};
        if (!model->alive) {
          return 0;
        }
      }
      return static_cast<std::intptr_t>(UiaReturnRawElementProvider(
        model_->window, static_cast<WPARAM>(wparam), static_cast<LPARAM>(lparam),
        static_cast<IRawElementProviderSimple*>(root_)));
    } catch (...) {
      return 0;
    }
  }

  void set_device_pixel_ratio(double scale) {
    if (!std::isfinite(scale) || scale <= 0.0) {
      throw std::invalid_argument("Win32 accessibility requires a positive finite DPR");
    }
    const std::scoped_lock lock{model_->mutex};
    validate_scaled_bounds(*model_, scale);
    model_->device_pixel_ratio = scale;
  }

private:
  std::shared_ptr<ProviderModel> model_;
  NodeProvider* root_{};
};

AccessibilityAdapter::AccessibilityAdapter(NativeWindowHandle window, ActionHandler action_handler,
                                           double device_pixel_ratio)
  : impl_(std::make_unique<Impl>(window, std::move(action_handler), device_pixel_ratio)) {}

AccessibilityAdapter::~AccessibilityAdapter() = default;
AccessibilityAdapter::AccessibilityAdapter(AccessibilityAdapter&&) noexcept = default;
AccessibilityAdapter& AccessibilityAdapter::operator=(AccessibilityAdapter&&) noexcept = default;

void AccessibilityAdapter::apply(std::span<const SemanticsChange> changes) {
  impl_->apply(changes);
}

void AccessibilityAdapter::set_device_pixel_ratio(double device_pixel_ratio) {
  impl_->set_device_pixel_ratio(device_pixel_ratio);
}

std::optional<std::intptr_t> AccessibilityAdapter::handle_message(std::uint32_t message,
                                                                  std::uintptr_t wparam,
                                                                  std::intptr_t lparam) noexcept {
  return impl_->handle(message, wparam, lparam);
}

} // namespace dui::win32
