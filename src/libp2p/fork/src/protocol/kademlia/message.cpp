/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/protocol/kademlia/message.hpp>

#include <functional>

#include <libp2p/multi/uvarint.hpp>
#include <libp2p/wire/kademlia_wire.hpp>

OUTCOME_CPP_DEFINE_CATEGORY(libp2p::protocol::kademlia, Message::Error, e) {
  using E = libp2p::protocol::kademlia::Message::Error;
  switch (e) {
    case E::INVALID_CONNECTEDNESS:
      return "invalid connectedness";
    case E::INVALID_PEER_ID:
      return "invalid peer id";
    case E::INVALID_ADDRESSES:
      return "invalid peer addresses";
  }
  return "unknown error (libp2p::protocol::kademlia::Message::Error)";
}
namespace libp2p::protocol::kademlia {

  using ConnStatus = Host::Connectedness;

  namespace {

    outcome::result<Message::Peer> assign_peer(const wire::KademliaPeerWire &src) {
      if (static_cast<ConnStatus>(src.connection) > ConnStatus::CAN_NOT_CONNECT) {
        return Message::Error::INVALID_CONNECTEDNESS;
      }

      auto peer_id_res = PeerId::fromBytes(src.id);
      if (!peer_id_res) {
        return Message::Error::INVALID_PEER_ID;
      }

      std::vector<multi::Multiaddress> addresses;
      for (const auto &addr : src.addrs) {
        auto res = multi::Multiaddress::create(addr);
        if (!res) {
          return Message::Error::INVALID_ADDRESSES;
        }
        addresses.push_back(res.value());
      }

      return Message::Peer{PeerInfo{peer_id_res.value(), std::move(addresses)},
                           ConnStatus(static_cast<int>(src.connection))};
    }

    outcome::result<void> assign_peers(boost::optional<Message::Peers> &dst,
                                       const std::vector<wire::KademliaPeerWire> &src) {
      if (!src.empty()) {
        dst = Message::Peers{};
        Message::Peers &v = dst.value();
        v.reserve(src.size());
        for (const auto &p : src) {
          auto res = assign_peer(p);
          if (!res) {
            return res.as_failure();
          }
          v.push_back(std::move(res.value()));
        }
      }
      return outcome::success();
    }

    void assign_record(Message::Record &dst, const wire::KademliaRecordWire &src) {
      dst.key = src.key;
      dst.time_received = src.time_received;
      dst.value = src.value;
    }

    wire::KademliaPeerWire to_wire_peer(const Message::Peer &p) {
      wire::KademliaPeerWire out;
      const auto pid_v = p.info.id.toVector();
      out.id.assign(pid_v.begin(), pid_v.end());
      for (const auto &addr : p.info.addresses) {
        auto &bytes = addr.getBytesAddress();
        out.addrs.emplace_back(bytes.begin(), bytes.end());
      }
      out.connection =
          static_cast<wire::KademliaConnectionTypeWire>(p.conn_status);
      return out;
    }

  }  // namespace

  void Message::clear() {
    type = Type::kPing;
    key.clear();
    record.reset();
    closer_peers.reset();
    provider_peers.reset();
    error_message_.clear();
  }

  bool Message::deserialize(BytesIn pb) {
    clear();
    auto wire_msg_res = wire::KademliaMessageWire::decode(pb);
    if (!wire_msg_res) {
      error_message_ = "Invalid wire data";
      return false;
    }
    auto &&wire_msg = wire_msg_res.value();
    type = static_cast<Type>(wire_msg.type);
    if (type > Type::kPing) {
      error_message_ = "Bad message type";
      return false;
    }
    key = wire_msg.key;
    if (wire_msg.record) {
      record.emplace();
      assign_record(record.value(), *wire_msg.record);
    }
    auto closer_res = assign_peers(closer_peers, wire_msg.closer_peers);
    if (not closer_res) {
      error_message_ = fmt::format("Bad closer peers: {}", closer_res.error());
      return false;
    }
    auto provider_res = assign_peers(provider_peers, wire_msg.provider_peers);
    if (not provider_res) {
      error_message_ =
          fmt::format("Bad provider peers: {}", provider_res.error());
      return false;
    }
    return true;
  }

  bool Message::serialize(std::vector<uint8_t> &buffer) const {
    wire::KademliaMessageWire wire_msg;
    wire_msg.type = static_cast<wire::KademliaMessageTypeWire>(type);
    wire_msg.key = key;
    if (record) {
      const Record &rec_src = record.value();
      wire_msg.record = wire::KademliaRecordWire{
          rec_src.key, rec_src.value, rec_src.time_received};
    }
    if (closer_peers) {
      for (const auto &p : closer_peers.value()) {
        wire_msg.closer_peers.push_back(to_wire_peer(p));
      }
    }
    if (provider_peers) {
      for (const auto &p : provider_peers.value()) {
        wire_msg.provider_peers.push_back(to_wire_peer(p));
      }
    }
    auto encoded_res = wire_msg.encode();
    if (!encoded_res) {
      return false;
    }
    const auto &encoded = encoded_res.value();
    size_t msg_sz = encoded.size();
    auto varint_len = multi::UVarint{msg_sz};
    auto varint_vec = varint_len.toVector();
    size_t prefix_sz = varint_vec.size();
    buffer.resize(prefix_sz + msg_sz);
    memcpy(buffer.data(), varint_vec.data(), prefix_sz);
    memcpy(buffer.data() + prefix_sz, encoded.data(), msg_sz);
    return true;
  }

  void Message::selfAnnounce(PeerInfo self) {
    closer_peers = Message::Peers{
        {Message::Peer{std::move(self), Message::Connectedness::CAN_CONNECT}}};
  }

  Message createPutValueRequest(const Key &key, const Value &value) {
    Message msg;
    msg.type = Message::Type::kPutValue;
    msg.record.emplace(Message::Record{key, value, "timestamp"});
    return msg;
  }

  Message createGetValueRequest(const Key &key,
                                boost::optional<PeerInfo> self_announce) {
    Message msg;
    msg.type = Message::Type::kGetValue;
    msg.key = key;
    if (self_announce) {
      msg.selfAnnounce(std::move(self_announce.value()));
    }
    return msg;
  }

  Message createAddProviderRequest(PeerInfo self, const Key &key) {
    Message msg;
    msg.type = Message::Type::kAddProvider;
    msg.key = key;
    msg.provider_peers = Message::Peers{
        {Message::Peer{std::move(self), Message::Connectedness::CAN_CONNECT}}};
    return msg;
  }

  Message createGetProvidersRequest(const Key &key,
                                    boost::optional<PeerInfo> self_announce) {
    Message msg;
    msg.type = Message::Type::kGetProviders;
    msg.key = key;
    if (self_announce) {
      msg.selfAnnounce(std::move(self_announce.value()));
    }
    return msg;
  }

  Message createFindNodeRequest(Key key,
                                boost::optional<PeerInfo> self_announce) {
    Message msg;
    msg.type = Message::Type::kFindNode;
    msg.key = std::move(key);
    if (self_announce) {
      msg.selfAnnounce(std::move(self_announce.value()));
    }
    return msg;
  }

}  // namespace libp2p::protocol::kademlia
