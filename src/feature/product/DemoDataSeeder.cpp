#include "feature/product/DemoDataSeeder.h"

#include "domain/attachments/AttachmentContent.h"
#include "common/Logger.h"
#include "common/Utilities.h"
#include "common/directory/DirectoryJson.h"
#include "feature/conversations/ConversationsHub.h"
#include "foundation/crypto/AttachmentContentHash.h"
#include "foundation/platform/AssetIO.h"
#include "foundation/platform/IAssetLocator.h"
#include "foundation/platform/os/OsTime.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/delivery/ReactionTypes.h"
#include "lib/messaging/QuoteReply.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <chrono>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

namespace pbr {

namespace {

constexpr int64_t kDayMs = 24LL * 60 * 60 * 1000;

auto& log() {
  static auto logger = logging::getLogger("DemoData");
  return logger;
}

/** A time as a fixed offset from today 00:00: `days_ago` whole days back, then `hour:minute` of that day. */
struct At {
  int days_ago;
  int hour;
  int minute;
};

enum class Who { Them, Me };
/** An attachment line: a picture, or (group chat only) a file card / a video card carrying the landscape picture's
 *  bytes under another type, so the cards can be looked at without more bundled assets. */
enum class Pic { None, Landscape, Portrait, File, Video };

struct Line {
  Who who;
  const char* text;  // for a picture: its file name
  At at;
  Pic pic = Pic::None;
  /** Group chats only: index into the group's members (1-based; 0 = not used for direct chats). */
  int member = 0;
  /** Quote reply: the text it replies to (composed with ComposeQuoteReply). */
  const char* quote = nullptr;
  /** Group chats only: the sender is this account id, a member who is not a contact (`member` is then unused). */
  const char* stranger = nullptr;
};

/** A reaction on a group line: `line` is 1-based into the group's lines; `member` 1-based, 0 = the user. */
struct DemoReaction {
  int line;
  int member;
  const char* emoji;
};

struct DemoContact {
  const char* id;
  const char* name;
};

// Chinese, Latin and digit-led names, so the phone Contacts index has several sections and a "#".
constexpr std::array<DemoContact, 18> kContacts = {{
    {"demo-contact-01", "李小明"},
    {"demo-contact-02", "Emma Johnson"},
    {"demo-contact-03", "王芳 Fang"},
    {"demo-contact-04", "Alexander Maximilian von Hohenzollern-Sigmaringen 三世（一个很长很长很长的演示名字）"},
    {"demo-contact-05", "陈老师"},
    {"demo-contact-06", "Mike"},
    {"demo-contact-07", "张伟"},
    {"demo-contact-08", "Sophie Martin"},
    {"demo-contact-09", "刘洋"},
    {"demo-contact-10", "David 陈大卫"},
    {"demo-contact-11", "赵敏"},
    {"demo-contact-12", "Olivia"},
    {"demo-contact-13", "黄磊"},
    {"demo-contact-14", "Bob Chen"},
    {"demo-contact-15", "孙丽"},
    {"demo-contact-16", "Grace"},
    {"demo-contact-17", "123 Studio"},
    {"demo-contact-18", "周杰"},
}};

struct DirectChat {
  int contact;  // index into kContacts
  int unread;
  std::vector<Line> lines;
};

const std::vector<DirectChat>& DirectChats() {
  static const std::vector<DirectChat> chats = {
      {0, 0,
       {
           {Who::Them, "在吗？晚上一起吃饭吧", {3, 18, 20}},
           {Who::Me, "好啊，去哪儿？", {3, 18, 22}},
           {Who::Them, "新开的那家火锅，七点半", {3, 18, 25}},
           {Who::Me, "行，我准时到", {3, 18, 26}},
           {Who::Them, "对了，记得带上上次说的那本书", {1, 21, 5}},
           {Who::Me, "没问题，今天就放包里", {1, 21, 9}, Pic::None, 0, "对了，记得带上上次说的那本书"},
           {Who::Them, "谢谢！那明天见", {0, 10, 42}},
       }},
      {1, 3,
       {
           {Who::Me, "Hi Emma, did the files arrive?", {1, 9, 12}},
           {Who::Them, "Yes, got them. Thanks!", {1, 9, 40}},
           {Who::Them, "One question about page 3", {0, 8, 15}},
           {Who::Them, "Could you check the numbers again?", {0, 8, 16}},
           {Who::Them, "No rush, any time today works.", {0, 8, 17}},
       }},
      {2, 0,
       {
           {Who::Them,
            "周末活动的安排我整理了一下：\n1. 周六上午 9:00 在公园门口集合\n2. 中午自带午餐，下午三点前返回\n"
            "3. 如果下雨就改到室内，地点另行通知\n大家看看有没有问题，Please let me know if anything conflicts.",
            {9, 14, 3}},
           {Who::Me, "收到，周六我可以，OK 👍", {9, 14, 30}},
           {Who::Them, "太好了，到时候见", {9, 14, 31}},
       }},
      {3, 120,
       {
           {Who::Them, "Please find the quarterly report attached to my previous mail.", {2, 11, 0}},
           {Who::Me, "Received, I will read it tonight.", {2, 11, 30}},
           {Who::Them, "报告里第二部分的数据还需要你确认一下。", {1, 16, 45}},
       }},
      {4, 0,
       {
           {Who::Them, "这是上次课堂上拍的照片", {5, 15, 10}},
           {Who::Them, "课堂.png", {5, 15, 11}, Pic::Landscape},
           {Who::Me, "真好看，谢谢老师", {5, 15, 40}},
       }},
      {5, 0,
       {
           {Who::Me, "This is the poster draft", {1, 20, 3}},
           {Who::Me, "poster.png", {1, 20, 4}, Pic::Portrait},
           {Who::Them, "Looks great, ship it", {1, 20, 30}},
       }},
      {6, 0,
       {
           {Who::Them, "明天的会议改到下午两点", {0, 7, 55}},
           {Who::Me, "好的", {0, 7, 58}},
       }},
      {7, 0,
       {
           {Who::Them, "Merci beaucoup for yesterday!", {3, 22, 10}},
           {Who::Me, "You are welcome, see you next week", {3, 22, 30}},
       }},
      {8, 0,
       {
           {Who::Me, "最近怎么样？", {9, 12, 0}},
           {Who::Them, "挺好的，刚搬了新家", {9, 12, 50}},
       }},
      {9, 0,
       {
           {Who::Them, "Dinner on Friday?", {6, 19, 15}},
           {Who::Me, "周五可以，几点？", {6, 19, 20}},
           {Who::Them, "7pm 怎么样", {6, 19, 21}},
       }},
  };
  return chats;
}

struct DemoGroup {
  const char* thread_id;
  const char* group_id;
  const char* title;
  std::vector<int> members;  // indexes into kContacts (the user is a member too)
  int unread;
  std::vector<Line> lines;  // `member` is 1-based into `members`
  std::vector<DemoReaction> reactions;
  /** Members who are not contacts: account ids (the senders of `stranger` lines). */
  std::vector<std::string> strangers;
};

const std::vector<DemoGroup>& Groups() {
  static const std::vector<DemoGroup> groups = {
      {"demo-thread-group-1",
       "demo-group-1",
       "周末爬山小队",
       {0, 1, 2},
       0,
       {
           {Who::Them, "这周六还去爬山吗？", {4, 20, 1}, Pic::None, 1},
           {Who::Them, "I am in! Weather looks good.", {4, 20, 5}, Pic::None, 2},
           {Who::Me, "我也去，早上八点集合", {4, 20, 9}},
           {Who::Them, "八点太早了吧……九点行不行", {4, 20, 12}, Pic::None, 3},
           {Who::Them, "Fine by me, 9 it is.", {4, 20, 15}, Pic::None, 2},
           {Who::Them, "上周的照片终于整理好了", {1, 19, 2}, Pic::None, 1},
           {Who::Them, "山顶.png", {1, 19, 3}, Pic::Portrait, 1},
           {Who::Them, "云海.png", {1, 19, 4}, Pic::Landscape, 1},
           {Who::Them, "Amazing view 😍", {1, 19, 10}, Pic::None, 2},
           {Who::Me, "路线图和装备清单.pdf", {1, 19, 20}, Pic::File},
           {Who::Them, "下山.mp4", {1, 19, 30}, Pic::Video, 3},
           {Who::Them, "我是小明的同事，也想一起去，可以吗？", {0, 8, 5}, Pic::None, 0, nullptr, "demo-acct-guest-01"},
           {Who::Me, "欢迎！周六九点见", {0, 8, 7}},
       },
       {{7, 0, "😍"}, {7, 3, "😍"}, {6, 2, "👍"}, {9, 1, "🙏"}, {12, 2, "👍"}, {12, 3, "👍"}, {12, 1, "🎉"}, {13, 1, "❤️"}, {10, 2, "👍"}},
       {"demo-acct-guest-01"}},
      {"demo-thread-group-2",
       "demo-group-2",
       "Project Sync 项目讨论",
       {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10},
       3,
       {
           {Who::Them, "Kickoff notes are in the shared folder.", {2, 10, 0}, Pic::None, 2},
           {Who::Them, "收到，我先看一下需求部分", {2, 10, 12}, Pic::None, 1},
           {Who::Me, "我负责界面这块，下周一给大家看第一版", {2, 10, 40}},
           {Who::Them, "Sounds good. Can we also add a review step before the release?", {1, 15, 5}, Pic::None, 6},
           {Who::Them, "同意，我来排一下时间", {0, 9, 30}, Pic::None, 10},
           {Who::Them, "Meeting moved to 3pm, same link.", {0, 9, 45}, Pic::None, 7},
           {Who::Them, "大家今天辛苦了", {0, 9, 50}, Pic::None, 4},
       },
       {},
       {}},
  };
  return groups;
}

const std::vector<Line>& AiLines() {
  static const std::vector<Line> lines = {
      {Who::Me, "帮我规划一个三天的京都行程", {2, 21, 0}},
      {Who::Them,
       "好的！第一天可以安排清水寺、二年坂三年坂和祇园，晚上在先斗町吃饭；第二天去伏见稻荷和岚山；"
       "第三天留给金阁寺和锦市场。需要我按预算再细化吗？",
       {2, 21, 1}},
      {Who::Me, "预算大概每天一千元人民币", {2, 21, 3}},
      {Who::Them,
       "那午餐可以选定食屋，晚餐一天正式一天简单。交通建议买一日券。Tip: book the popular restaurants early.",
       {2, 21, 4}},
      {Who::Me, "谢谢，很有帮助", {2, 21, 6}},
  };
  return lines;
}

std::string ThreadIdForDirect(const int index) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "demo-thread-%02d", index + 1);
  return buf;
}

