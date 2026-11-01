// iOS system camera for the chat: UIImagePickerController (sourceType Camera) offers photo and video in one screen.
// Photos are re-encoded as JPEG without EXIF (so no location); videos are copied by AVAssetExportSession in
// passthrough mode with the sharing metadata filter (no re-encode, location / device tags dropped, orientation kept).
// The result is written to tmp/pp-picks/<uuid>/ like a picked file, so ReleasePickedFile deletes it. The camera
// shares the "one picker at a time" guard with the photo / file pickers (NativeFileDialogIosShared.h).
// Objective-C++ without ARC here (as in NativeFileDialog_Ios.mm): retain / release by hand.
#include "foundation/platform/CameraCapture.h"
#include "foundation/platform/NativeFileDialogIosShared.h"

#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <cstdlib>
#include <functional>
#include <string>
#include <utility>
#include <vector>

@interface PPCameraDelegate : NSObject <UIImagePickerControllerDelegate, UINavigationControllerDelegate> {
@public
  pbr::CameraCaptureCallback callback;
}
@end

namespace {

using pbr::CameraCaptureOutcome;
using namespace pbr::ios_pick;

/** PP_BROWSER_FAIL_VIDEO_CLEAN=1 (read once): the video clean-up is treated as failed, for device testing. */
bool ForcedVideoCleanFailure() {
  static const bool forced = [] {
    const char* value = std::getenv("PP_BROWSER_FAIL_VIDEO_CLEAN");
    return value != nullptr && value[0] == '1';
  }();
  return forced;
}

/** End the capture on the main thread: free the guard first, so the callback may start another pick. */
void Finish(PPCameraDelegate* delegate, CameraCaptureOutcome outcome, std::vector<std::string> paths) {
  pbr::CameraCaptureCallback callback = std::move(delegate->callback);
  delegate->callback = nullptr;
  if (ActivePicker() == delegate) {
    SetActivePicker(nil);
    [delegate release];
  }
  if (callback) {
    callback(outcome, std::move(paths));
  }
}

void FinishOnMain(PPCameraDelegate* delegate, CameraCaptureOutcome outcome, std::vector<std::string> paths) {
  dispatch_async(dispatch_get_main_queue(), ^{
    Finish(delegate, outcome, paths);
  });
}

/** tmp/pp-picks/<uuid>/ for one result; nil on failure. */
NSString* NewResultDir() {
  NSString* dir = [PickRoot() stringByAppendingPathComponent:[[NSUUID UUID] UUIDString]];
  if (![[NSFileManager defaultManager] createDirectoryAtPath:dir
                                 withIntermediateDirectories:YES
                                                  attributes:nil
                                                       error:nil]) {
    return nil;
  }
  return dir;
}

void RemoveFile(NSString* path) {
  if (path != nil) {
    [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
  }
}

void FinishWithFile(PPCameraDelegate* delegate, NSString* file) {
  FinishOnMain(delegate, CameraCaptureOutcome::Picked, {std::string(file.fileSystemRepresentation)});
}

/** The export APIs used below are deprecated from iOS 18 in favour of async variants; this target is iOS 15. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

/**
 * Copy `asset` to `dir`/VID.<ext> without re-encoding and without metadata. MPEG-4 first; when the session does not
 * offer it or the export fails (HEVC .mov in an mp4 can be refused), once more as a QuickTime .mov, still passthrough.
 * `done(path)` gets the output path, or nil with nothing left on disk.
 */
void ExportCleanVideo(AVAsset* asset, NSString* dir, bool mpeg4, void (^done)(NSString* path)) {
  AVAssetExportSession* session = [[[AVAssetExportSession alloc] initWithAsset:asset
                                                                    presetName:AVAssetExportPresetPassthrough]
      autorelease];
  NSString* file_type = mpeg4 ? AVFileTypeMPEG4 : AVFileTypeQuickTimeMovie;
  if (session == nil || ![session.supportedFileTypes containsObject:file_type]) {
    if (mpeg4) {
      ExportCleanVideo(asset, dir, false, done);
    } else {
      done(nil);
    }
    return;
  }
  NSString* out = [dir stringByAppendingPathComponent:mpeg4 ? @"VID.mp4" : @"VID.mov"];
  session.outputURL = [NSURL fileURLWithPath:out];
  session.outputFileType = file_type;
  session.shouldOptimizeForNetworkUse = NO;
  session.metadataItemFilter = [AVMetadataItemFilter metadataItemFilterForSharing];
  [session exportAsynchronouslyWithCompletionHandler:^{
    if (session.status == AVAssetExportSessionStatusCompleted) {
      done(out);
      return;
    }
    RemoveFile(out); // a partial output
    if (mpeg4) {
      ExportCleanVideo(asset, dir, false, done);
    } else {
      done(nil);
    }
  }];
}

#pragma clang diagnostic pop

} // namespace

@implementation PPCameraDelegate

- (void)imagePickerController:(UIImagePickerController*)picker
    didFinishPickingMediaWithInfo:(NSDictionary<UIImagePickerControllerInfoKey, id>*)info {
  [picker dismissViewControllerAnimated:YES completion:nil];
  NSString* media_type = info[UIImagePickerControllerMediaType];
  [self retain]; // until the file has been written
  if ([media_type isEqualToString:UTTypeMovie.identifier]) {
    [self finishVideo:info[UIImagePickerControllerMediaURL]];
  } else {
    [self finishPhoto:info[UIImagePickerControllerOriginalImage]];
  }
}

- (void)imagePickerControllerDidCancel:(UIImagePickerController*)picker {
  [picker dismissViewControllerAnimated:YES completion:nil];
  Finish(self, CameraCaptureOutcome::Cancelled, {});
}

// Called with an extra retain on self, which it balances.
- (void)finishPhoto:(UIImage*)image {
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    @autoreleasepool {
      // UIImageJPEGRepresentation writes the pixels and the orientation, no GPS / EXIF block of the camera.
      NSData* jpeg = image != nil ? UIImageJPEGRepresentation(image, 0.9) : nil;
      NSString* dir = jpeg != nil ? NewResultDir() : nil;
      NSString* file = dir != nil ? [dir stringByAppendingPathComponent:@"IMG.jpg"] : nil;
      if (file != nil && [jpeg writeToFile:file atomically:YES]) {
        FinishWithFile(self, file);
      } else {
        RemoveFile(dir);
        FinishOnMain(self, CameraCaptureOutcome::Unavailable, {}); // the camera gave no usable picture
      }
    }
    [self release];
  });
}

