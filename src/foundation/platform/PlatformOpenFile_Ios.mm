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
}
@end

namespace {

PPAttachmentPreview* g_preview = nil; // the preview that is up (or being presented); nil when none

/** Let go of the current preview. Autoreleased: this runs inside the preview's own delegate callbacks. */
void EndPreview() {
  if (g_preview == nil) {
    return;
  }
  PPAttachmentPreview* preview = g_preview;
  g_preview = nil;
  if (pbr::ios_pick::ActivePicker() == preview) {
    pbr::ios_pick::SetActivePicker(nil);
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
      [host presentViewController:controller animated:YES completion:nil];
      pbr::logging::getLogger("attachment").info << "attachment preview path=quicklook";
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
      EndPreview();
      return false;
    }
    pbr::logging::getLogger("attachment").info << "attachment preview path=document-interaction";
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
  EndPreview();
}

- (void)documentInteractionControllerDidDismissOptionsMenu:(UIDocumentInteractionController*)controller {
  EndPreview();
}

@end

namespace pbr {

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
