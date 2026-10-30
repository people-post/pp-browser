// iOS file pickers: SDL3 has no iOS dialog backend. Photos via PHPickerViewController (no photo-library
// permission needed), files via UIDocumentPickerViewController. Picked items are copied into
// tmp/pp-picks/<uuid>/<name>; the consumer releases its copy when done (ReleasePickedFile), and leftovers
// of an earlier run (a crash, a kill) are cleared when the first pick of this run starts.
#include "foundation/platform/NativeFileDialog.h"

#import <PhotosUI/PhotosUI.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <SDL3/SDL.h>

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Objective-C++ without ARC here (as in PlatformShare_Ios.mm): retain / release by hand.
@interface PPPickDelegate : NSObject <PHPickerViewControllerDelegate, UIDocumentPickerDelegate> {
@public
  pbr::NativeFileDialogCallback callback;
}
@end

/** The export picker's delegate: its own, since the pick delegate deletes what it was handed (here: the saved file). */
@interface PPExportDelegate : NSObject <UIDocumentPickerDelegate> {
@public
  std::function<void(bool)> done;
  NSURL* temp; // retained; removed when the export ends
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
  static bool cleared_leftovers = false; // only earlier runs: this run's copies may still be in use
  if (!cleared_leftovers) {
    ClearPickRoot();
    cleared_leftovers = true;
  }
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

namespace {

PPExportDelegate* g_export = nil; // like g_active: the picker holds its delegate weakly

void FinishExport(PPExportDelegate* delegate, const bool saved) {
  std::function<void(bool)> done = std::move(delegate->done);
  delegate->done = nullptr;
  if (delegate->temp != nil) {
    [[NSFileManager defaultManager] removeItemAtURL:delegate->temp error:nil];
    [delegate->temp release];
    delegate->temp = nil;
  }
  if (g_export == delegate) {
    [g_export release];
    g_export = nil;
  }
  if (done) {
    done(saved);
  }
}

} // namespace

@implementation PPExportDelegate

- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
  FinishExport(self, urls.count > 0);
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
  FinishExport(self, false);
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

void ReleasePickedFile(const std::string& path) {
  @autoreleasepool {
    NSString* file = [NSString stringWithUTF8String:path.c_str()];
    NSString* root = [PickRoot() stringByAppendingString:@"/"];
    // Resolve /private/var vs /var: compare standardized paths.
    if (file == nil || ![file.stringByStandardizingPath hasPrefix:root.stringByStandardizingPath]) {
      return;
    }
    [[NSFileManager defaultManager] removeItemAtPath:[file stringByDeletingLastPathComponent] error:nil];
  }
}

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

void SaveFileAs(SDL_Window* window, const std::string& suggested_name, std::string bytes,
                std::function<void(bool saved)> done) {
  if (!done) {
    return;
  }
  @autoreleasepool {
    UIViewController* host = (g_export == nil && g_active == nil) ? PresentingController(window) : nil;
    if (host == nil) {
      done(false);
      return;
    }
    // The picker exports a file that exists: write it under tmp first (removed when the export ends).
    NSString* dir = [NSTemporaryDirectory() stringByAppendingPathComponent:@"pp-exports"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    NSString* name = [NSString stringWithUTF8String:suggested_name.c_str()];
    NSURL* temp = [NSURL fileURLWithPath:[dir stringByAppendingPathComponent:name.lastPathComponent]];
    NSData* data = [NSData dataWithBytes:bytes.data() length:bytes.size()];
    if (name.length == 0 || ![data writeToURL:temp options:NSDataWritingAtomic error:nil]) {
      done(false);
      return;
    }
    PPExportDelegate* delegate = [[PPExportDelegate alloc] init]; // released in FinishExport
    delegate->done = std::move(done);
    delegate->temp = [temp retain];
    g_export = delegate;
    UIDocumentPickerViewController* picker =
        [[[UIDocumentPickerViewController alloc] initForExportingURLs:@[ temp ] asCopy:YES] autorelease];
    picker.delegate = delegate;
    [host presentViewController:picker animated:YES completion:nil];
  }
}

std::optional<std::string> ReadPickedFile(const std::string& path, const size_t max_bytes) {
  @autoreleasepool {
    NSData* data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path.c_str()]];
    if (data == nil || data.length > max_bytes) {
      return std::nullopt;
    }
    return std::string(static_cast<const char*>(data.bytes), data.length);
  }
}

} // namespace pbr
