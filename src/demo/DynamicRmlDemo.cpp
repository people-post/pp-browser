#include "demo/DynamicRmlDemo.h"

#include "app/Application.h"
#include "ui/DataModelHost.h"
#include "ui/DocumentLoader.h"
#include "ui/RmlMount.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/ElementDocument.h>

namespace pbr {

namespace {

struct DynamicDemoData {
  Rml::String draft;
  Rml::String status;
  bool theme_alt = false;
};

DynamicDemoData g_demo;

Rml::Element* MountTarget(Rml::Context* context) {
  if (!context || context->GetNumDocuments() == 0) {
    return nullptr;
  }
  return context->GetDocument(0)->GetElementById("mount-target");
}

Rml::ElementDocument* ActiveDocument(Rml::Context* context) {
  if (!context || context->GetNumDocuments() == 0) {
    return nullptr;
  }
  return context->GetDocument(0);
}

const char* PanelFragmentA() {
  return R"frag(
    <div class="stack dynamic-panel">
      <h2>Panel A</h2>
      <p class="muted">Mounted via RmlMount::MountInner</p>
      <textarea id="draft-input" placeholder="Type here, then swap panels..." rows="3" data-value="draft"></textarea>
      <div class="scroll-list" data-mount-id="list">
        <p>Scroll item 1</p>
        <p>Scroll item 2</p>
        <p>Scroll item 3</p>
        <p>Scroll item 4</p>
        <p>Scroll item 5</p>
        <p>Scroll item 6</p>
        <p>Scroll item 7</p>
        <p>Scroll item 8</p>
        <p>Scroll item 9</p>
        <p>Scroll item 10</p>
        <p>Scroll item 11</p>
        <p>Scroll item 12</p>
      </div>
      <button data-event-click="panel_action('A clicked')">Test injected binding</button>
    </div>
  )frag";
}

const char* PanelFragmentB() {
  return R"frag(
    <div class="stack dynamic-panel dynamic-panel-b">
      <h2>Panel B</h2>
      <p class="muted">Different markup, same data model bindings</p>
      <textarea id="draft-input" placeholder="Focus should return here after swap..." rows="3" data-value="draft"></textarea>
      <div class="scroll-list" data-mount-id="list">
        <p>B scroll row 1</p>
        <p>B scroll row 2</p>
        <p>B scroll row 3</p>
        <p>B scroll row 4</p>
        <p>B scroll row 5</p>
        <p>B scroll row 6</p>
        <p>B scroll row 7</p>
        <p>B scroll row 8</p>
        <p>B scroll row 9</p>
        <p>B scroll row 10</p>
        <p>B scroll row 11</p>
        <p>B scroll row 12</p>
      </div>
      <button data-event-click="panel_action('B clicked')">Test injected binding</button>
    </div>
  )frag";
}

void SetStatus(const Rml::String& message) {
  g_demo.status = message;
  DataModelHost::Instance().Dirty("dynamic", "status");
}

void SwapPanelA(Rml::DataModelHandle /*model*/, Rml::Event& ev, const Rml::VariantList& /*args*/) {
  Rml::Context* context = ev.GetTargetElement()->GetContext();
  if (Rml::Element* target = MountTarget(context)) {
    if (DocumentLoader::MountFragment(target, PanelFragmentA())) {
      SetStatus("Mounted panel A");
    } else {
      SetStatus("Failed to mount panel A");
    }
  }
}

void SwapPanelB(Rml::DataModelHandle /*model*/, Rml::Event& ev, const Rml::VariantList& /*args*/) {
  Rml::Context* context = ev.GetTargetElement()->GetContext();
  if (Rml::Element* target = MountTarget(context)) {
    if (DocumentLoader::MountFragment(target, PanelFragmentB())) {
      SetStatus("Mounted panel B");
    } else {
      SetStatus("Failed to mount panel B");
    }
  }
}

void PanelAction(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/, const Rml::VariantList& args) {
  if (!args.empty() && args[0].GetType() == Rml::Variant::STRING) {
    SetStatus(args[0].Get<Rml::String>());
  }
}

void InjectTheme(Rml::DataModelHandle /*model*/, Rml::Event& ev, const Rml::VariantList& /*args*/) {
  Rml::Context* context = ev.GetTargetElement()->GetContext();
  Rml::ElementDocument* doc = ActiveDocument(context);
  if (!doc) {
    SetStatus("No active document for theme inject");
    return;
  }

  g_demo.theme_alt = !g_demo.theme_alt;
  const char* rcss = g_demo.theme_alt
                         ? R"(
        .dynamic-demo-shell { background-color: #1a2e1a; }
        .dynamic-panel h2 { color: #7cfc7c; }
        .dynamic-toolbar button { background-color: #3a6a3a; }
      )"
                         : R"(
        .dynamic-demo-shell { background-color: #1a1a2e; }
        .dynamic-panel h2 { color: #ffffff; }
        .dynamic-toolbar button { background-color: #4a6cf7; }
      )";

  if (RmlMount::InjectRcss(doc, rcss, "dynamic-theme")) {
    SetStatus(g_demo.theme_alt ? "Injected alternate theme" : "Restored default theme");
  } else {
    SetStatus("Theme inject failed");
  }
  DataModelHost::Instance().Dirty("dynamic", "theme_alt");
}

} // namespace

bool SetupDynamicRmlDemo(Rml::Context* context) {
  if (!context) {
    return false;
  }

  g_demo = {};
  g_demo.status = "Ready";

  DataModelHost::Instance().Clear();

  if (!DataModelHost::Instance().Register(context, "dynamic", [](Rml::DataModelConstructor& ctor) {
        ctor.Bind("draft", &g_demo.draft);
        ctor.Bind("status", &g_demo.status);
        ctor.Bind("theme_alt", &g_demo.theme_alt);
        ctor.BindEventCallback("swap_panel_a", &SwapPanelA);
        ctor.BindEventCallback("swap_panel_b", &SwapPanelB);
        ctor.BindEventCallback("panel_action", &PanelAction);
        ctor.BindEventCallback("inject_theme", &InjectTheme);
      })) {
    return false;
  }

  if (DocumentLoader::LoadFile(context, Application::AssetsPath("samples/dynamic_rml_demo.rml")) == nullptr) {
    return false;
  }

  if (Rml::Element* target = MountTarget(context)) {
    DocumentLoader::MountFragment(target, PanelFragmentA());
  }

  return true;
}

} // namespace pbr
