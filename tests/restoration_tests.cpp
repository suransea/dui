#include "dui/ui.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <class Exception, class Operation> bool throws(Operation&& operation) {
  try {
    operation();
  } catch (const Exception&) {
    return true;
  }
  return false;
}

struct Handles {
  std::optional<dui::StateHandle<bool>> boolean;
  std::optional<dui::StateHandle<std::int64_t>> signed_value;
  std::optional<dui::StateHandle<std::uint64_t>> unsigned_value;
  std::optional<dui::StateHandle<std::string>> text;
};

struct RestorableValues {
  Handles* handles;
  std::vector<std::int64_t>* first_build_values;

  auto build(dui::BuildContext& context) const {
    auto boolean = context.restorable_state<"boolean">(false, "a.boolean");
    auto signed_value = context.restorable_state<"signed">(
      std::int64_t{-2}, "b.signed",
      [](const std::int64_t& value) { return std::to_string(value); });
    auto unsigned_value = context.restorable_state<"unsigned">(std::uint64_t{3}, "c.unsigned");
    auto text = context.restorable_state<"text">(std::string{"default"}, "d.text");
    handles->boolean = boolean;
    handles->signed_value = signed_value;
    handles->unsigned_value = unsigned_value;
    handles->text = text;
    first_build_values->push_back(signed_value.get());
    return dui::Text{text.get()};
  }
};

struct ConditionalState {
  bool restore;
  std::optional<dui::StateHandle<int>>* handle;

  auto build(dui::BuildContext& context) const {
    auto value =
      restore ? context.restorable_state<"value">(1, "conditional") : context.state<"value">(1);
    *handle = value;
    return dui::Text{std::to_string(value.get())};
  }
};

struct OneRestorable {
  std::string id;
  int initial;
  int* observed;

  auto build(dui::BuildContext& context) const {
    auto value = context.restorable_state<"value">(initial, id);
    *observed = value.get();
    return dui::Text{std::to_string(value.get())};
  }
};

struct DuplicateRestorationIds {
  auto build(dui::BuildContext& context) const {
    static_cast<void>(context.restorable_state<"first">(1, "duplicate"));
    static_cast<void>(context.restorable_state<"second">(2, "duplicate"));
    return dui::Text{"unreachable"};
  }
};

struct SignedByteState {
  auto build(dui::BuildContext& context) const {
    const auto value = context.restorable_state<"byte">(std::int8_t{}, "byte");
    return dui::Text{std::to_string(value.get())};
  }
};

struct UnsignedByteState {
  auto build(dui::BuildContext& context) const {
    const auto value = context.restorable_state<"byte">(std::uint8_t{}, "byte");
    return dui::Text{std::to_string(value.get())};
  }
};

struct ReentrantFormatter {
  dui::BuildOwner* owner;
  bool* save_rejected;
  bool* restore_rejected;
  bool* discard_rejected;

  auto build(dui::BuildContext& context) const {
    const auto value = context.restorable_state<"value">(
      1, "formatted",
      [owner = owner, save_rejected = save_rejected, restore_rejected = restore_rejected,
       discard_rejected = discard_rejected](const int& state) {
        *save_rejected =
          throws<std::logic_error>([&] { static_cast<void>(owner->save_restoration_state()); });
        *restore_rejected = throws<std::logic_error>([&] { owner->restore_state({}); });
        *discard_rejected = throws<std::logic_error>([&] { owner->discard_pending_restoration(); });
        return std::to_string(state);
      });
    return dui::Text{std::to_string(value.get())};
  }
};

struct MovingStringInitial {
  std::string* initial;
  std::optional<dui::StateHandle<std::string>>* handle;

  auto build(dui::BuildContext& context) const {
    auto value = context.restorable_state<"text">(std::move(*initial), "moving");
    *handle = value;
    return dui::Text{value.get()};
  }
};

struct ChildRestorable {
  std::string id;

  auto build(dui::BuildContext& context) const {
    const auto value = context.restorable_state<"value">(1, id);
    return dui::Text{std::to_string(value.get())};
  }
};

