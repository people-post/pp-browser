#pragma once
// Internal to the iOS pickers (NativeFileDialog_Ios.mm, CameraCapture_Ios.mm): the "one picker at a time" guard,
// the tmp/pp-picks root all picked / captured files live under, and the controller to present from.
// Definitions are in NativeFileDialog_Ios.mm. Objective-C++ only.

#import <UIKit/UIKit.h>

struct SDL_Window;

namespace pbr::ios_pick {

/** tmp/pp-picks: every picked or captured file is written to <PickRoot>/<uuid>/<name>. */
NSString* PickRoot();

void ClearPickRoot();

/** Clears PickRoot the first time it is called in this run (leftovers of a crash / kill), never again. */
void ClearLeftoversOnce();

/** The controller to present from, or nil when UIKit would refuse (none, or mid-transition). */
UIViewController* PresentingController(SDL_Window* window);

/** The delegate of the picker that is up (photo, file or camera), nil when none. Not retained here. */
id ActivePicker();

/** Claim (owner) or release (nil) the guard; the caller keeps the owner alive until it releases the guard. */
void SetActivePicker(id owner);

} // namespace pbr::ios_pick
