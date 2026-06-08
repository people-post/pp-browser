#include "demo/SearchDemo.h"

#include "app/Application.h"
#include "ui/DataModelHost.h"
#include "ui/DocumentLoader.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <vector>

namespace ppbrowser {

namespace {

struct SearchRow {
  Rml::String name;
  Rml::String email;
};

struct DemoData {
  Rml::String query;
  std::vector<SearchRow> results;
  bool loading = false;
  Rml::String error;
};

DemoData g_demo;

void SearchUsers(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/, const Rml::VariantList& /*args*/) {
  g_demo.loading = true;
  g_demo.error = "";
  g_demo.results.clear();
  DataModelHost::Instance().Dirty("main", "loading");
  DataModelHost::Instance().Dirty("main", "results");
  DataModelHost::Instance().Dirty("main", "error");

  if (g_demo.query.empty()) {
    g_demo.loading = false;
    g_demo.error = "Enter a search query.";
    DataModelHost::Instance().Dirty("main", "loading");
    DataModelHost::Instance().Dirty("main", "error");
    return;
  }

  g_demo.results.push_back({Rml::String("Ada Lovelace"), Rml::String("ada@example.com")});
  g_demo.results.push_back({Rml::String("Grace Hopper"), Rml::String("grace@example.com")});
  g_demo.loading = false;
  DataModelHost::Instance().Dirty("main", "loading");
  DataModelHost::Instance().Dirty("main", "results");
}

} // namespace

bool SetupSearchDemo(Rml::Context* context) {
  if (!context) {
    return false;
  }

  g_demo = {};

  DataModelHost::Instance().Clear();

  if (!DataModelHost::Instance().Register(context, "main", [](Rml::DataModelConstructor& ctor) {
        if (auto row_handle = ctor.RegisterStruct<SearchRow>()) {
          row_handle.RegisterMember("name", &SearchRow::name);
          row_handle.RegisterMember("email", &SearchRow::email);
        }
        ctor.RegisterArray<std::vector<SearchRow>>();
        ctor.Bind("query", &g_demo.query);
        ctor.Bind("loading", &g_demo.loading);
        ctor.Bind("error", &g_demo.error);
        ctor.Bind("results", &g_demo.results);
        ctor.BindEventCallback("search_users", &SearchUsers);
      })) {
    return false;
  }

  return DocumentLoader::LoadFile(context, Application::AssetsPath("samples/search_demo.rml")) != nullptr;
}

} // namespace ppbrowser