struct DuplicateAcrossElements {
  auto build(dui::BuildContext&) const {
    return dui::VStack{ChildRestorable{"shared"}, ChildRestorable{"shared"}};
  }
};

struct LazyItem {
  int id;
  std::vector<std::optional<dui::StateHandle<int>>>* handles;

  auto build(dui::BuildContext& context) const {
    auto value = context.restorable_state<"value">(id, "lazy." + std::to_string(id));
    handles->at(static_cast<std::size_t>(id)) = value;
    return dui::Text{"item " + std::to_string(id) + ':' + std::to_string(value.get())};
  }
};

void replacement_owner_restores_supported_values() {
  dui::RestorationSnapshot snapshot;
  std::optional<dui::StateHandle<std::int64_t>> stale_handle;
  {
    Handles handles;
    std::vector<std::int64_t> observed;
    dui::BuildOwner owner;
    owner.render(RestorableValues{&handles, &observed});
    handles.boolean->set(true);
    handles.signed_value->set(std::numeric_limits<std::int64_t>::min());
    handles.unsigned_value->set(std::numeric_limits<std::uint64_t>::max());
    handles.text->set(std::string{"restored\0text", 13});
    stale_handle = handles.signed_value;
    snapshot = owner.save_restoration_state();
    require(snapshot.records.size() == 4 && snapshot.records[0].id == "a.boolean" &&
              snapshot.records[1].id == "b.signed" && snapshot.records[2].id == "c.unsigned" &&
              snapshot.records[3].id == "d.text" && std::get<bool>(snapshot.records[0].value) &&
              std::get<std::int64_t>(snapshot.records[1].value) ==
                std::numeric_limits<std::int64_t>::min() &&
              std::get<std::uint64_t>(snapshot.records[2].value) ==
                std::numeric_limits<std::uint64_t>::max() &&
              std::get<std::string>(snapshot.records[3].value) == std::string{"restored\0text", 13},
            "restoration snapshot lost values or canonical ID order");

    Handles replacement_handles;
    std::vector<std::int64_t> replacement_observed;
    dui::BuildOwner replacement;
    replacement.restore_state(snapshot);
    require(replacement.pending_restoration_count() == 4,
            "replacement owner did not stage restoration records");
    replacement.render(RestorableValues{&replacement_handles, &replacement_observed});
    require(
      replacement_observed == std::vector<std::int64_t>{std::numeric_limits<std::int64_t>::min()} &&
        replacement_handles.boolean->get() &&
        replacement_handles.unsigned_value->get() == std::numeric_limits<std::uint64_t>::max() &&
        replacement_handles.text->get() == std::string{"restored\0text", 13} &&
        replacement.pending_restoration_count() == 0 && replacement.pending_build_count() == 0 &&
        stale_handle->get() == std::numeric_limits<std::int64_t>::min(),
      "replacement owner did not expose restored values during its first build");
    const auto inspected = replacement.inspect();
    require(inspected.root.has_value() && inspected.root->state_values.size() == 1 &&
              inspected.root->state_values[0] ==
                dui::InspectorStateSnapshot{
                  "signed", std::to_string(std::numeric_limits<std::int64_t>::min())},
            "restored state did not compose with inspector formatting");
  }
  require(throws<std::logic_error>([&] { static_cast<void>(stale_handle->get()); }),
          "old state handle survived destruction of its original owner");
}

