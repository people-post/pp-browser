// iOS: open a local file in the app. UIApplication openURL: refuses file:// addresses, so the portable
// PlatformOpenFile.cpp cannot work here. QLPreviewController previews text, PDF, pictures, audio and video in
// place (its share button stays: exporting is the person's own choice); a type it cannot show falls back to the
// "Open in…" sheet of UIDocumentInteractionController.
// Objective-C++ without ARC here (as in NativeFileDialog_Ios.mm): retain / release by hand.
#include "foundation/platform/PlatformOpenFile.h"

#include "foundation/platform/NativeFileDialogIosShared.h"
#include "foundation/platform/ui/RmlUi_Backend.h"
#include "common/Logger.h"
#include "common/PbrCompat.h"

#import <QuickLook/QuickLook.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <functional>
#include <string>

/**
 * Keeps what the system only holds weakly (a preview's data source and delegate, the interaction controller) alive
 * until the person closes it. Claims the one-picker guard of NativeFileDialogIosShared.h meanwhile.
 */
@interface PPAttachmentPreview
    : NSObject <QLPreviewControllerDataSource, QLPreviewControllerDelegate, UIDocumentInteractionControllerDelegate> {
@public
  NSURL* url;
  UIDocumentInteractionController* interaction;
  bool sound; // the sound hooks' `begin` ran for it: `end` runs when it goes
}
@end

namespace {

PPAttachmentPreview* g_preview = nil; // the preview that is up (or being presented); nil when none
std::function<void()> g_sound_begin;   // SetPlatformOpenFileSoundHooks
std::function<void()> g_sound_end;

/**
 * What the next device log needs to tell why QuickLook shows its info page for a file: the extension and type it
 * sees, whether it says it can preview it, the name's length (not the name), whether the file is a hard link, and its
 * data-protection class. No folder, no name, no content.
 */
std::string DescribeForLog(NSURL* url) {
  UTType* type = [UTType typeWithFilenameExtension:url.pathExtension];
  NSDictionary* attributes = [[NSFileManager defaultManager] attributesOfItemAtPath:url.path error:nil];
  std::string line = std::string("ext=") + (url.pathExtension.length > 0 ? url.pathExtension.UTF8String : "-") +
                     " uti=" + (type != nil ? type.identifier.UTF8String : "-") +
                     " can_preview=" + ([QLPreviewController canPreviewItem:url] ? "1" : "0") +
                     " name_len=" + std::to_string(url.lastPathComponent.length) +
                     " links=" + std::to_string([attributes[NSFileReferenceCount] unsignedLongValue]) +
                     " protection=" + (attributes[NSFileProtectionKey] != nil ? [attributes[NSFileProtectionKey] UTF8String] : "-");
  return line;
}

/** Sound or video: QuickLook plays it through the app's own audio session. */
bool PlaysSound(NSURL* url) {
  UTType* type = [UTType typeWithFilenameExtension:url.pathExtension];
  return type != nil && [type conformsToType:UTTypeAudiovisualContent];
}

/** Let go of the current preview. Autoreleased: this runs inside the preview's own delegate callbacks. */
void EndPreview(PPAttachmentPreview* ended) {
  if (g_preview == nil || g_preview != ended) {
    return; // not the current preview (already ended, or a later one is up)
  }
  PPAttachmentPreview* preview = g_preview;
  g_preview = nil;
  if (pbr::ios_pick::ActivePicker() == preview) {
    pbr::ios_pick::SetActivePicker(nil);
  }
  if (preview->sound && g_sound_end) {
    g_sound_end();
  }
  [preview autorelease];
}

bool OpenOnMain(const std::string& path) {
  @autoreleasepool {
    if (g_preview != nil) {
      return true; // one is already up (or on its way): a second tap is not an error
    }
    UIViewController* host =
        pbr::ios_pick::ActivePicker() == nil ? pbr::ios_pick::PresentingController(Backend::GetWindow()) : nil;
    if (host == nil || host.presentedViewController != nil) {
      return false; // nothing to present from, or something is already on screen
    }
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    if (url == nil) {
      return false;
    }
    PPAttachmentPreview* preview = [[PPAttachmentPreview alloc] init]; // released in EndPreview
    preview->url = [url retain];

    if ([QLPreviewController canPreviewItem:url]) {
      QLPreviewController* controller = [[[QLPreviewController alloc] init] autorelease];
      controller.dataSource = preview;
      controller.delegate = preview;
      g_preview = preview;
      pbr::ios_pick::SetActivePicker(preview);
      if (PlaysSound(url) && g_sound_begin) {
        g_sound_begin(); // loud, with the silent switch on: not on the earpiece a call left the session on
        preview->sound = true;
      }
      [host presentViewController:controller animated:YES completion:nil];
      // UIKit may refuse silently: if the preview never appears, do not keep the guard claimed.
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (g_preview == preview && controller.presentingViewController == nil) {
          pbr::logging::getLogger("attachment").warning << "attachment preview present refused";
          EndPreview(preview);
        }
      });
      pbr::logging::getLogger("attachment").info << "attachment preview path=quicklook " << DescribeForLog(url)
                                                  << " sound_session=" << (preview->sound ? 1 : 0);
      return true;
    }

