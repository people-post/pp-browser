#include "gui/shell/RmlMount.h"

#include "domain/ai/RmlValidator.h"
#include "common/Logger.h"

#include <ui/dom/Context.h>
#include <ui/data/DataModelHandle.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/base/StreamMemory.h>
#include <ui/style/StyleSheetContainer.h>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

logging::Logger& MountLog() {
  static logging::Logger log = logging::getLogger("RmlMount");
  return log;
}

} // namespace

struct RmlMount::DocumentStyleState {
  ui::SharedPtr<ui::StyleSheetContainer> base;
  std::unordered_map<std::string, std::string> injected;
};

std::unordered_map<ui::ElementDocument*, RmlMount::DocumentStyleState> RmlMount::style_state_;

bool RmlMount::IsDescendantOf(ui::Element* ancestor, ui::Element* node) {
  while (node) {
    if (node == ancestor) {
      return true;
    }
    node = node->GetParentNode();
  }
  return false;
}

void RmlMount::CollectScrollState(ui::Element* element, MountState& state) {
  if (!element) {
    return;
  }

  if (element->HasAttribute("data-mount-id")) {
    const ui::Variant* id_variant = element->GetAttribute("data-mount-id");
    if (id_variant && id_variant->GetType() == ui::Variant::STRING) {
      state.scroll_positions.push_back(
          {id_variant->Get<ui::String>(), {element->GetScrollLeft(), element->GetScrollTop()}});
    }
  }

  const int child_count = element->GetNumChildren();
  for (int i = 0; i < child_count; ++i) {
    CollectScrollState(element->GetChild(i), state);
  }
}

void RmlMount::RestoreScrollState(ui::Element* element, const MountState& state) {
  if (!element) {
    return;
  }

  if (element->HasAttribute("data-mount-id")) {
    const ui::Variant* id_variant = element->GetAttribute("data-mount-id");
    if (id_variant && id_variant->GetType() == ui::Variant::STRING) {
      const ui::String& mount_id = id_variant->Get<ui::String>();
      for (const auto& entry : state.scroll_positions) {
        if (entry.first == mount_id) {
          element->SetScrollLeft(entry.second.x);
          element->SetScrollTop(entry.second.y);
          break;
        }
      }
    }
  }

  const int child_count = element->GetNumChildren();
  for (int i = 0; i < child_count; ++i) {
    RestoreScrollState(element->GetChild(i), state);
  }
}

MountState RmlMount::CaptureState(ui::Element* subtree) {
  MountState state;
  if (!subtree) {
    return state;
  }

  if (ui::Context* context = subtree->GetContext()) {
    if (ui::Element* focus = context->GetFocusElement()) {
      if (IsDescendantOf(subtree, focus)) {
        state.focused_id = focus->GetId();
      }
    }
  }

  CollectScrollState(subtree, state);
  return state;
}

void RmlMount::RestoreState(ui::Element* subtree, const MountState& state) {
  if (!subtree) {
    return;
  }

  if (!state.focused_id.empty()) {
    if (ui::Element* focus_target = subtree->GetElementById(state.focused_id)) {
      focus_target->Focus();
    }
  }

  RestoreScrollState(subtree, state);
}

bool RmlMount::MountInner(ui::Element* target, const std::string& rml, MountOptions opts) {
  if (!target) {
    MountLog().error << "MountInner: null target element";
    return false;
  }

  if (opts.validate) {
    const ValidationResult validation = RmlValidator::ValidateFragment(rml);
    if (!validation.ok) {
      for (const std::string& error : validation.errors) {
        MountLog().error << "MountInner validation failed: " << error;
      }
      return false;
    }
  }

  MountState saved_state;
  if (opts.preserve_focus || opts.preserve_scroll) {
    saved_state = CaptureState(target);
  }

  target->SetInnerRML(rml.c_str());

  if (opts.preserve_focus || opts.preserve_scroll) {
    MountState restore = saved_state;
    if (!opts.preserve_focus) {
      restore.focused_id.clear();
    }
    if (!opts.preserve_scroll) {
      restore.scroll_positions.clear();
    }
    RestoreState(target, restore);
  }

  if (ui::ElementDocument* document = target->GetOwnerDocument()) {
    document->UpdateDocument();
  }

  // SetInnerRML attaches data views into views_to_add; they only apply on DataModel::Update.
  // Flush now so data-if / data-rml match C++ state in this turn (Dirty alone after remount
  // used to race: Present / idle could paint before the next Context::Update).
  if (ui::Context* context = target->GetContext()) {
    for (auto& entry : context->GetDataModels()) {
      if (ui::DataModelHandle handle = entry.second.GetModelHandle()) {
        handle.Update(true);
      }
    }
  }

  return true;
}

bool RmlMount::ReapplyInjectedStyles(ui::ElementDocument* doc, DocumentStyleState& state) {
  if (!state.base) {
    MountLog().error << "InjectRcss: missing base stylesheet snapshot";
    return false;
  }

  ui::SharedPtr<ui::StyleSheetContainer> merged = state.base->CombineStyleSheetContainer(ui::StyleSheetContainer());
  if (!merged) {
    MountLog().error << "InjectRcss: failed to clone base stylesheet";
    return false;
  }

  for (const auto& [tag, css] : state.injected) {
    auto sheet = ui::MakeShared<ui::StyleSheetContainer>();
    auto stream = ui::MakeUnique<ui::StreamMemory>(reinterpret_cast<const ui::byte*>(css.c_str()), css.size());
    stream->SetSourceURL(tag.c_str());
    if (!sheet->LoadStyleSheetContainer(stream.get())) {
      MountLog().error << "InjectRcss: failed to parse RCSS for tag '" << tag << "'";
      return false;
    }
    merged->MergeStyleSheetContainer(*sheet);
  }

  doc->SetStyleSheetContainer(std::move(merged));
  doc->UpdateDocument();
  return true;
}

bool RmlMount::InjectRcss(ui::ElementDocument* doc, const std::string& rcss, const std::string& source_tag) {
  if (!doc || rcss.empty()) {
    MountLog().error << "InjectRcss: invalid document or empty RCSS";
    return false;
  }

  auto test_sheet = ui::MakeShared<ui::StyleSheetContainer>();
  auto test_stream =
      ui::MakeUnique<ui::StreamMemory>(reinterpret_cast<const ui::byte*>(rcss.c_str()), rcss.size());
  test_stream->SetSourceURL(source_tag.c_str());
  if (!test_sheet->LoadStyleSheetContainer(test_stream.get())) {
    MountLog().error << "InjectRcss: failed to parse RCSS";
    return false;
  }

  DocumentStyleState& state = style_state_[doc];
  if (!state.base) {
    if (const ui::StyleSheetContainer* current = doc->GetStyleSheetContainer()) {
      state.base = current->CombineStyleSheetContainer(ui::StyleSheetContainer());
    } else {
      state.base = ui::MakeShared<ui::StyleSheetContainer>();
    }
  }

  state.injected[source_tag] = rcss;
  return ReapplyInjectedStyles(doc, state);
}

void RmlMount::ClearDocumentStyleState(ui::ElementDocument* doc) {
  if (doc) {
    style_state_.erase(doc);
  }
}

} // namespace pbr
