# The app's presenters (projects/capability-repos CR019): what the chats list and the call screen show and what
# their buttons do, with no GUI types — the GUI renders them (gui/), pp-app-probe presses the same buttons.
# Capabilities expose facades; another app brings its own presenters.
pp_browser_add_feature_library(pp_feature_presenters
  SOURCES
    AvatarCropFrame.cpp
    CallBackOffer.cpp
    CallConnectStory.cpp
    CallLocalHold.cpp
    CallMediaFrame.cpp
    CallMediaHintClock.cpp
    CallReconnectCapsule.cpp
    CallRingSteps.cpp
    CallStepScroll.cpp
    CallStoryList.cpp
    CallWakePlaceholder.cpp
    CallPresenter.cpp
    CallSystemUiBridge.cpp
    ChatPresenter.cpp
    CropFingerOwnership.cpp
    CropGestureAccumulator.cpp
    DeliveryNoticePlan.cpp
    GroupEventText.cpp
    GroupLinkText.cpp
    HomeBrief.cpp
    HomeToday.cpp
    NewCallGuard.cpp
    QuickReactions.cpp
    SendMark.cpp
    ThreadSearch.cpp
    ThreadSearchSession.cpp
    TwoFingerGesture.cpp
  PUBLIC_LIBS
    pp_feature_conversations
    pp_feature_calls)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