    UIDocumentInteractionController* interaction =
        [[UIDocumentInteractionController interactionControllerWithURL:url] retain];
    preview->interaction = interaction;
    interaction.delegate = preview;
    g_preview = preview;
    pbr::ios_pick::SetActivePicker(preview);
    const CGRect anchor = CGRectMake(CGRectGetMidX(host.view.bounds), CGRectGetMidY(host.view.bounds), 1, 1);
    if (![interaction presentOptionsMenuFromRect:anchor inView:host.view animated:YES]) {
      EndPreview(preview);
      return false;
    }
    pbr::logging::getLogger("attachment").info << "attachment preview path=document-interaction "
                                                << DescribeForLog(url);
    return true;
  }
}

} // namespace

@implementation PPAttachmentPreview

- (void)dealloc {
  [url release];
  [interaction release];
  [super dealloc];
}

- (NSInteger)numberOfPreviewItemsInPreviewController:(QLPreviewController*)controller {
  return 1;
}

- (id<QLPreviewItem>)previewController:(QLPreviewController*)controller previewItemAtIndex:(NSInteger)index {
  return url;
}

- (void)previewControllerDidDismiss:(QLPreviewController*)controller {
  EndPreview(self);
}

- (void)documentInteractionControllerDidDismissOptionsMenu:(UIDocumentInteractionController*)controller {
  EndPreview(self);
}

- (void)documentInteractionControllerDidDismissOpenInMenu:(UIDocumentInteractionController*)controller {
  EndPreview(self);
}

// A hand-off to another app: the menu may not report a dismiss, so the guard is let go here too.
- (void)documentInteractionController:(UIDocumentInteractionController*)controller
        willBeginSendingToApplication:(NSString*)application {
  EndPreview(self);
}

- (void)documentInteractionController:(UIDocumentInteractionController*)controller
           didEndSendingToApplication:(NSString*)application {
  EndPreview(self);
}

@end

namespace pbr {

void SetPlatformOpenFileSoundHooks(std::function<void()> begin, std::function<void()> end) {
  dispatch_async(dispatch_get_main_queue(), ^{ // read on the main thread only (the preview's)
    g_sound_begin = begin;
    g_sound_end = end;
  });
}

void PlatformForgetOpenedFiles() {} // QuickLook is handed the named link inside the view cache: wiped with it

bool PlatformOpenFile(const std::string& path) {
  if (path.empty() || ![[NSFileManager defaultManager] fileExistsAtPath:[NSString stringWithUTF8String:path.c_str()]]) {
    return false;
  }
  if ([NSThread isMainThread]) {
    return OpenOnMain(path);
  }
  __block bool opened = false;
  dispatch_sync(dispatch_get_main_queue(), ^{
    opened = OpenOnMain(path);
  });
  return opened;
}

} // namespace pbr