constexpr const char* kAiThreadId = "demo-thread-ai";

/** A direct chat with someone who is not a contact, unread: Home's "New people" widget shows it. */
constexpr const char* kStrangerThreadId = "demo-thread-stranger";
constexpr const char* kStrangerName = "林小雨";
constexpr const char* kStrangerAccount = "demo-acct-stranger-01";

const std::vector<Line>& StrangerLines() {
  static const std::vector<Line> lines = {
      {Who::Them, "你好，我是读书会的小雨，在群里看到你的", {0, 9, 2}},
      {Who::Them, "想问问上次你推荐的那本书叫什么名字？", {0, 9, 3}},
  };
  return lines;
}

/** Not secret and not a real key: fixed, so reseeding produces byte-identical rows. Only passes the format checks. */
ByteVector FixedBytes(const size_t size, const uint8_t seed) {
  ByteVector out(size);
  for (size_t i = 0; i < size; ++i) {
    out[i] = static_cast<uint8_t>(seed + i);
  }
  return out;
}

struct Picture {
  ByteVector bytes;
  ChatAttachmentFields fields;
};

Roe<Picture> LoadPicture(const DemoSeedDeps& deps, const char* asset, const char* filename, const uint8_t seed) {
  auto bytes = deps.read_asset(asset);
  if (!bytes) {
    return Error(std::string("demo picture unreadable: ") + asset + ": " + bytes.error().message);
  }
  if (bytes->empty()) {
    return Error(std::string("demo picture empty: ") + asset);
  }
  auto hash = AttachmentContentHash(*bytes);
  if (!hash) {
    return hash.error();
  }
  Picture picture;
  picture.fields.mime = "image/png";
  picture.fields.filename = filename;
  picture.fields.byte_length = bytes->size();
  picture.fields.content_hash = *hash;
  picture.fields.content_key = FixedBytes(32, seed);
  picture.fields.blob_nonce = FixedBytes(24, static_cast<uint8_t>(seed + 100));
  picture.bytes = std::move(*bytes);
  return picture;
}

