#include "ui/RmlMount.h"

#include "agent/RmlValidator.h"
#include "common/Logger.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/StreamMemory.h>
#include <RmlUi/Core/StyleSheetContainer.h>

namespace pbr {

namespace {

logging::Logger& MountLog() {
  static logging::Logger log = logging::getLogger("RmlMount");
  return log;
}

} // namespace

struct RmlMount::DocumentStyleState {
  Rml::SharedPtr<Rml::StyleSheetContainer> base;
  std::unordered_map<std::string, std::string> injected;
};

std::unordered_map<Rml::ElementDocument*, RmlMount::DocumentStyleState> RmlMount::style_state_;

bool RmlMount::IsDescendantOf(Rml::Element* ancestor, Rml::Element* node) {
  while (node) {
    if (node == ancestor) {
      return true;
    }
    node = node->GetParentNode();
  }
  return false;
}

void RmlMount::CollectScrollState(Rml::Element* element, MountState& state) {
  if (!element) {
    return;
  }

  if (element->HasAttribute("data-mount-id")) {
    const Rml::Variant* id_variant = element->GetAttribute("data-mount-id");
    if (id_variant && id_variant->GetType() == Rml::Variant::STRING) {
      state.scroll_positions.push_back(
          {id_variant->Get<Rml::String>(), {element->GetScrollLeft(), element->GetScrollTop()}});
    }
  }

  const int child_count = element->GetNumChildren();
  for (int i = 0; i < child_count; ++i) {
    CollectScrollState(element->GetChild(i), state);
  }
}

void RmlMount::RestoreScrollState(Rml::Element* element, const MountState& state) {
  if (!element) {
    return;
  }

  if (element->HasAttribute("data-mount-id")) {
    const Rml::Variant* id_variant = element->GetAttribute("data-mount-id");
    if (id_variant && id_variant->GetType() == Rml::Variant::STRING) {
      const Rml::String& mount_id = id_variant->Get<Rml::String>();
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

MountState RmlMount::CaptureState(Rml::Element* subtree) {
  MountState state;
  if (!subtree) {
    return state;
  }

  if (Rml::Context* context = subtree->GetContext()) {
    if (Rml::Element* focus = context->GetFocusElement()) {
      if (IsDescendantOf(subtree, focus)) {
        state.focused_id = focus->GetId();
      }
    }
  }

  CollectScrollState(subtree, state);
  return state;
}

void RmlMount::RestoreState(Rml::Element* subtree, const MountState& state) {
  if (!subtree) {
    return;
  }

  if (!state.focused_id.empty()) {
    if (Rml::Element* focus_target = subtree->GetElementById(state.focused_id)) {
      focus_target->Focus();
    }
  }

  RestoreScrollState(subtree, state);
}

bool RmlMount::MountInner(Rml::Element* target, const std::string& rml, MountOptions opts) {
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

  if (Rml::ElementDocument* document = target->GetOwnerDocument()) {
    document->UpdateDocument();
  }

  return true;
}

bool RmlMount::ReapplyInjectedStyles(Rml::ElementDocument* doc, DocumentStyleState& state) {
  if (!state.base) {
    MountLog().error << "InjectRcss: missing base stylesheet snapshot";
    return false;
  }

  Rml::SharedPtr<Rml::StyleSheetContainer> merged = state.base->CombineStyleSheetContainer(Rml::StyleSheetContainer());
  if (!merged) {
    MountLog().error << "InjectRcss: failed to clone base stylesheet";
    return false;
  }

  for (const auto& [tag, css] : state.injected) {
    auto sheet = Rml::MakeShared<Rml::StyleSheetContainer>();
    auto stream = Rml::MakeUnique<Rml::StreamMemory>(reinterpret_cast<const Rml::byte*>(css.c_str()), css.size());
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

bool RmlMount::InjectRcss(Rml::ElementDocument* doc, const std::string& rcss, const std::string& source_tag) {
  if (!doc || rcss.empty()) {
    MountLog().error << "InjectRcss: invalid document or empty RCSS";
    return false;
  }

  auto test_sheet = Rml::MakeShared<Rml::StyleSheetContainer>();
  auto test_stream =
      Rml::MakeUnique<Rml::StreamMemory>(reinterpret_cast<const Rml::byte*>(rcss.c_str()), rcss.size());
  test_stream->SetSourceURL(source_tag.c_str());
  if (!test_sheet->LoadStyleSheetContainer(test_stream.get())) {
    MountLog().error << "InjectRcss: failed to parse RCSS";
    return false;
  }

  DocumentStyleState& state = style_state_[doc];
  if (!state.base) {
    if (const Rml::StyleSheetContainer* current = doc->GetStyleSheetContainer()) {
      state.base = current->CombineStyleSheetContainer(Rml::StyleSheetContainer());
    } else {
      state.base = Rml::MakeShared<Rml::StyleSheetContainer>();
    }
  }

  state.injected[source_tag] = rcss;
  return ReapplyInjectedStyles(doc, state);
}

void RmlMount::ClearDocumentStyleState(Rml::ElementDocument* doc) {
  if (doc) {
    style_state_.erase(doc);
  }
}

} // namespace pbr
