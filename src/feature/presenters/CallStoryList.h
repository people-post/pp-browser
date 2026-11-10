#pragma once

#include "feature/presenters/CallConnectStory.h"
#include "feature/presenters/CallRingSteps.h"

#include <string>
#include <vector>

namespace pbr {

/**
 * What the answering page's list is made of beyond CallStoryLines' own steps: whether a 1:1 answerer's list opens with
 * the three done lines the ring page showed, which row is the current one, which way the spark runs, and, when the
 * rows reached do not fit, how far the list moves up inside a clip of what height. Decided here; the controller
 * forwards the inputs and renders the result.
 */
struct CallStoryListInput {
  CallStoryView view;
  /** The page is on screen only while connecting; after that nothing here is wanted (and nothing may change with the
   *  page height of a connected call). */
  bool story_active = false;
  bool group = false;
  /** The video card (the camera behind the steps): it carries no lead rows. */
  bool video_stage = false;
  /** The other party as the page names them ("Stranger" when unknown), and the kind the call rang with. */
  std::string caller;
  CallRingMedia media = CallRingMedia::Voice;
  /** Window height less the status bar and the bottom inset; 0: not known. */
  int page_height_dp = 0;
  /** A hint line (status / quality) and the diagnostics line sit under the list and take room from it. */
  bool hint_shown = false;
  bool debug_shown = false;
};

struct CallStoryList {
  /** Done rows above the steps (call keys / the caller's name); empty: none. */
  std::vector<CallRingStep> lead;
  /** 1-based row now current in the whole list (the row count once every step is done); 0 when there is no list. */
  int reached_row = 0;
  /** This device is the one being called: the spark runs from them towards us. */
  bool inbound = false;
  /** Rows the list moves up, and the clip's height; both 0 when everything reached fits (the list is then free). */
  int scroll_rows = 0;
  int clip_dp = 0;
};

CallStoryList PlanCallStoryList(const CallStoryListInput& in);

} // namespace pbr