/**
 * `at` on the local calendar day `days_ago` before today (a DST change makes some days 23 or 25 hours, so this steps the
 * calendar date, not 24h). A time that falls after `now_ms` (a "today 18:20" seen at 09:00) becomes now minus a few
 * minutes; the clamp is monotonic, so the messages keep their order (display_order breaks ties).
 */
int64_t TimeOf(const DemoSeedDeps& deps, const At at) {
  constexpr int64_t kFutureGapMs = 5LL * 60 * 1000;
  int64_t result = deps.today_midnight_ms - at.days_ago * kDayMs + (at.hour * 60 + at.minute) * 60 * 1000;
  std::tm local{};
  if (os::LocalTime(static_cast<std::time_t>(deps.today_midnight_ms / 1000), &local)) {
    local.tm_mday -= at.days_ago;
    local.tm_hour = at.hour;
    local.tm_min = at.minute;
    local.tm_sec = 0;
    local.tm_isdst = -1;
    const std::time_t t = std::mktime(&local);
    if (t != static_cast<std::time_t>(-1)) {
      result = static_cast<int64_t>(t) * 1000;
    }
  }
  if (deps.now_ms > 0 && result > deps.now_ms - kFutureGapMs) {
    result = deps.now_ms - kFutureGapMs;
  }
  return result;
}

