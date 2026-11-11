#include "feature/presenters/MessageResend.h"

#include "feature/presenters/ChatPresenter.h"
#include "feature/presenters/SendMark.h"

#include <utility>

namespace pbr {

SendMarkTap DecideSendMarkTap(const SendMarkTapFacts& facts) {
  SendMarkTap tap;
  if (!facts.row || !facts.thread || !MayShowSendMark(*facts.row)) {
    return tap;
  }
  const ThreadMessage& row = *facts.row;
  if (facts.started_ms) {
    tap.kind = SendMarkTap::Kind::StillTrying; // never a second send beside one still alive
    return tap;
  }
  if (MarkOf(row, std::nullopt, facts.now_ms, facts.no_recipients) != SendMark::Failed) {
    return tap;
  }
  const Thread& thread = *facts.thread;
  if (thread.kind != ThreadKind::Direct && thread.kind != ThreadKind::Group) {
    return tap;
  }
  if (row.content_type != ChatContentType::Text) {
    tap.kind = SendMarkTap::Kind::ReasonOnly;
    tap.refusal = SendMarkTap::Refusal::NotText;
    return tap;
  }
  // A private chat's numbering cannot skip the removed row; an "@ai++" question sent again as plain text would no
  // longer ask the AI.
  if ((thread.kind == ThreadKind::Direct && thread.channel == ThreadChannel::E2e) || row.ai_invoke_mode) {
    tap.kind = SendMarkTap::Kind::ReasonOnly;
    tap.refusal = SendMarkTap::Refusal::ThisKind;
    return tap;
  }
  tap.kind = SendMarkTap::Kind::Resent;
  return tap;
}

SendMarkTap ResendAfterTap(const SendMarkTapFacts& facts, const ResendPorts& ports) {
  SendMarkTap tap = DecideSendMarkTap(facts);
  if (tap.kind != SendMarkTap::Kind::Resent) {
    return tap;
  }
  if (facts.already_resent) {
    // Its new copy went out on an earlier tap: only the removal is tried again.
    auto removed = ports.remove_unsent ? ports.remove_unsent(facts.row->id) : Roe<bool>(false);
    tap.old_removed = removed && *removed;
    tap.removal_error = !removed;
    if (!tap.old_removed) {
      tap.kind = SendMarkTap::Kind::RemovalFailed;
    }
    return tap;
  }
  const ThreadMessage& old = *facts.row;
  Roe<ThreadMessage> sent = Error("Messaging not ready");
  if (facts.thread->kind == ThreadKind::Group) {
    std::vector<TextSpan> spans = text_spans::FromPayloadJson(old.payload_json);
    if (!spans.empty() && ports.spans_on_roster) {
      spans = ports.spans_on_roster(std::move(spans));
    }
    if (!ChatPresenter::KeepsMentionSpans(old.text, spans)) {
      spans.clear();
    }
    if (ports.send_group) {
      sent = ports.send_group(old.text, std::move(spans));
    }
  } else if (ports.send_direct) {
    sent = ports.send_direct(old.text);
  }
  if (!sent) {
    tap.kind = SendMarkTap::Kind::SendFailed;
    tap.error = sent.error();
    return tap;
  }
  tap.new_id = sent->id;
  // The new message exists: the unsent one goes. Not removed (delivered in between, or the store refused): it stays.
  if (ports.remove_unsent && sent->id != old.id) {
    auto removed = ports.remove_unsent(old.id);
    tap.old_removed = removed && *removed;
    tap.removal_error = !removed;
  }
  return tap;
}

} // namespace pbr
