#include "foundation/platform/PlatformShareImage.h"

#import <Photos/Photos.h>
#import <UIKit/UIKit.h>
#include <SDL3/SDL.h>

namespace pbr {

bool PlatformCanShareImage() {
  return true;
}

// Objective-C++ without ARC here: autorelease what we allocate.
bool PlatformShareImage(SDL_Window* window, const std::vector<uint8_t>& png, const ShareAnchor anchor) {
  if (window == nullptr || png.empty()) {
    return false;
  }
  @autoreleasepool {
    UIWindow* uiwindow = static_cast<UIWindow*>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr));
    UIViewController* host = uiwindow.rootViewController;
    if (host == nil) {
      return false;
    }
    while (host.presentedViewController != nil) {
      host = host.presentedViewController;
    }
    if (host.isBeingPresented || host.isBeingDismissed || host.transitionCoordinator != nil) {
      return false; // UIKit refuses a second presentation mid-transition without telling us
    }
    NSData* data = [NSData dataWithBytes:png.data() length:png.size()];
    UIImage* image = [UIImage imageWithData:data];
    if (image == nil) {
      return false;
    }
    UIActivityViewController* sheet = [[[UIActivityViewController alloc] initWithActivityItems:@[ image ]
                                                                         applicationActivities:nil] autorelease];
    if (sheet.popoverPresentationController != nil) {
      const float density = SDL_GetWindowPixelDensity(window) > 0.f ? SDL_GetWindowPixelDensity(window) : 1.f;
      CGRect rect = CGRectMake(CGRectGetMidX(host.view.bounds), CGRectGetMidY(host.view.bounds), 1, 1);
      if (anchor.w > 0.f && anchor.h > 0.f) {
        rect = CGRectMake(anchor.x / density, anchor.y / density, anchor.w / density, anchor.h / density);
      }
      sheet.popoverPresentationController.sourceView = host.view;
      sheet.popoverPresentationController.sourceRect = rect;
      sheet.popoverPresentationController.permittedArrowDirections = UIPopoverArrowDirectionAny;
    }
    [host presentViewController:sheet animated:YES completion:nil];
    return true;
  }
}

ImageSaveKind PlatformImageSaveKind() {
  return ImageSaveKind::Photos;
}

void PlatformSaveImage(SDL_Window* /*window*/, const std::string& /*suggested_name*/, std::vector<uint8_t> png,
                       std::function<void(ImageSaveResult)> done) {
  if (!done) {
    return;
  }
  if (png.empty()) {
    done(ImageSaveResult::Failed);
    return;
  }
  // Only "add to the photo library": no permission to read the user's photos is asked for.
  NSData* data = [[NSData alloc] initWithBytes:png.data() length:png.size()]; // released in the handlers below
  [PHPhotoLibrary requestAuthorizationForAccessLevel:PHAccessLevelAddOnly
                                             handler:^(PHAuthorizationStatus status) {
                                               if (status != PHAuthorizationStatusAuthorized &&
                                                   status != PHAuthorizationStatusLimited) {
                                                 [data release];
                                                 done(ImageSaveResult::Denied);
                                                 return;
                                               }
                                               [[PHPhotoLibrary sharedPhotoLibrary]
                                                   performChanges:^{
                                                     PHAssetCreationRequest* request =
                                                         [PHAssetCreationRequest creationRequestForAsset];
                                                     [request addResourceWithType:PHAssetResourceTypePhoto
                                                                             data:data
                                                                          options:nil];
                                                   }
                                                   completionHandler:^(BOOL success, NSError* /*error*/) {
                                                     [data release];
                                                     done(success ? ImageSaveResult::Saved : ImageSaveResult::Failed);
                                                   }];
                                             }];
}

} // namespace pbr
