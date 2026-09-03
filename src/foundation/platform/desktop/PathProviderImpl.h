#pragma once

#include <string>

namespace pbr::desktop {

std::string ReadEnv(const char* name);
std::string ExpandHome(std::string path);
std::string HomeDir();

std::string ConfigDirImpl();
std::string DataDirImpl(const std::string& override_path);
std::string CacheDirImpl(const std::string& data_dir);
std::string PackagedBundleAssetsDirImpl();

} // namespace pbr::desktop
