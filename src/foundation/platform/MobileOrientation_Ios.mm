#include "foundation/platform/MobileOrientation.h"

#import <UIKit/UIKit.h>

namespace pbr {

const char* MobileOrientationsHint() {
  return UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad ? TabletOrientationsHint()
                                                                              : PhoneOrientationsHint();
}

} // namespace pbr