/** The attachment fields of a picture / file / video line (a file or video: the landscape bytes, its own name). */
ChatAttachmentFields FieldsOf(const Line& line, const Picture& landscape, const Picture& portrait) {
  if (line.pic == Pic::Portrait) {
    return portrait.fields;
  }
  ChatAttachmentFields fields = landscape.fields;
  if (line.pic == Pic::File || line.pic == Pic::Video) {
    fields.mime = line.pic == Pic::File ? "application/pdf" : "video/mp4";
    fields.filename = line.text;
  }
  return fields;
}

/** The reactions of a group: annotation rows after its lines, each on its target line's message. */
Roe<void> PutReactions(const DemoSeedDeps& deps, const std::string& thread_id, const std::string& id_prefix,
                       const std::vector<Line>& lines, const std::vector<DemoReaction>& reactions,
                       const std::vector<std::string>& sender_of_member) {
  int order = static_cast<int>(lines.size());
  for (const DemoReaction& reaction : reactions) {
    ++order;
    const std::string target = id_prefix + "-" + std::to_string(reaction.line);
    ThreadMessage message;
    message.id = id_prefix + "-r" + std::to_string(order);
    message.thread_id = thread_id;
    message.display_order = order;
    message.timestamp = TimeOf(deps, lines.back().at);  // after the last line: times keep the display order
    message.transport = MessageTransport::Relay;
    message.delivery = MessageDelivery::Relayed;
    message.content_type = ChatContentType::Annotation;
    message.target_message_id = target;
    message.text = reaction.emoji;
    message.payload_json = BuildReactionPayloadJson(kAnnotationTypeReaction, target, reaction.emoji);
    if (reaction.member == 0) {
      message.sender_contact_id = kLocalSelfContactId;
      message.relay_visible = false;
    } else {
      message.sender_contact_id = sender_of_member.at(static_cast<size_t>(reaction.member - 1));
    }
    if (auto put = deps.threads.UpsertMessage(message); !put) {
      return put.error();
    }
  }
  return {};
}

