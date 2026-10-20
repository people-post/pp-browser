// iOS file pickers: SDL3 has no iOS dialog backend. Photos via PHPickerViewController (no photo-library
// permission needed), files via UIDocumentPickerViewController. Picked items are copied into
// tmp/pp-picks/<uuid>/<name>; the folder is cleared when the next pick starts.
#include "foundation/platform/NativeFileDialog.h"

#import <PhotosUI/PhotosUI.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <SDL3/SDL.h>

#include <string>
#include <utility>
#include <vector>

// Objective-C++ without ARC here (as in PlatformShare_Ios.mm): retain / release by hand.
@interface PPPickDelegate : NSObject <PHPickerViewControllerDelegate, UIDocumentPickerDelegate> {
@public
  pbr::NativeFileDialogCallback callback;
}
@end

namespace {

PPPickDelegate* g_active = nil; // pickers hold their delegate weakly: kept alive here until the pick ends

NSString* PickRoot() { return [NSTemporaryDirectory() stringByAppendingPathComponent:@"pp-picks"]; }

void ClearPickRoot() { [[NSFileManager defaultManager] removeItemAtPath:PickRoot() error:nil]; }

/** Copy `source` to tmp/pp-picks/<uuid>/<name>; empty on failure. */
std::string CopyIntoPickRoot(NSURL* source, NSString* name) {
  NSFileManager* files = [NSFileManager defaultManager];
  NSString* dir = [PickRoot() stringByAppendingPathComponent:[[NSUUID UUID] UUIDString]];
  if (![files createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil]) {
    return {};
  }
  NSString* file_name = name.length > 0 ? name.lastPathComponent : source.lastPathComponent;
  if (file_name.pathExtension.length == 0 && source.pathExtension.length > 0) {
    file_name = [file_name stringByAppendingPathExtension:source.pathExtension];
  }
  NSString* dest = [dir stringByAppendingPathComponent:file_name];
  if (![files copyItemAtPath:source.path toPath:dest error:nil]) {
    return {};
  }
  return std::string(dest.fileSystemRepresentation);
}

void Finish(PPPickDelegate* delegate, std::vector<std::string> paths) {
  pbr::NativeFileDialogCallback callback = std::move(delegate->callback);
  delegate->callback = nullptr;
  if (g_active == delegate) {
    [g_active release];
    g_active = nil;
  }
  if (callback) {
    callback(std::move(paths));
  }
}

/** The controller to present from, or nil when UIKit would refuse (none, or mid-transition). */
UIViewController* PresentingController(SDL_Window* window) {
  if (window == nullptr) {
    return nil;
  }
  UIWindow* uiwindow = static_cast<UIWindow*>(
      SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr));
  UIViewController* host = uiwindow.rootViewController;
  while (host != nil && host.presentedViewController != nil) {
    host = host.presentedViewController;
  }
  if (host == nil || host.isBeingPresented || host.isBeingDismissed || host.transitionCoordinator != nil) {
    return nil;
  }
  return host;
}

/** Start a pick: nil (and the callback answered "nothing picked") when another pick is up or UIKit is busy. */
PPPickDelegate* BeginPick(SDL_Window* window, pbr::NativeFileDialogCallback callback, UIViewController** host) {
  *host = g_active == nil ? PresentingController(window) : nil;
  if (*host == nil) {
    callback({});
    return nil;
  }
  ClearPickRoot(); // the previous pick's copy has been read by now
  PPPickDelegate* delegate = [[PPPickDelegate alloc] init]; // released in Finish
  delegate->callback = std::move(callback);
  g_active = delegate;
  return delegate;
}

} // namespace

@implementation PPPickDelegate