void privacy_revocation_and_id_validation_are_explicit() {
  std::optional<dui::StateHandle<int>> handle;
  dui::BuildOwner owner;
  owner.render(ConditionalState{true, &handle});
  handle->set(8);
  require(owner.save_restoration_state().records.size() == 1,
          "restorable state was omitted from capture");
  owner.render(ConditionalState{false, &handle});
  require(owner.save_restoration_state().records.empty() && handle->get() == 8,
          "ordinary state declaration did not revoke restoration without changing state");

  dui::BuildOwner adoption;
  adoption.restore_state({{{"conditional", std::int64_t{99}}}});
  adoption.render(ConditionalState{false, &handle});
  adoption.render(ConditionalState{true, &handle});
  require(handle->get() == 1 && adoption.pending_restoration_count() == 0 &&
            std::get<std::int64_t>(adoption.save_restoration_state().records[0].value) == 1,
          "an existing live slot did not supersede pending restoration when opting in");

  dui::BuildOwner duplicate;
  require(throws<std::logic_error>([&] { duplicate.render(DuplicateRestorationIds{}); }),
          "duplicate live restoration IDs were accepted");
  dui::BuildOwner duplicate_elements;
  require(throws<std::logic_error>([&] { duplicate_elements.render(DuplicateAcrossElements{}); }),
          "duplicate restoration IDs across Elements were accepted");
  int ignored = 0;
  dui::BuildOwner empty;
  require(throws<std::invalid_argument>([&] {
            empty.render(OneRestorable{"", 1, &ignored});
          }),
          "empty restoration ID was accepted");

  std::string initial{"first"};
  std::optional<dui::StateHandle<std::string>> string_handle;
  dui::BuildOwner rebuilding;
  rebuilding.render(MovingStringInitial{&initial, &string_handle});
  initial = "must remain";
  rebuilding.render(MovingStringInitial{&initial, &string_handle});
  require(initial == "must remain" && string_handle->get() == "first",
          "rebuilding an existing restorable slot consumed its ignored initial value");
}

void pending_records_survive_and_fail_transactionally() {
  dui::RestorationSnapshot snapshot{{{"later", std::int64_t{9}}, {"known", std::int64_t{7}}}};
  dui::BuildOwner owner;
  owner.restore_state(snapshot);
  int observed = 0;
  owner.render(OneRestorable{"known", 1, &observed});
  require(observed == 7 && owner.pending_restoration_count() == 1,
          "matching restoration did not preserve unrelated pending state");
  const auto carried = owner.save_restoration_state();
  require(carried.records.size() == 2 && carried.records[0].id == "known" &&
            carried.records[1].id == "later",
          "capture did not merge live and pending records deterministically");

  dui::BuildOwner repeated;
  repeated.restore_state(carried);
  require(repeated.save_restoration_state() == carried,
          "unclaimed records did not survive a repeated replacement");
  repeated.discard_pending_restoration();
  require(repeated.pending_restoration_count() == 0 &&
            repeated.save_restoration_state().records.empty(),
          "explicit pending restoration discard was ineffective");

  dui::BuildOwner mismatch;
  mismatch.restore_state({{{"byte", std::int64_t{300}}}});
  require(throws<std::invalid_argument>([&] { mismatch.render(SignedByteState{}); }) &&
            mismatch.pending_restoration_count() == 1,
          "out-of-range restoration was consumed or accepted");

  dui::BuildOwner category;
  category.restore_state({{{"known", std::uint64_t{7}}}});
  require(throws<std::invalid_argument>([&] {
            category.render(OneRestorable{"known", 1, &observed});
          }) &&
            category.pending_restoration_count() == 1,
          "mismatched restoration category was consumed or accepted");

  dui::BuildOwner unsigned_range;
  unsigned_range.restore_state({{{"byte", std::uint64_t{256}}}});
  require(throws<std::invalid_argument>([&] { unsigned_range.render(UnsignedByteState{}); }) &&
            unsigned_range.pending_restoration_count() == 1,
          "out-of-range unsigned restoration was consumed or accepted");

  dui::BuildOwner reverse_category;
  reverse_category.restore_state({{{"byte", std::int64_t{7}}}});
  require(throws<std::invalid_argument>([&] { reverse_category.render(UnsignedByteState{}); }) &&
            reverse_category.pending_restoration_count() == 1,
          "signed restoration was accepted for unsigned state");
}