Roe<void> PutMessages(const DemoSeedDeps& deps, const std::string& thread_id, const std::string& id_prefix,
                      const std::vector<Line>& lines, const std::vector<std::string>& sender_of_member,
                      const Picture& landscape, const Picture& portrait) {
  int order = 0;
  for (const Line& line : lines) {
    ++order;
    ThreadMessage message;
    message.id = id_prefix + "-" + std::to_string(order);
    message.thread_id = thread_id;
    message.display_order = order;
    message.timestamp = TimeOf(deps, line.at);
    message.transport = MessageTransport::Relay;
    // Never Pending: nothing here is waiting to be sent. Our own lines are relay_visible=false (no outbox row).
    message.delivery = MessageDelivery::Relayed;
    if (line.who == Who::Me) {
      message.sender_contact_id = kLocalSelfContactId;
      message.relay_visible = false;
    } else {
      message.sender_contact_id = line.stranger != nullptr ? std::string(line.stranger)
                                  : line.member > 0      ? sender_of_member.at(static_cast<size_t>(line.member - 1))
                                                         : sender_of_member.at(0);
    }
    if (line.pic != Pic::None) {
      message.content_type = ChatContentType::Attachment;
      message.text = line.text;
      message.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(FieldsOf(line, landscape, portrait));
    } else if (line.quote != nullptr) {
      message.text = ComposeQuoteReply(line.text, line.quote);
    } else {
      message.text = line.text;
    }
    if (auto put = deps.threads.UpsertMessage(message); !put) {
      return put.error();
    }
  }
  return {};
}

/** A local-only account id for a demo contact: lets the chat list resolve the contact's name for a direct chat. The
 *  demo app has no relay, directory or mesh, so this id is never looked up or routed anywhere. */
std::string DemoAccountId(const std::string& contact_id) {
  return "demo-acct-" + contact_id;
}

Thread ThreadFor(const std::string& id, const ThreadKind kind, const std::string& title,
                 std::vector<std::string> participants, const std::vector<Line>& lines, const int unread,
                 const DemoSeedDeps& deps) {
  Thread thread;
  thread.id = id;
  thread.kind = kind;
  thread.channel = kind == ThreadKind::Direct ? ThreadChannel::E2ePublic : ThreadChannel::None;
  thread.title = title;
  thread.participant_contact_ids = std::move(participants);
  thread.encrypted = kind != ThreadKind::Ai;
  thread.unread_count = unread;
  thread.listed = true;
  const Line& last = lines.back();
  thread.updated_at = TimeOf(deps, last.at);
  thread.preview = last.pic != Pic::None || last.quote == nullptr ? last.text : ComposeQuoteReply(last.text, last.quote);
  return thread;
}

} // namespace

int64_t LocalMidnightMs(const int64_t now_ms) {
  const std::time_t seconds = static_cast<std::time_t>(now_ms / 1000);
  std::tm local{};
  if (!os::LocalTime(seconds, &local)) {
    return now_ms - now_ms % kDayMs;
  }
  local.tm_hour = 0;
  local.tm_min = 0;
  local.tm_sec = 0;
  local.tm_isdst = -1;
  return static_cast<int64_t>(std::mktime(&local)) * 1000;
}

Roe<ByteVector> ReadDemoAsset(const std::string& relative) {
  std::string text;
  if (!AssetIO::ReadText(IAssetLocator::Instance().Resolve(relative), text)) {
    return Error("asset not found: " + relative);
  }
  return ByteVector(text.begin(), text.end());
}