// Called with an extra retain on self, which it balances. The camera's own file is deleted on every path.
- (void)finishVideo:(NSURL*)source {
  if (source == nil) {
    FinishOnMain(self, CameraCaptureOutcome::Unavailable, {}); // the camera gave no file
    [self release];
    return;
  }
  [source retain];
  NSString* dir = ForcedVideoCleanFailure() ? nil : NewResultDir();
  if (dir == nil) {
    // Forced failure, or no room to write: nothing is sent, never the original.
    RemoveFile(source.path);
    [source release];
    FinishOnMain(self, CameraCaptureOutcome::Unprocessable, {});
    [self release];
    return;
  }
  [dir retain];
  ExportCleanVideo([AVURLAsset URLAssetWithURL:source options:nil], dir, true, ^(NSString* path) {
    RemoveFile(source.path); // success or failure: the camera's original never stays behind
    if (path != nil) {
      FinishWithFile(self, path);
    } else {
      RemoveFile(dir);
      FinishOnMain(self, CameraCaptureOutcome::Unprocessable, {});
    }
    [source release];
    [dir release];
    [self release];
  });
}

@end

namespace {

/** Present from the main thread once UIKit allows it (not mid-transition); gives up after about 2 seconds. */
void PresentWhenReady(SDL_Window* window, PPCameraDelegate* delegate, UIImagePickerController* picker, int attempts) {
  UIViewController* host = PresentingController(window);
  if (host != nil) {
    [host presentViewController:picker animated:YES completion:nil];
    return;
  }
  if (attempts <= 0) {
    Finish(delegate, CameraCaptureOutcome::Unavailable, {});
    return;
  }
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
    PresentWhenReady(window, delegate, picker, attempts - 1);
  });
}