- (void)picker:(PHPickerViewController*)picker didFinishPicking:(NSArray<PHPickerResult*>*)results {
  [picker dismissViewControllerAnimated:YES completion:nil];
  if (results.count == 0) {
    Finish(self, {});
    return;
  }
  NSItemProvider* provider = results.firstObject.itemProvider;
  // Ask for the JPEG / PNG the "compatible" mode offers: SDL_image cannot decode the HEIC originals here
  // (2026-10-20 device: photos failed as "not an image"). Any image only as a last resort.
  NSString* type = UTTypeImage.identifier;
  if ([provider hasItemConformingToTypeIdentifier:UTTypeJPEG.identifier]) {
    type = UTTypeJPEG.identifier;
  } else if ([provider hasItemConformingToTypeIdentifier:UTTypePNG.identifier]) {
    type = UTTypePNG.identifier;
  }
  NSString* name = provider.suggestedName;
  [self retain]; // until the file has been copied
  [provider loadFileRepresentationForTypeIdentifier:type
                                  completionHandler:^(NSURL* url, NSError* /*error*/) {
                                    // `url` is removed when this handler returns: copy it now.
                                    std::vector<std::string> paths;
                                    if (url != nil) {
                                      std::string copied = CopyIntoPickRoot(url, name);
                                      if (!copied.empty()) {
                                        paths.push_back(std::move(copied));
                                      }
                                    }
                                    dispatch_async(dispatch_get_main_queue(), ^{
                                      Finish(self, paths);
                                      [self release];
                                    });
                                  }];
}

- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
  std::vector<std::string> paths;
  if (NSURL* url = urls.firstObject) {
    std::string copied = CopyIntoPickRoot(url, url.lastPathComponent);
    [[NSFileManager defaultManager] removeItemAtURL:url error:nil]; // the picker's own copy (asCopy)
    if (!copied.empty()) {
      paths.push_back(std::move(copied));
    }
  }
  Finish(self, std::move(paths));
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
  Finish(self, {});
}

@end

namespace pbr {

void ShowOpenImageFileDialog(SDL_Window* window, NativeFileDialogCallback callback, const bool include_heic) {
  if (!callback) {
    return;
  }
  @autoreleasepool {
    UIViewController* host = nil;
    PPPickDelegate* delegate = BeginPick(window, std::move(callback), &host);
    if (delegate == nil) {
      return;
    }
    (void)include_heic; // the compatible representation is always JPEG / PNG
    PHPickerConfiguration* config = [[[PHPickerConfiguration alloc] init] autorelease];
    config.filter = [PHPickerFilter imagesFilter];
    config.selectionLimit = 1;
    config.preferredAssetRepresentationMode = PHPickerConfigurationAssetRepresentationModeCompatible;
    PHPickerViewController* picker = [[[PHPickerViewController alloc] initWithConfiguration:config] autorelease];
    picker.delegate = delegate;
    [host presentViewController:picker animated:YES completion:nil];
  }
}

namespace {

void PresentDocumentPicker(SDL_Window* window, NativeFileDialogCallback callback, NSArray<UTType*>* types) {
  @autoreleasepool {
    UIViewController* host = nil;
    PPPickDelegate* delegate = BeginPick(window, std::move(callback), &host);
    if (delegate == nil) {
      return;
    }
    UIDocumentPickerViewController* picker =
        [[[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types asCopy:YES] autorelease];
    picker.delegate = delegate;
    picker.allowsMultipleSelection = NO;
    [host presentViewController:picker animated:YES completion:nil];
  }
}

} // namespace

void ShowOpenFileDialog(SDL_Window* window, NativeFileDialogCallback callback) {
  if (!callback) {
    return;
  }
  PresentDocumentPicker(window, std::move(callback), @[ UTTypeItem ]);
}

void ShowOpenImageDocumentDialog(SDL_Window* window, NativeFileDialogCallback callback, const bool /*include_heic*/) {
  if (!callback) {
    return;
  }
  // Only what SDL_image decodes here: HEIC files are left out (the photo library converts; a file cannot).
  PresentDocumentPicker(window, std::move(callback), @[ UTTypeJPEG, UTTypePNG, UTTypeGIF, UTTypeWebP ]);
}

} // namespace pbr