Roe<bool> DemoDataSeeder::SeedOnce(const DemoSeedDeps& deps) {
  if (done_) {
    return false;
  }
  if (!deps.own_relay_user_id.empty()) {
    log().warning << "Demo data not written: this account is registered";
    done_ = true;
    return false;
  }
  if (!deps.read_asset) {
    return Error("demo data: no asset reader");
  }

  // Pictures first: if one cannot be read, stop before any row is written (no half data, no empty picture message).
  auto landscape = LoadPicture(deps, "demo/demo_landscape.png", "IMG_2041.png", 1);
  if (!landscape) {
    return landscape.error();
  }
  auto portrait = LoadPicture(deps, "demo/demo_portrait.png", "IMG_2042.png", 51);
  if (!portrait) {
    return portrait.error();
  }

  for (const DemoContact& contact : kContacts) {
    Contact row;
    row.id = contact.id;
    row.remote.nickname = contact.name;
    row.remote.fetched_at = deps.now_ms;
    row.remote.ids = {mesh::ContactId{mesh::ContactIdKind::Account, DemoAccountId(contact.id), true}};
    row.local.trust = TrustLevel::Friendly;
    SyncContactMirrors(row);
    if (auto put = deps.contacts.Upsert(row); !put) {
      return put.error();
    }
  }

  const auto& directs = DirectChats();
  for (size_t i = 0; i < directs.size(); ++i) {
    const DirectChat& chat = directs[i];
    const DemoContact& contact = kContacts[static_cast<size_t>(chat.contact)];
    const std::string thread_id = ThreadIdForDirect(static_cast<int>(i));
    Thread thread = ThreadFor(thread_id, ThreadKind::Direct, contact.name, {contact.id}, chat.lines, chat.unread, deps);
    thread.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
    thread.peer_identity_value = DemoAccountId(contact.id);
    if (auto put = deps.threads.UpsertThread(thread); !put) {
      return put.error();
    }
    if (auto put = PutMessages(deps, thread_id, "demo-msg-" + std::to_string(i + 1), chat.lines, {contact.id},
                               *landscape, *portrait);
        !put) {
      return put.error();
    }
    if (auto put = deps.threads.UpsertThread(thread); !put) {  // the final unread / preview / time
      return put.error();
    }
    for (const Line& line : chat.lines) {
      if (line.pic == Pic::None) {
        continue;
      }
      const Picture& picture = line.pic == Pic::Landscape ? *landscape : *portrait;
      if (auto copied = CopyAttachmentPlaintext(deps.profile_dir, deps.content, thread_id, picture.fields,
                                                picture.bytes, deps.attachment_dek, deps.max_receive_bytes);
          !copied) {
        return copied.error();
      }
    }
  }

  {
    if (deps.put_directory_hit) {
      mesh::DirectoryHit hit;
      hit.hit_id = kStrangerAccount;
      hit.nickname = kStrangerName;
      hit.account_id = std::string(kStrangerAccount);
      hit.ids = {mesh::ContactId{mesh::ContactIdKind::Account, kStrangerAccount, true}};
      deps.put_directory_hit(hit);
    }
    Thread thread = ThreadFor(kStrangerThreadId, ThreadKind::Direct, kStrangerName, {}, StrangerLines(), 2, deps);
    thread.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
    thread.peer_identity_value = kStrangerAccount;
    if (auto put = deps.threads.UpsertThread(thread); !put) {
      return put.error();
    }
    if (auto put = PutMessages(deps, kStrangerThreadId, "demo-msg-stranger", StrangerLines(), {kStrangerAccount},
                               *landscape, *portrait);
        !put) {
      return put.error();
    }
    if (auto put = deps.threads.UpsertThread(thread); !put) {
      return put.error();
    }
  }

  int group_no = 0;
  for (const DemoGroup& group : Groups()) {
    ++group_no;
    std::vector<std::string> member_contacts;
    for (const int member : group.members) {
      member_contacts.push_back(kContacts[static_cast<size_t>(member)].id);
    }
    Thread thread = ThreadFor(group.thread_id, ThreadKind::Group, group.title, member_contacts, group.lines,
                              group.unread, deps);
    thread.group_id = group.group_id;
    if (auto put = deps.threads.UpsertThread(thread); !put) {
      return put.error();
    }

    GroupMetadata metadata;
    metadata.group_id = group.group_id;
    metadata.owner_identity = kLocalSelfContactId;
    metadata.title = group.title;
    metadata.sync_state = GroupSyncState::Active;
    if (auto put = deps.groups.UpsertMetadata(metadata); !put) {
      return put.error();
    }
    const int64_t joined = TimeOf(deps, {30, 12, 0});
    if (auto put = deps.groups.UpsertMember(group.group_id,
                                            {kLocalSelfContactId, kLocalSelfContactId, MemberRole::Owner, joined});
        !put) {
      return put.error();
    }
    for (const std::string& id : member_contacts) {
      if (auto put = deps.groups.UpsertMember(group.group_id, {id, id, MemberRole::Member, joined}); !put) {
        return put.error();
      }
    }
    for (const std::string& account : group.strangers) {
      if (auto put = deps.groups.UpsertMember(group.group_id, {account, account, MemberRole::Member, joined}); !put) {
        return put.error();
      }
    }
    if (auto put = deps.groups.UpsertGroupTarget(group.group_id, group.thread_id, 1, 1); !put) {
      return put.error();
    }
    const std::string prefix = "demo-msg-g" + std::to_string(group_no);
    if (auto put = PutMessages(deps, group.thread_id, prefix, group.lines, member_contacts, *landscape, *portrait);
        !put) {
      return put.error();
    }
    if (auto put = PutReactions(deps, group.thread_id, prefix, group.lines, group.reactions, member_contacts); !put) {
      return put.error();
    }
    for (const Line& line : group.lines) {
      if (line.pic == Pic::None) {
        continue;
      }
      const Picture& picture = line.pic == Pic::Portrait ? *portrait : *landscape;
      if (auto copied = CopyAttachmentPlaintext(deps.profile_dir, deps.content, group.thread_id,
                                                FieldsOf(line, *landscape, *portrait), picture.bytes,
                                                deps.attachment_dek, deps.max_receive_bytes);
          !copied) {
        return copied.error();
      }
    }
    if (auto put = deps.threads.UpsertThread(thread); !put) {
      return put.error();
    }
  }

  {
    const std::vector<Line>& lines = AiLines();
    Thread thread = ThreadFor(kAiThreadId, ThreadKind::Ai, "京都三日行程", {}, lines, 0, deps);
    if (auto put = deps.threads.UpsertThread(thread); !put) {
      return put.error();
    }
    // Their lines are the assistant's.
    if (auto put = PutMessages(deps, kAiThreadId, "demo-msg-ai", lines, {kAiAssistantContactId}, *landscape,
                               *portrait);
        !put) {
      return put.error();
    }
    if (auto put = deps.threads.UpsertThread(thread); !put) {
      return put.error();
    }
  }

  deps.threads.Flush();
  done_ = true;
  log().info << "Demo data written: " << kContacts.size() << " contacts, " << directs.size() << " direct chats, "
             << Groups().size() << " groups, 1 AI chat";
  return true;
}

