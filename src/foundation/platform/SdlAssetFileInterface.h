#pragma once

#include <ui/base/FileInterface.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace pbr {

#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)

class SdlAssetFileInterface : public ui::FileInterface {
public:
  ui::FileHandle Open(const ui::String& path) override;
  void Close(ui::FileHandle file) override;
  size_t Read(void* buffer, size_t size, ui::FileHandle file) override;
  bool Seek(ui::FileHandle file, long offset, int origin) override;
  size_t Tell(ui::FileHandle file) override;
};

#endif

} // namespace pbr
