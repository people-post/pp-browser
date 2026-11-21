#include "feature/presenters/VoiceSendQueue.h"

#include <utility>

namespace pbr {

VoiceSendQueue::VoiceSendQueue(VoiceSendPorts ports) : ports_(std::move(ports)) {}

void VoiceSendQueue::Enqueue(VoiceSendJob job) {
  waiting_.push_back({std::move(job), Now()});
  Pump();
  Changed();
}

void VoiceSendQueue::Pump() {
  if (pumping_) {
    return; // a `start` that ended at once: the loop below goes on with the next job
  }
  pumping_ = true;
  while (!in_flight_ && !waiting_.empty() && ports_.slot_free && ports_.slot_free()) {
    Waiting next = std::move(waiting_.front());
    waiting_.pop_front();
    const int64_t now = Now();
    in_flight_ = Running{next.job, now - next.enqueued_ms, now};
    if (ports_.start) {
      ports_.start(next.job);
    } else {
      in_flight_.reset();
    }
  }
  pumping_ = false;
}

void VoiceSendQueue::OnFinished(const std::string& row_id, const AttachmentSendOutcome outcome) {
  if (!in_flight_ || in_flight_->job.row_id != row_id) {
    return;
  }
  const Running done = std::move(*in_flight_);
  in_flight_.reset();
  if (ports_.report) {
    ports_.report(done.job, outcome, VoiceSendTiming{done.wait_ms, Now() - done.started_ms});
  }
  Pump(); // whatever the outcome: a failed one never holds the line
  Changed();
}

std::optional<VoiceSendJob> VoiceSendQueue::CancelWaiting(const std::string& row_id) {
  for (auto it = waiting_.begin(); it != waiting_.end(); ++it) {
    if (it->job.row_id == row_id) {
      VoiceSendJob job = std::move(it->job);
      waiting_.erase(it);
      Discard(job);
      Changed();
      return job;
    }
  }
  return std::nullopt;
}

std::vector<VoiceSendJob> VoiceSendQueue::DropWaiting(const std::optional<std::string>& thread_id) {
  std::vector<VoiceSendJob> dropped;
  for (auto it = waiting_.begin(); it != waiting_.end();) {
    if (thread_id && it->job.thread_id != *thread_id) {
      ++it;
      continue;
    }
    Discard(it->job);
    dropped.push_back(std::move(it->job));
    it = waiting_.erase(it);
  }
  if (!dropped.empty()) {
    Changed();
  }
  return dropped;
}

bool VoiceSendQueue::IsWaiting(const std::string& row_id) const {
  for (const Waiting& item : waiting_) {
    if (item.job.row_id == row_id) {
      return true;
    }
  }
  return false;
}

std::vector<std::string> VoiceSendQueue::FilesInUse() const {
  std::vector<std::string> files;
  auto add = [&files](const VoiceSendJob& job) {
    if (job.kind == VoiceJobKind::Fresh && !job.path.empty()) {
      files.push_back(job.path);
    }
  };
  if (in_flight_) {
    add(in_flight_->job);
  }
  for (const Waiting& item : waiting_) {
    add(item.job);
  }
  return files;
}

size_t VoiceSendQueue::CountForThread(const std::string& thread_id) const {
  size_t count = in_flight_ && in_flight_->job.thread_id == thread_id ? 1 : 0;
  for (const Waiting& item : waiting_) {
    if (item.job.thread_id == thread_id) {
      ++count;
    }
  }
  return count;
}

void VoiceSendQueue::Changed() const {
  if (ports_.changed) {
    ports_.changed();
  }
}

void VoiceSendQueue::Discard(const VoiceSendJob& job) {
  if (job.kind == VoiceJobKind::Fresh && !job.path.empty() && ports_.remove_file) {
    ports_.remove_file(job.path);
  }
}

} // namespace pbr