Roe<DemoSeedDeps> MakeDemoSeedDeps(ConversationsHub& hub) {
  IGroupRosterStore* groups = hub.GroupRosterOrNull();
  std::shared_ptr<CasStore> content = hub.Content();
  if (groups == nullptr || !content) {
    return Error("demo data: the profile stores are not open");
  }
  auto identity = hub.Identity().Get();
  if (!identity) {
    return identity.error();
  }
  const int64_t now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  return DemoSeedDeps{hub.Store(),
                      hub.Contacts(),
                      *groups,
                      *content,
                      hub.ProfileDataDir(),
                      hub.Attachments().CopyDek(),
                      hub.CurrentAttachmentLimits().max_receive_bytes,
                      identity->relay_user_id,
                      LocalMidnightMs(now_ms),
                      now_ms,
                      ReadDemoAsset,
                      [&hub](const mesh::DirectoryHit& hit) { hub.DirectoryShadows().Put(hit); }};
}

std::string DemoAskAiAnswer(const std::string& question) {
  if (question.find("出错") != std::string::npos || util::ToLowerAscii(question).find("error") != std::string::npos) {
    return {};
  }
  const bool chinese = std::any_of(question.begin(), question.end(),
                                   [](const char c) { return static_cast<unsigned char>(c) >= 0xE0; });
  // A setting's "?" asks about that setting; anything else gets a general sample.
  const bool about_setting = question.find("「") != std::string::npos || question.find("“") != std::string::npos;
  if (!about_setting) {
    if (chinese) {
      return "这是演示版的示例回答（演示版不连 AI）。\n\n"
             "真正的 AI 会在这里一段一段地写出回答，你可以：\n\n"
             "- 接着追问，它记得这段对话；\n"
             "- 往上拖面板看更多，往下拖收起，回到原来的页面；\n"
             "- 点右上角「在 AI 聊天里打开」看完整对话。";
    }
    return "This is a sample answer from the demo app (it has no AI).\n\n"
           "A real answer is written here bit by bit. You can:\n\n"
           "- ask a follow-up; it remembers this conversation;\n"
           "- drag the panel up for more room, or down to go back to your page;\n"
           "- tap \"Open in AI chat\" for the whole conversation.";
  }
  if (chinese) {
    return "这是演示版的示例回答（演示版不连 AI）。\n\n"
           "真正的 AI 会在这里说清这个设置：\n\n"
           "- **一句话**：它是做什么的；\n"
           "- **开或关**：打开、关掉以后会怎样；\n"
           "- **什么时候改**：一般用默认就好，什么情况下需要动它。\n\n"
           "还没看懂，可以接着问，或者点下面的「问 PP 客服」。";
  }
  return "This is a sample answer from the demo app (it has no AI).\n\n"
         "A real answer explains this setting here:\n\n"
         "- **In short**: what it does;\n"
         "- **On or off**: what changes either way;\n"
         "- **When to change it**: the default suits most people, and when it does not.\n\n"
         "Still unsure? Ask a follow-up, or tap \"Ask PP Support\" below.";
}

