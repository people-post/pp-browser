#pragma once

#include "lib/ui/ShellTypes.h"

namespace pbr {

/**
 * Whether the main loop ticks ContactsController. Its Tick runs the Add contact search's 300 ms debounce and the
 * contact page's edits, so it must run whenever one of those pages is up, not only on the Contacts tab: the phone's
 * bottom bar has no Contacts tab, and both pages are opened from Chats.
 */
inline bool ShouldTickContacts(const NavTab nav_tab, const bool detail_sheet_open, const bool add_contact_open) {
  return nav_tab == NavTab::Contacts || detail_sheet_open || add_contact_open;
}

} // namespace pbr
