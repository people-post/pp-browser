// "Linked devices" vocabulary (Kenneth 2026-11-14): one account on several devices is called "linked devices" / 关联设备,
// and the code that links a new one is the "link code" / 关联码. The word 转移 / "transfer" is retired from the device
// context; this test lists the strings that may still carry it for another meaning.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {
namespace {

std::string Asset(const char* relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::string Lower(std::string text) {
  for (char& c : text) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

// Keys whose value contains `word`, read line by line (a value may sit on the line after its key).
std::set<std::string> KeysWithValueContaining(const std::string& file, const std::string& word, bool lower) {
  std::set<std::string> keys;
  std::istringstream in(file);
  std::string line;
  std::string key;
  while (std::getline(in, line)) {
    std::string value = line;
    const size_t open = line.find_first_not_of(' ');
    if (open != std::string::npos && line[open] == '"') {
      const size_t close = line.find("\": ", open);
      if (close != std::string::npos) {
        key = line.substr(open + 1, close - open - 1);
        value = line.substr(close + 3);
      } else if (line.size() > open + 1 && line.back() == ':') {
        key = line.substr(open + 1, line.size() - open - 3);
        continue;
      }
    }
    if ((lower ? Lower(value) : value).find(word) != std::string::npos) {
      keys.insert(key);
    }
  }
  return keys;
}

// Allowed exceptions: none in Chinese ("转让给" in the group chat is a different word); in English the group-chat
// "Transfer to {name}" (giving a group away).
TEST(LinkedDevicesWordingTest, NoChineseStringTalksAboutTransferringAnAccount) {
  EXPECT_TRUE(KeysWithValueContaining(Asset("locales/zh-Hans.json"), "转移", false).empty());
}

TEST(LinkedDevicesWordingTest, OnlyTheGroupOwnerStringMayStillSayTransferInEnglish) {
  const std::set<std::string> allowed{"chat.group.transfer_to"};
  EXPECT_EQ(KeysWithValueContaining(Asset("locales/en.json"), "transfer", true), allowed);
}

// 2026-11-14 review: the feature is "linking" (关联), not "moving" (搬) the account. These are the strings that describe
// it: the first-run fork, the reset dialog that points back to it, and the "linked" toast. No 搬 may come back in them.
TEST(LinkedDevicesWordingTest, TheLinkingStringsNeverSayMoveInChinese) {
  const std::string zh = Asset("locales/zh-Hans.json");
  EXPECT_TRUE(KeysWithValueContaining(zh, "搬", false).empty()) << "no 搬 anywhere in the Chinese strings";
  for (const char* key : {"pin.link_imported", "pin.identity_fork_message", "settings.storage.reset_confirm_message"}) {
    EXPECT_EQ(KeysWithValueContaining(zh, "关联", false).count(key), 1u) << key << " says 关联";
  }
  EXPECT_NE(zh.find("\"pin.link_imported\": \"已关联到这台设备\""), std::string::npos);
}

TEST(LinkedDevicesWordingTest, TheLinkingStringsSayLinkInEnglish) {
  const std::string en = Asset("locales/en.json");
  EXPECT_NE(en.find("\"pin.link_imported\": \"Linked to this device\""), std::string::npos);
  for (const char* key : {"pin.identity_fork_message", "settings.storage.reset_confirm_message"}) {
    EXPECT_EQ(KeysWithValueContaining(en, "link this device", true).count(key), 1u) << key;
    EXPECT_EQ(KeysWithValueContaining(en, "bring", true).count(key), 0u) << key << " no longer says bring";
  }
}

TEST(LinkedDevicesWordingTest, TheTitlesAndTheCodeUseTheAgreedWords) {
  const std::string zh = Asset("locales/zh-Hans.json");
  const std::string en = Asset("locales/en.json");
  EXPECT_NE(zh.find("\"settings.devices.title\": \"关联的设备\""), std::string::npos);
  EXPECT_NE(zh.find("\"settings.security.transfer_row\": \"关联新设备\""), std::string::npos);
  EXPECT_NE(zh.find("\"settings.security.link_device.copy_button\": \"复制关联码\""), std::string::npos);
  EXPECT_NE(zh.find("\"settings.security.link_device.export_done\": \"关联码已复制\""), std::string::npos);
  EXPECT_NE(zh.find("\"settings.security.link_device.export_confirm_title\": \"复制关联码？\""), std::string::npos);
  EXPECT_NE(zh.find("\"pin.link_paste_title\": \"粘贴关联码\""), std::string::npos);
  EXPECT_NE(en.find("\"settings.devices.title\": \"Linked devices\""), std::string::npos);
  EXPECT_NE(en.find("\"settings.security.transfer_row\": \"Link a new device\""), std::string::npos);
  EXPECT_NE(en.find("\"settings.security.link_device.copy_button\": \"Copy link code\""), std::string::npos);
  EXPECT_NE(en.find("\"pin.link_paste_title\": \"Paste link code\""), std::string::npos);
}

} // namespace
} // namespace pbr
