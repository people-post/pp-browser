#include "domain/attachments/OutgoingAttachmentStore.h"

#include "foundation/crypto/FileCipher.h"
#include "lib/messaging/ChatPayloadCodec.h"
#include "common/Utilities.h"
#include "common/ValueJson.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

constexpr const char* kAadPurpose = "outgoing-attachment";

bool SafeId(const std::string& id) {
  return !id.empty() && id.size() <= 64 &&
         std::all_of(id.begin(), id.end(), [](const char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-'; });
}

Roe<void> WriteBytes(const std::string& path, const uint8_t* data, const size_t size) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return Error("Could not write the outgoing attachment");
  }
  if (size > 0) {
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
  }
  out.close();
  if (!out) {
    return Error("Could not write the outgoing attachment");
  }
  return {};
}

} // namespace

OutgoingAttachmentStore::OutgoingAttachmentStore(std::string profile_dir, std::string profile_id)
    : profile_dir_(std::move(profile_dir)), profile_id_(std::move(profile_id)) {}

std::string OutgoingAttachmentStore::Dir() const {
  return (std::filesystem::path(profile_dir_) / "blob_cipher" / "outgoing").string();
}

std::string OutgoingAttachmentStore::CipherPath(const std::string& id) const {
  return (std::filesystem::path(Dir()) / id).string();
}

std::string OutgoingAttachmentStore::RecordPath(const std::string& id) const { return CipherPath(id) + ".rec"; }

std::string OutgoingAttachmentStore::NewId() { return util::GenerateUuid(); }

Roe<void> OutgoingAttachmentStore::Update(const OutgoingAttachmentRecord& record, const ByteVector& dek) {
  if (!SafeId(record.id)) {
    return Error("Invalid outgoing attachment id");
  }
  Object json;
  json.set("id", record.id);
  json.set("thread_id", record.thread_id);
  json.set("fields", ChatPayloadCodec::AttachmentFieldsToJson(record.fields));
  json.setJsonUInt("cipher_bytes", record.cipher_bytes);
  json.setJsonUInt("delivered", record.delivered ? 1 : 0);
  json.set("created_ms", record.created_ms);
  const std::string text = DumpJson(json);
  auto sealed = FileCipher::Encrypt(dek, ByteVector(text.begin(), text.end()),
                                    FileCipher::BuildAad(kAadPurpose, profile_id_));
  if (!sealed) {
    return sealed.error();
  }
  return WriteBytes(RecordPath(record.id), sealed->data(), sealed->size());
}

Roe<void> OutgoingAttachmentStore::Begin(const OutgoingAttachmentRecord& record,
                                         const std::vector<uint8_t>& ciphertext, const ByteVector& dek) {
  if (!SafeId(record.id)) {
    return Error("Invalid outgoing attachment id");
  }
  std::error_code ec;
  std::filesystem::create_directories(Dir(), ec);
  if (ec) {
    return Error("Could not create the outgoing attachment folder");
  }
  if (auto written = WriteBytes(CipherPath(record.id), ciphertext.data(), ciphertext.size()); !written) {
    Remove(record.id);
    return written;
  }
  OutgoingAttachmentRecord stored = record;
  stored.cipher_bytes = ciphertext.size();
  if (auto updated = Update(stored, dek); !updated) {
    Remove(record.id);
    return updated;
  }
  return {};
}

Roe<OutgoingAttachmentRecord> OutgoingAttachmentStore::Load(const std::string& id, const ByteVector& dek) const {
  if (!SafeId(id)) {
    return Error("Invalid outgoing attachment id");
  }
  std::ifstream in(RecordPath(id), std::ios::binary);
  if (!in) {
    return Error(kLoadGone, "Outgoing attachment record is gone");
  }
  const ByteVector sealed((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto opened = FileCipher::Decrypt(dek, sealed, FileCipher::BuildAad(kAadPurpose, profile_id_));
  if (!opened) {
    return Error(kLoadDamaged, "Outgoing attachment record cannot be opened");
  }
  auto json = TryParseObject(std::string(opened->begin(), opened->end()));
  if (!json) {
    return Error(kLoadDamaged, "Outgoing attachment record is damaged");
  }
  OutgoingAttachmentRecord record;
  record.id = json->getString("id").value_or("");
  record.thread_id = json->getString("thread_id").value_or("");
  record.cipher_bytes = json->getNonNegInt("cipher_bytes").value_or(0);
  record.delivered = json->getNonNegInt("delivered").value_or(0) != 0;
  record.created_ms = static_cast<int64_t>(json->getNonNegInt("created_ms").value_or(0));
  auto fields = ChatPayloadCodec::DecodeAttachmentJson(json->getString("fields").value_or(""));
  if (record.id != id || !fields) {
    return Error(kLoadDamaged, "Outgoing attachment record is damaged");
  }
  record.fields = std::move(*fields);
  return record;
}

bool OutgoingAttachmentStore::HasCipher(const std::string& id) const {
  std::error_code ec;
  return SafeId(id) && std::filesystem::is_regular_file(CipherPath(id), ec);
}

Roe<std::vector<uint8_t>> OutgoingAttachmentStore::ReadCipher(const std::string& id) const {
  if (!HasCipher(id)) {
    return Error("The encrypted copy of the attachment is gone");
  }
  std::ifstream in(CipherPath(id), std::ios::binary);
  std::error_code ec;
  const auto size = std::filesystem::file_size(CipherPath(id), ec);
  std::vector<uint8_t> bytes(ec ? 0 : static_cast<size_t>(size));
  in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  bytes.resize(static_cast<size_t>(in.gcount()));
  if (bytes.empty()) {
    return Error("The encrypted copy of the attachment is gone");
  }
  return bytes;
}

std::vector<OutgoingAttachmentRecord> OutgoingAttachmentStore::ListForThread(const std::string& thread_id,
                                                                              const ByteVector& dek) const {
  std::vector<OutgoingAttachmentRecord> found;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(Dir(), ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name.size() < 5 || name.substr(name.size() - 4) != ".rec") {
      continue;
    }
    const std::string id = name.substr(0, name.size() - 4);
    if (!HasCipher(id)) {
      continue;
    }
    if (auto record = Load(id, dek); record && record->thread_id == thread_id) {
      found.push_back(std::move(*record));
    }
  }
  std::sort(found.begin(), found.end(),
            [](const OutgoingAttachmentRecord& a, const OutgoingAttachmentRecord& b) { return a.created_ms < b.created_ms; });
  return found;
}

void OutgoingAttachmentStore::Remove(const std::string& id) const {
  if (!SafeId(id)) {
    return;
  }
  std::error_code ec;
  std::filesystem::remove(CipherPath(id), ec);
  std::filesystem::remove(RecordPath(id), ec);
}

size_t OutgoingAttachmentStore::CleanupStale(const int64_t max_age_ms) const {
  std::set<std::string> gone;
  std::error_code ec;
  const auto now = std::filesystem::file_time_type::clock::now();
  for (std::filesystem::directory_iterator it(Dir(), ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code time_ec;
    const auto written = std::filesystem::last_write_time(it->path(), time_ec);
    if (time_ec) {
      continue;
    }
    const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - written).count();
    if (age_ms < max_age_ms) {
      continue;
    }
    std::string name = it->path().filename().string();
    if (name.size() > 4 && name.substr(name.size() - 4) == ".rec") {
      name.resize(name.size() - 4);
    }
    gone.insert(name);
  }
  for (const std::string& id : gone) {
    Remove(id);
  }
  return gone.size();
}

} // namespace pbr
