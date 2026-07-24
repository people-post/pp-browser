#if defined(_WIN32)

#include "base/platform/desktop/LocalNotifierImpl.h"

#include "base/platform/ProductBranding.h"
#include "common/Logger.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <roapi.h>
#include <windows.ui.notifications.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

#pragma comment(lib, "runtimeobject.lib")

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;
using namespace ABI::Windows::Data::Xml::Dom;
using namespace ABI::Windows::Foundation;
using namespace ABI::Windows::UI::Notifications;

namespace pbr::desktop {
namespace {

std::mutex g_mu;
bool g_init_attempted = false;
bool g_init_ok = false;
bool g_logged_fail = false;
bool g_ro_initialized = false;
ComPtr<IToastNotifier> g_notifier;
ComPtr<IToastNotificationFactory> g_factory;
ComPtr<IToastNotificationManagerStatics> g_manager;
ComPtr<IToastNotificationManagerStatics2> g_manager2;
ComPtr<IToastNotificationHistory> g_history;
std::unordered_map<std::string, std::string> g_thread_to_tag;

void LogFailOnce(const char* detail) {
  if (g_logged_fail) {
    return;
  }
  g_logged_fail = true;
  logging::getLogger("LocalNotifier").warning
      << "Desktop notifications unavailable: " << (detail ? detail : "unknown");
}

std::wstring Utf8ToWide(const std::string& utf8) {
  if (utf8.empty()) {
    return {};
  }
  const int needed =
      MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) {
    return {};
  }
  std::wstring out(static_cast<size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(), needed);
  return out;
}

std::string XmlEscape(const std::string& input) {
  std::string out;
  out.reserve(input.size());
  for (char c : input) {
    switch (c) {
    case '&':
      out += "&amp;";
      break;
    case '<':
      out += "&lt;";
      break;
    case '>':
      out += "&gt;";
      break;
    case '"':
      out += "&quot;";
      break;
    case '\'':
      out += "&apos;";
      break;
    default:
      out += c;
      break;
    }
  }
  return out;
}

std::string TagForThread(const std::string& thread_id) {
  if (thread_id.empty()) {
    return "frame-default";
  }
  std::string tag = "t-";
  for (unsigned char c : thread_id) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_') {
      tag += static_cast<char>(c);
    } else {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "%02x", c);
      tag += buf;
    }
    if (tag.size() >= 60) {
      break;
    }
  }
  return tag;
}

HRESULT CreateHString(const wchar_t* value, HSTRING* out) {
  return WindowsCreateString(value, value ? static_cast<UINT32>(wcslen(value)) : 0, out);
}

class ToastActivatedHandler final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                          ITypedEventHandler<ToastNotification*, IInspectable*>> {
public:
  explicit ToastActivatedHandler(std::string thread_id) : thread_id_(std::move(thread_id)) {}

  IFACEMETHODIMP Invoke(IToastNotification* /*sender*/, IInspectable* /*args*/) override {
    if (!thread_id_.empty()) {
      DispatchDesktopNotificationActivation(thread_id_);
    }
    return S_OK;
  }

private:
  std::string thread_id_;
};

bool EnsureInitLocked() {
  if (g_init_attempted) {
    return g_init_ok;
  }
  g_init_attempted = true;

  SetCurrentProcessExplicitAppUserModelID(Utf8ToWide(kProductAumid).c_str());

  HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
  if (hr == RPC_E_CHANGED_MODE) {
    hr = RoInitialize(RO_INIT_SINGLETHREADED);
  }
  if (FAILED(hr) && hr != S_FALSE) {
    LogFailOnce("Windows Runtime init failed");
    return false;
  }
  g_ro_initialized = true;

  HSTRING hs_class = nullptr;
  hr = CreateHString(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager, &hs_class);
  if (FAILED(hr)) {
    LogFailOnce("ToastNotificationManager HSTRING failed");
    return false;
  }
  hr = RoGetActivationFactory(hs_class, IID_PPV_ARGS(&g_manager));
  WindowsDeleteString(hs_class);
  if (FAILED(hr) || !g_manager) {
    LogFailOnce("ToastNotificationManager factory failed");
    return false;
  }

  HSTRING hs_aumid = nullptr;
  hr = CreateHString(Utf8ToWide(kProductAumid).c_str(), &hs_aumid);
  if (FAILED(hr)) {
    LogFailOnce("AUMID HSTRING failed");
    return false;
  }
  hr = g_manager->CreateToastNotifierWithId(hs_aumid, &g_notifier);
  WindowsDeleteString(hs_aumid);
  if (FAILED(hr) || !g_notifier) {
    LogFailOnce("CreateToastNotifierWithId failed");
    return false;
  }

  HSTRING hs_toast_class = nullptr;
  hr = CreateHString(RuntimeClass_Windows_UI_Notifications_ToastNotification, &hs_toast_class);
  if (FAILED(hr)) {
    LogFailOnce("ToastNotification HSTRING failed");
    return false;
  }
  hr = RoGetActivationFactory(hs_toast_class, IID_PPV_ARGS(&g_factory));
  WindowsDeleteString(hs_toast_class);
  if (FAILED(hr) || !g_factory) {
    LogFailOnce("ToastNotification factory failed");
    return false;
  }

  if (SUCCEEDED(g_manager.As(&g_manager2)) && g_manager2) {
    g_manager2->get_History(&g_history);
  }

  g_init_ok = true;
  return true;
}

