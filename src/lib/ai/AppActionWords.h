#pragma once

#include "common/ShareableLine.h"

#include <string_view>
#include <vector>

namespace pbr {

/**
 * Wordlist for the on-device "operate the app" judgement (see AppActionClassifier.h), keyed by PP tool name.
 * The Chinese lists are the ones brief_AI uses (brief_AI/data/app_actions.json), copied verbatim; the English
 * lists are ours. Words are lowercase; Chinese ones have no spaces (the message is stripped of whitespace
 * before matching), English ones are space-separated tokens.
 */
struct AppActionWords {
  std::string_view tool;
  /** A bare settings-read phrase on its own ("supported languages") counts as a request. */
  bool settings_read = false;
  std::vector<std::string_view> zh_verbs, zh_objects, zh_phrases;
  std::vector<std::string_view> en_verbs, en_objects, en_phrases;
  /** A phrase on its own ("深色模式", "switch to english" minus the verb) is already a command. */
  bool bare_command = false;
  /** The object word on its own ("通知呢", "Notifications?") hits the tool; the planner then asks on or off. */
  bool bare_object = false;
};

inline const std::vector<AppActionWords>& AppActionTable() {
  static const std::vector<AppActionWords> table = {
      {"search_people", false,
       {"找", "搜", "搜索", "查找"}, {"用户", "昵称", "peerid", "公共目录", "目录里"},
       {"搜一下用户", "搜索用户", "找用户", "查找用户", "按昵称找", "搜昵称", "找一下", "搜一下"},
       {"find", "search", "look up", "look for"},
       {"user", "users", "people", "person", "someone", "nickname", "directory", "peerid"},
       {"search for users", "find users", "find someone", "search the directory", "search by nickname"}},
      {"list_contacts", false,
       {"有哪些", "列出", "看看", "查看", "搜", "找", "显示"}, {"联系人", "通讯录", "好友"},
       {"我有哪些联系人", "通讯录里有没有", "我的联系人", "我的好友", "联系人列表", "通讯录里"},
       {"show", "list", "view", "see", "display"}, {"my contacts", "contact list", "address book"},
       {"list my contacts", "show my contacts", "view my contacts", "see my contacts", "my contact list",
        "contact list", "address book", "who are my contacts", "my friends"}},
      {"add_contact", false,
       {"加", "添加", "加上"}, {"联系人", "好友"},
       {"为好友", "加好友", "加为好友", "加到联系人", "添加联系人", "加联系人", "加个好友"},
       {"add", "save"}, {"contact", "friend"},
       {"add contact", "add a contact", "add as a friend", "add as friend", "add friend", "add a friend"}},
      {"list_conversations", false,
       {"有哪些", "列出", "看看", "查看", "显示"}, {"聊天", "会话", "对话列表"},
       {"我有哪些聊天", "列出我的会话", "我的会话", "会话列表", "聊天列表", "我的聊天"},
       {"show", "list", "view", "see", "display"}, {"my chats", "my conversations", "chat list"},
       {"my chats", "my conversations", "chat list", "conversation list"}},
      {"open_conversation", false,
       {"打开", "切到", "切换到", "进入", "跳到"}, {"聊天", "会话", "对话"},
       {"打开和", "打开跟", "打开与", "切到和", "切换到和", "进入和"},
       {"open", "switch to", "go to"}, {"chat", "conversation", "thread"},
       {"open the chat with", "open my chat with", "open the conversation with", "switch to the chat with",
        "go to the chat with"}},
      {"start_conversation", false,
       {"发消息", "发信息", "发个消息", "开个聊天", "开始聊天", "私聊"}, {"给", "和", "跟", "与", "联系人"},
       {"发消息给", "给他发消息", "开个聊天", "开始聊天", "和他聊天", "私聊"},
       {"send", "start"}, {"message to", "chat with", "conversation with"},
       {"send a message to", "send message to", "send him a message", "send her a message",
        "send them a message", "start a chat with", "start a conversation with"}},
      {"register_user", false,
       {"注册", "登记"}, {"账号", "这台设备", "到网络", "新用户"},
       {"帮我注册", "注册账号", "注册一下", "我要注册", "注册到网络", "注册这台设备"},
       {"register", "sign up"}, {"account", "this device", "new user", "the network"},
       {"register an account", "register this device", "register me", "sign me up", "create an account",
        "register on the network", "register to the network"}},
      {"update_profile_nickname", false,
       {"改", "改成", "修改", "换", "换成", "设置", "设成"}, {"昵称", "网名", "用户名"},
       {"改昵称", "改个昵称", "换昵称", "修改昵称", "昵称改成", "名字改成"},
       {"change", "set", "update", "rename", "edit"}, {"nickname", "display name", "username", "screen name"},
       {"change my nickname", "change nickname", "set my nickname", "update my nickname",
        "change my display name"}},
      {"get_profile_identity", true,
       {"看看", "查看", "查", "显示", "是什么", "是多少"}, {"账号信息", "peerid", "注册状态", "身份信息", "个人信息"},
       {"我的账号信息", "我注册了吗", "我的id是", "我的peerid", "注册状态", "我的身份", "账号信息"},
       {"show", "view", "see", "check"}, {"account info", "peerid", "registration status", "my identity", "my profile"},
       {"my account info", "am i registered", "my peerid", "my registration status", "my identity", "my profile"}},
      {"get_preferences", true,
       {"看看", "查看", "是什么", "显示", "看一下", "看下", "看", "瞧瞧"}, {"设置", "偏好", "配置"},
       {"我现在的设置", "我的设置是什么", "当前设置", "我的偏好设置", "看看设置", "我的设置", "看一下设置", "看下设置",
        "查一下设置"},
       {"show", "view", "see", "check"}, {"my settings", "my preferences", "app settings", "app preferences"},
       {"my settings", "my preferences", "current settings", "app settings"}},
      {"get_security_status", true,
       {"看看", "查看", "查", "检查", "看一下", "看下", "查一下", "瞧瞧"}, {"安全状态", "安全设置", "pin"},
       {"我的账号安全吗", "账号安全吗", "安全状态", "pin设好了吗", "账户安全"},
       {"check", "show", "view"}, {"security status", "security settings", "pin"},
       {"is my account secure", "is my account safe", "my security status", "security status", "account security"}},
      {"get_network_settings", true,
       {"看看", "查看", "是什么", "显示", "看一下", "看下", "看", "瞧瞧"}, {"网络设置", "节点设置", "中继设置", "网络参与"},
       {"我的网络设置", "网络设置是什么", "看看网络设置", "节点设置", "看一下网络设置", "看下网络设置"},
       {"show", "view", "see", "check"}, {"network settings", "node settings", "relay settings"},
       {"my network settings", "network settings", "node settings"}},
      {"get_reachability", true,
       {"能不能", "能否", "可不可以", "通不通", "看看", "查看", "看一下", "看下", "看", "查一下", "瞧瞧"},
       {"连上", "连接到我", "可达", "被联系", "网络状态", "网络情况", "连接状态"},
       {"能被连上吗", "能被别人连上", "能不能被连上", "网络通不通", "可达状态", "别人能连到我吗", "能连上我吗",
        "网络状态", "网络情况", "连接状态"},
       {"show", "view", "see", "check"}, {"network status", "connection status"},
       {"can others reach my device", "can people reach my device", "am i reachable", "is my device reachable",
        "can others connect to my device", "can anyone connect to my device", "my reachability",
        "reachability status", "network status", "my network status", "connection status"}},
      {"get_llm_settings", true,
       {"用的是", "是什么", "看看", "查看"}, {"模型", "ai设置", "助手设置"},
       {"ai用的是什么模型", "用的是什么模型", "当前模型是什么", "助手用的模型", "模型设置"},
       {"show", "check", "view"}, {"ai settings", "model settings"},
       {"what model are you using", "what model do you use", "which model do you use", "which model are you using",
        "which ai model", "ai settings", "model settings"}},
      {"get_integrations", true,
       {"接了", "连了", "开了", "看看", "查看"}, {"外部服务", "mcp", "搜索服务", "集成"},
       {"接了哪些外部服务", "接了哪些服务", "mcp服务", "搜索服务开了吗"},
       {"show", "list", "check", "view"}, {"external services", "integrations"},
       {"which external services", "what integrations", "my integrations", "connected services"}},
      {"list_locales", true,
       {"支持", "有哪些", "可选"}, {"界面语言"},
       {"支持哪些语言", "可选的语言", "界面语言有哪些", "哪些界面语言", "有哪些语言可以选"},
       {}, {},
       {"which languages are supported", "supported languages", "available languages", "what languages are available",
        "what languages do you support", "interface languages", "which languages can i choose"}},
      {"set_appearance", false,
       {"换成", "切换", "改成", "换", "开", "打开", "用", "改到", "改为"},
       {"深色模式", "浅色模式", "夜间模式", "暗色", "跟随系统", "外观"},
       {"换成深色", "换成浅色", "切换深色", "切换浅色", "开深色模式", "开夜间模式", "跟随系统外观", "改外观", "深色模式",
        "浅色模式", "夜间模式", "暗色模式", "跟随系统"},
       {"switch", "change", "turn", "enable", "set"},
       {"dark mode", "light mode", "night mode", "dark theme", "light theme", "appearance"},
       {"follow the system", "follow system", "match the system", "system theme", "system appearance",
        "switch to dark mode", "switch to light mode", "enable dark mode", "turn on dark mode", "turn off dark mode",
        "use dark mode", "use light mode", "change the appearance", "change appearance", "dark mode", "light mode",
        "night mode"},
       true},
      {"set_language", false,
       {"改成", "换成", "切换", "切到", "设成", "设置成", "用", "换到", "改到"},
       {"英文", "中文", "英语", "简体", "繁体", "日文", "日语", "界面语言"},
       {"界面改成英文", "界面改成中文", "改成英文界面", "改成中文界面", "切换语言", "换个语言", "语言改成", "界面语言",
        "切换到中文", "切换到英文", "切换到英语", "切换到简体", "切换到繁体", "切换到日文", "切换到日语", "切到中文",
        "切到英文", "换成中文", "换成英文", "换成英语", "改成中文", "改成英文", "改成英语", "换到中文", "换到英文"},
       {"change", "switch", "set"},
       {"english", "chinese", "simplified chinese", "traditional chinese", "japanese", "interface language",
        "app language"},
       {"change the language", "change language", "switch language", "change the interface language",
        "change the app language", "switch to english", "switch to chinese", "set the language to"},
       true},
      {"set_notifications", false,
       {"关掉", "关闭", "关", "打开", "开启", "开", "静音"}, {"通知", "提醒", "推送"},
       {"关掉通知", "关闭通知", "打开通知", "开启通知", "别再通知", "通知关了", "关通知", "开通知"},
       {"turn", "disable", "enable", "mute"}, {"notifications", "notification", "alerts"},
       {"turn off notifications", "turn on notifications", "disable notifications", "enable notifications",
        "mute notifications", "stop notifications"}, false, true},
      {"set_reduce_transparency", false,
       {"关掉", "关闭", "关", "打开", "开启", "开", "减少"}, {"毛玻璃", "透明效果", "透明度", "半透明"},
       {"关掉毛玻璃", "毛玻璃效果", "减少透明", "透明效果", "关掉透明"},
       {"turn", "disable", "enable", "reduce"}, {"transparency", "blur", "frosted glass"},
       {"reduce transparency", "turn off transparency", "disable transparency", "turn off the blur", "disable blur",
        "reduce the blur", "turn off frosted glass", "disable frosted glass"}, false, true},
      {"set_group_invite_policy", false,
       {"不让", "别让", "禁止", "允许", "只让", "设置"}, {"拉我进群", "拉进群", "群邀请", "邀请我"},
       {"不让陌生人拉我进群", "拉我进群", "群邀请", "谁可以拉我", "陌生人拉我", "进群邀请"},
       {"block", "disable", "allow", "stop", "don't let", "do not let"},
       {"group invites", "group invitations", "add me to groups", "invite me to groups", "add me to a group",
        "invite me to a group"},
       {"block group invites", "disable group invites", "who can add me to groups", "who can invite me to groups",
        "group invite settings"}},
      {"set_auto_renew_registration", false,
       {"打开", "开启", "关掉", "关闭", "开", "关"}, {"自动续期", "续期", "自动续约"},
       {"自动续期", "打开续期", "关掉续期", "自动续约"},
       {"turn", "enable", "disable"}, {"auto renew", "auto renewal", "automatic renewal"},
       {"auto renew my registration", "auto renew registration", "renew my registration automatically",
        "registration auto renew"}, false, true},
      {"set_call_diagnostics", false,
       {"打开", "开启", "关掉", "关闭", "显示", "开", "关"}, {"通话诊断", "诊断信息"},
       {"通话诊断", "诊断信息", "打开诊断", "关掉诊断"},
       {"turn", "enable", "disable", "show", "hide"}, {"call diagnostics"},
       {"call diagnostics", "turn on call diagnostics", "turn off call diagnostics"}, false, true},
      {"set_node_enabled", false,
       {"打开", "开启", "关掉", "关闭", "开", "关", "当"}, {"节点模式", "节点", "中继节点"},
       {"节点模式", "打开节点", "关掉节点", "当节点", "作为节点"},
       {"turn", "enable", "disable", "become"}, {"node mode", "relay node"},
       {"node mode", "enable node mode", "become a node", "run as a node", "act as a node"}, false, true},
      {"set_mesh_capabilities", false,
       {"打开", "开启", "关掉", "关闭", "开", "关"}, {"中继", "dht", "mesh", "网络能力"},
       {"打开中继", "关掉中继", "打开dht", "关掉dht", "中继功能", "dht功能", "开中继", "关中继"},
       {"turn", "enable", "disable"}, {"relay", "dht", "mesh", "network capabilities"},
       {"turn on relay", "turn off relay", "enable relay", "disable relay", "turn on dht", "turn off dht",
        "enable dht", "disable dht", "mesh capabilities"}},
      {"probe_reachability", false,
       {"重新检测", "重新测", "再测", "检测", "测试", "测一下", "检查"}, {"网络", "可达", "连通", "连接"},
       {"重新检测一下网络", "重新检测网络", "再测一下网络", "检测一下网络", "测一下网络", "检测网络连通", "重新检测"},
       {"test", "check", "recheck", "retest", "probe"}, {"my network", "network connectivity", "reachability"},
       {"test my network", "check my network", "recheck my network", "retest my network",
        "test network connectivity", "check network connectivity", "run a network test"}},
      {"reset_tool_permissions", false,
       {"重置", "清掉", "清除", "清空", "重设"}, {"权限", "工具权限", "ai权限", "总是允许"},
       {"重置ai的权限", "重置权限", "清掉权限", "重置工具权限", "工具权限"},
       {"reset", "clear", "revoke"}, {"tool permissions", "ai permissions", "assistant permissions", "always allow"},
       {"reset ai permissions", "reset tool permissions", "clear ai permissions", "clear tool permissions",
        "reset assistant permissions"}},
      // Narrower than brief_AI's list on purpose: a bare 客服 object (or 找客服 / 联系客服) also reads "帮我联系淘宝客服"
      // as PP Support, so the device only takes sentences that ask for support themselves ("我要找客服", "转人工") or
      // name PP's ("联系 PP 客服"). Anything looser stays with brief_AI, which can still hand it back.
      {"open_support", false,
       {"找", "联系", "转", "转到", "接通"}, {"pp客服"},
       {"我要找客服", "我想找客服", "帮我找客服", "我要联系客服", "我想联系客服", "帮我联系客服", "我要客服", "转人工",
        "转人工客服", "我要人工客服", "找人工客服", "帮我转客服", "转客服", "转到客服", "接通客服", "联系pp客服", "找pp客服",
        "转到pp客服", "接通pp客服", "打开pp客服", "联系pp支持", "找pp支持", "打开pp支持"},
       {"contact", "open", "talk to", "speak to"}, {"pp support"},
       {"contact support", "contact customer service", "contact customer support", "contact pp support",
        "customer service", "customer support", "talk to support", "talk to customer service", "talk to a human",
        "speak to a human", "open pp support"},
       true},
  };
  return table;
}

/**
 * The table's tools as words for a shareable line (common/ShareableLine.h). A constexpr list of its own: MSVC
 * rejects a ShareWord built inside the table's runtime initializer (C7595). app_action_classifier_test checks
 * that every table tool is here.
 */
inline constexpr ShareWord kAppActionToolWords[] = {
    "search_people", "list_contacts", "add_contact", "list_conversations",
    "open_conversation", "start_conversation", "register_user", "update_profile_nickname",
    "get_profile_identity", "get_preferences", "get_security_status", "get_network_settings",
    "get_reachability", "get_llm_settings", "get_integrations", "list_locales",
    "set_appearance", "set_language", "set_notifications", "set_reduce_transparency",
    "set_group_invite_policy", "set_auto_renew_registration", "set_call_diagnostics", "set_node_enabled",
    "set_mesh_capabilities", "probe_reachability", "reset_tool_permissions", "open_support"};

/** `tool` as a word for a shareable line: one of the table's tools, else `other` (a tool from an MCP server). */
inline ShareWord AppActionToolWord(std::string_view tool) {
  if (tool.empty()) {
    return ShareWord("none");
  }
  for (const ShareWord& word : kAppActionToolWords) {
    if (word.view() == tool) {
      return word;
    }
  }
  return ShareWord("other");
}

} // namespace pbr