std::vector<DemoHeadline> DemoBriefHeadlines(const bool chinese) {
  if (chinese) {
    return {{"本周油价小幅回落，部分地区每升降一毛", "https://www.example.com/news/oil", 60},
            {"北方多地迎来今冬首场降雪，出行注意安全", "https://news.example.org/weather/snow", 150},
            {"新款手机续航测试结果出炉：最长超过两天", "https://tech.example.net/phones/battery", 26 * 60},
            {"本地图书馆周末延长开放时间", "https://www.example.com/city/library", 30 * 60}};
  }
  return {{"Fuel prices ease slightly this week", "https://www.example.com/news/oil", 60},
          {"First snow of the winter reaches the north", "https://news.example.org/weather/snow", 150},
          {"New phones tested: the best battery lasts over two days", "https://tech.example.net/phones/battery", 26 * 60},
          {"City library opens longer at weekends", "https://www.example.com/city/library", 30 * 60}};
}

std::string DemoPeerReply(const std::string& sent) {
  const bool chinese =
      std::any_of(sent.begin(), sent.end(), [](const char c) { return static_cast<unsigned char>(c) >= 0xE0; });
  return chinese ? "收到！（演示版的自动回复）" : "Got it! (an automatic reply in the demo app)";
}

} // namespace pbr