void PresentCamera(SDL_Window* window, PPCameraDelegate* delegate, pbr::CameraCaptureKind kind,
                   int max_video_seconds) {
  const bool microphone =
      [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio] == AVAuthorizationStatusAuthorized;
  // The microphone is only checked, never asked for here (a call or a voice message asks). Without it a video would
  // come out silent, so only pictures are offered: Photo / PhotoOrVideo become picture-only and the caller still gets
  // Picked for the picture; a Video-only request has nothing to offer and answers Unavailable.
  NSArray<NSString*>* media_types = nil;
  switch (kind) {
    case pbr::CameraCaptureKind::Photo:
      media_types = @[ UTTypeImage.identifier ];
      break;
    case pbr::CameraCaptureKind::Video:
      media_types = microphone ? @[ UTTypeMovie.identifier ] : nil;
      break;
    case pbr::CameraCaptureKind::PhotoOrVideo:
      media_types = microphone ? @[ UTTypeImage.identifier, UTTypeMovie.identifier ] : @[ UTTypeImage.identifier ];
      break;
  }
  if (media_types == nil) {
    Finish(delegate, CameraCaptureOutcome::Unavailable, {});
    return;
  }
  UIImagePickerController* picker = [[[UIImagePickerController alloc] init] autorelease];
  picker.sourceType = UIImagePickerControllerSourceTypeCamera;
  picker.mediaTypes = media_types;
  picker.cameraDevice = UIImagePickerControllerCameraDeviceRear;
  if (kind == pbr::CameraCaptureKind::Video) {
    picker.cameraCaptureMode = UIImagePickerControllerCameraCaptureModeVideo;
  }
  picker.videoMaximumDuration = max_video_seconds > 0 ? max_video_seconds : 10;
  picker.videoQuality = UIImagePickerControllerQualityTypeMedium;
  picker.modalPresentationStyle = UIModalPresentationFullScreen;
  picker.delegate = delegate; // weak: the guard keeps the delegate alive
  PresentWhenReady(window, delegate, picker, 40);
}

} // namespace

namespace pbr {

void ShowCameraCapture(SDL_Window* window, CameraCaptureKind kind, int max_video_seconds,
                       uint64_t /*max_video_bytes*/, CameraCaptureCallback callback) {
  if (!callback) {
    return;
  }
  // max_video_bytes: UIImagePickerController has no size limit; Medium quality plus the duration limit keep a clip
  // small, and the caller checks the size again.
  if (![NSThread isMainThread]) {
    dispatch_async(dispatch_get_main_queue(), ^{
      ShowCameraCapture(window, kind, max_video_seconds, 0, callback);
    });
    return;
  }
  @autoreleasepool {
    if (ActivePicker() != nil) { // another picker is up
      callback(CameraCaptureOutcome::Cancelled, {});
      return;
    }
    // Simulator, or a device without a camera.
    if (![UIImagePickerController isSourceTypeAvailable:UIImagePickerControllerSourceTypeCamera]) {
      callback(CameraCaptureOutcome::Unavailable, {});
      return;
    }
    AVAuthorizationStatus status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
    if (status == AVAuthorizationStatusDenied || status == AVAuthorizationStatusRestricted) {
      callback(CameraCaptureOutcome::Unavailable, {});
      return;
    }
    ClearLeftoversOnce();
    PPCameraDelegate* delegate = [[PPCameraDelegate alloc] init]; // released in Finish
    delegate->callback = std::move(callback);
    SetActivePicker(delegate); // held for the permission prompt too
    if (status == AVAuthorizationStatusAuthorized) {
      PresentCamera(window, delegate, kind, max_video_seconds);
      return;
    }
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                             completionHandler:^(BOOL granted) {
                               dispatch_async(dispatch_get_main_queue(), ^{
                                 if (granted) {
                                   PresentCamera(window, delegate, kind, max_video_seconds);
                                 } else {
                                   Finish(delegate, CameraCaptureOutcome::Unavailable, {});
                                 }
                               });
                             }];
  }
}

std::string CaptureScratchDir() {
  @autoreleasepool {
    return std::string(PickRoot().fileSystemRepresentation);
  }
}

} // namespace pbr
