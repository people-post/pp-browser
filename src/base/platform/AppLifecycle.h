#pragma once

#include <functional>

namespace pbr {

enum class AppLifecycleState { Foreground, Background };

class AppLifecycle {
public:
  static AppLifecycleState Current();
  static bool IsForeground();

  static void OnWillEnterBackground();
  static void OnDidEnterForeground();
  static void OnLowMemory();

  static void AddBackgroundListener(std::function<void()> listener);
  static void ClearBackgroundListeners();
};

} // namespace pbr
