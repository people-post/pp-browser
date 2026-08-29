/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <libp2p/protocol/kademlia/impl/storage.hpp>

#include <unordered_map>

#include <libp2p/basic/scheduler.hpp>
#include <libp2p/protocol/kademlia/config.hpp>
#include <libp2p/protocol/kademlia/storage_backend.hpp>

namespace libp2p::protocol::kademlia {

  class StorageImpl : public Storage,
                      public std::enable_shared_from_this<StorageImpl> {
    struct Record {
      ContentId key;
      Time expire_time{};
      Time refresh_time{};
      Time updated_at{};
    };

   public:
    StorageImpl(const Config &config,
                std::shared_ptr<StorageBackend> backend,
                std::shared_ptr<basic::Scheduler> scheduler);

    ~StorageImpl() override;

    outcome::result<void> putValue(Key key, Value value) override;

    outcome::result<std::pair<Value, Time>> getValue(
        const Key &key) const override;

    bool hasValue(const Key &key) const override;

    std::vector<std::pair<Key, Value>> getAllRecords() const override;

   private:
    void onRefreshTimer();
    void setTimerRefresh();

    const Config &config_;
    std::shared_ptr<StorageBackend> backend_;
    std::shared_ptr<basic::Scheduler> scheduler_;

    std::unordered_map<ContentId, Record> table_;
    basic::Scheduler::Handle refresh_timer_;
  };

}  // namespace libp2p::protocol::kademlia
