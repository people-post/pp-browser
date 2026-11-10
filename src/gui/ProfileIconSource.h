#pragma once

#include <vector>

namespace pbr {

/** What Me → Profile's photo menu can do. */
enum class ProfileIconSourceAction { Camera, Library, Remove };

struct ProfileIconSourcePlan {
  /** In the order the menu shows them (the Cancel row is the menu's own, not listed). */
  std::vector<ProfileIconSourceAction> actions;
  /** One action only: run it at once, a menu of one row would only be a tap in the way. */
  bool run_directly = false;
};

/** Which sources the photo menu offers: the camera only on a phone, "remove" only while there is a photo. The library
 *  is always there, so the desktop without a photo has just that one and goes straight to the file dialog. */
inline ProfileIconSourcePlan PlanProfileIconSources(const bool has_camera, const bool has_icon) {
  ProfileIconSourcePlan plan;
  if (has_camera) {
    plan.actions.push_back(ProfileIconSourceAction::Camera);
  }
  plan.actions.push_back(ProfileIconSourceAction::Library);
  if (has_icon) {
    plan.actions.push_back(ProfileIconSourceAction::Remove);
  }
  plan.run_directly = plan.actions.size() == 1;
  return plan;
}

} // namespace pbr
