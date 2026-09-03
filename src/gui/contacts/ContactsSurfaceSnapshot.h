#pragma once

namespace pbr {

/**
 * Surface facts contacts UI pushes upward — no shell Dirty or chrome enums.
 * App-owned bridge maps this to ContactsShellProjection then ShellHost apply.
 */
struct ContactsSurfaceSnapshot {
  /** A contact is selected (detail chrome should be open). */
  bool detail_open = false;
  /** Sum of list-row unread counts. */
  int contacts_unread = 0;
};

} // namespace pbr