void snapshot_installation_is_pristine_and_transactional() {
  dui::BuildOwner owner;
  owner.restore_state({{{"original", std::int64_t{1}}}});
  require(throws<std::invalid_argument>([&] {
            owner.restore_state({{{"duplicate", std::int64_t{1}}, {"duplicate", std::int64_t{2}}}});
          }) &&
            owner.pending_restoration_count() == 1 &&
            owner.save_restoration_state().records[0].id == "original",
          "invalid snapshot replaced previously staged restoration");
  require(throws<std::invalid_argument>([&] {
            owner.restore_state({{{"", false}}});
          }) &&
            owner.save_restoration_state().records[0].id == "original",
          "empty snapshot ID replaced previously staged restoration");

  dui::BuildOwner byte_order;
  const std::string embedded{"a\0z", 3};
  const std::string high_byte{static_cast<char>(0xff)};
  byte_order.restore_state({{{high_byte, false}, {embedded, false}, {"a", false}}});
  const auto ordered = byte_order.save_restoration_state();
  require(ordered.records[0].id == "a" && ordered.records[1].id == embedded &&
            ordered.records[2].id == high_byte,
          "restoration IDs were not sorted in bytewise order");

  int observed = 0;
  owner.render(OneRestorable{"original", 0, &observed});
  require(throws<std::logic_error>([&] { owner.restore_state({}); }),
          "owner accepted restoration after mounting an Element");
}

void dormant_and_reentrant_state_are_covered() {
  struct Item {
    int id;
  };
  const std::vector<Item> items{{0}, {1}, {2}};
  std::vector<std::optional<dui::StateHandle<int>>> handles(items.size());
  const auto make_view = [&](double offset,
                             std::vector<std::optional<dui::StateHandle<int>>>* target_handles) {
    auto source =
      dui::lazy_for_each(items, dui::key<&Item::id>, [target_handles](const Item& item) {
        return LazyItem{item.id, target_handles};
      }).keep_alive_when([](const Item& item) { return item.id == 0; });
    return dui::Viewport{
      offset, dui::SliverFixedExtentList{16.0, dui::cache_extent(0.0), std::move(source)}};
  };

  dui::BuildOwner owner;
  owner.render(make_view(0.0, &handles));
  static_cast<void>(owner.frame(dui::BoxConstraints::tight({100.0, 16.0})));
  handles[0]->set(42);
  owner.render(make_view(16.0, &handles));
  static_cast<void>(owner.frame(dui::BoxConstraints::tight({100.0, 16.0})));
  const auto snapshot = owner.save_restoration_state();
  require(snapshot.records.size() == 2 && snapshot.records[0].id == "lazy.0" &&
            std::get<std::int64_t>(snapshot.records[0].value) == 42 &&
            snapshot.records[1].id == "lazy.1",
          "restoration capture omitted dormant or active lazy state");

  std::vector<std::optional<dui::StateHandle<int>>> restored_handles(items.size());
  dui::BuildOwner replacement;
  replacement.restore_state(snapshot);
  replacement.render(make_view(16.0, &restored_handles));
  static_cast<void>(replacement.frame(dui::BoxConstraints::tight({100.0, 16.0})));
  require(replacement.pending_restoration_count() == 1 && restored_handles[1].has_value() &&
            !restored_handles[0].has_value(),
          "unrealized lazy restoration was consumed before its first mount");
  replacement.render(make_view(0.0, &restored_handles));
  static_cast<void>(replacement.frame(dui::BoxConstraints::tight({100.0, 16.0})));
  require(restored_handles[0].has_value() && restored_handles[0]->get() == 42 &&
            replacement.pending_restoration_count() == 0,
          "lazy state was not restored when its item was first realized");

  bool save_rejected = false;
  bool restore_rejected = false;
  bool discard_rejected = false;
  dui::BuildOwner formatter_owner;
  formatter_owner.render(
    ReentrantFormatter{&formatter_owner, &save_rejected, &restore_rejected, &discard_rejected});
  require(save_rejected && restore_rejected && discard_rejected,
          "restoration owner operations reentered an inspector formatter");
}

} // namespace

int main() {
  try {
    replacement_owner_restores_supported_values();
    privacy_revocation_and_id_validation_are_explicit();
    pending_records_survive_and_fail_transactionally();
    snapshot_installation_is_pristine_and_transactional();
    dormant_and_reentrant_state_are_covered();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "All restoration tests passed\n";
  return 0;
}