std::wstring BuildToastXml(const std::string& title, const std::string& body,
                           const std::string& thread_id) {
  const std::string args = thread_id.empty() ? std::string("open") : ("open&thread_id=" + thread_id);
  std::string xml = "<toast launch=\"";
  xml += XmlEscape(args);
  xml += "\"><visual><binding template=\"ToastGeneric\"><text>";
  xml += XmlEscape(title);
  xml += "</text><text>";
  xml += XmlEscape(body);
  xml += "</text></binding></visual></toast>";
  return Utf8ToWide(xml);
}

} // namespace

void PostDesktopNotification(const std::string& title, const std::string& body,
                             const std::string& thread_id) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (!EnsureInitLocked()) {
    return;
  }

  ComPtr<IInspectable> inspectable;
  HSTRING hs_xml_doc = nullptr;
  if (FAILED(CreateHString(RuntimeClass_Windows_Data_Xml_Dom_XmlDocument, &hs_xml_doc))) {
    LogFailOnce("XmlDocument HSTRING failed");
    return;
  }
  HRESULT hr = RoActivateInstance(hs_xml_doc, &inspectable);
  WindowsDeleteString(hs_xml_doc);
  ComPtr<IXmlDocument> doc;
  if (FAILED(hr) || FAILED(inspectable.As(&doc)) || !doc) {
    LogFailOnce("XmlDocument activate failed");
    return;
  }

  ComPtr<IXmlDocumentIO> doc_io;
  if (FAILED(doc.As(&doc_io)) || !doc_io) {
    LogFailOnce("XmlDocumentIO unavailable");
    return;
  }

  const std::wstring xml = BuildToastXml(title, body, thread_id);
  HSTRING hs_xml = nullptr;
  if (FAILED(CreateHString(xml.c_str(), &hs_xml))) {
    return;
  }
  hr = doc_io->LoadXml(hs_xml);
  WindowsDeleteString(hs_xml);
  if (FAILED(hr)) {
    LogFailOnce("LoadXml failed");
    return;
  }

  ComPtr<IToastNotification> toast;
  hr = g_factory->CreateToastNotification(doc.Get(), &toast);
  if (FAILED(hr) || !toast) {
    LogFailOnce("CreateToastNotification failed");
    return;
  }

  const std::string tag = TagForThread(thread_id);
  ComPtr<IToastNotification2> toast2;
  if (SUCCEEDED(toast.As(&toast2)) && toast2) {
    HSTRING hs_tag = nullptr;
    if (SUCCEEDED(CreateHString(Utf8ToWide(tag).c_str(), &hs_tag))) {
      toast2->put_Tag(hs_tag);
      WindowsDeleteString(hs_tag);
    }
    HSTRING hs_group = nullptr;
    if (SUCCEEDED(CreateHString(L"frame.messages", &hs_group))) {
      toast2->put_Group(hs_group);
      WindowsDeleteString(hs_group);
    }
  }

  ComPtr<ToastActivatedHandler> handler = Microsoft::WRL::Make<ToastActivatedHandler>(thread_id);
  EventRegistrationToken activated_token{};
  toast->add_Activated(handler.Get(), &activated_token);

  hr = g_notifier->Show(toast.Get());
  if (FAILED(hr)) {
    LogFailOnce("Toast Show failed");
    return;
  }
  if (!thread_id.empty()) {
    g_thread_to_tag[thread_id] = tag;
  }
}

void ClearDesktopNotification(const std::string& thread_id) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (!g_init_ok || thread_id.empty()) {
    return;
  }
  std::string tag;
  const auto it = g_thread_to_tag.find(thread_id);
  if (it != g_thread_to_tag.end()) {
    tag = it->second;
    g_thread_to_tag.erase(it);
  } else {
    tag = TagForThread(thread_id);
  }
  if (!g_history) {
    return;
  }
  HSTRING hs_tag = nullptr;
  HSTRING hs_group = nullptr;
  HSTRING hs_aumid = nullptr;
  if (FAILED(CreateHString(Utf8ToWide(tag).c_str(), &hs_tag)) ||
      FAILED(CreateHString(L"frame.messages", &hs_group)) ||
      FAILED(CreateHString(Utf8ToWide(kProductAumid).c_str(), &hs_aumid))) {
    if (hs_tag) {
      WindowsDeleteString(hs_tag);
    }
    if (hs_group) {
      WindowsDeleteString(hs_group);
    }
    if (hs_aumid) {
      WindowsDeleteString(hs_aumid);
    }
    return;
  }
  g_history->RemoveGroupedTagWithId(hs_tag, hs_group, hs_aumid);
  WindowsDeleteString(hs_tag);
  WindowsDeleteString(hs_group);
  WindowsDeleteString(hs_aumid);
}

void ShutdownDesktopNotifications() {
  std::lock_guard<std::mutex> lock(g_mu);
  g_thread_to_tag.clear();
  g_history.Reset();
  g_manager2.Reset();
  g_factory.Reset();
  g_notifier.Reset();
  g_manager.Reset();
  if (g_ro_initialized) {
    RoUninitialize();
    g_ro_initialized = false;
  }
  g_init_attempted = false;
  g_init_ok = false;
}

} // namespace pbr::desktop

#endif
