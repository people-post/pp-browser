#pragma once

#include "domain/messaging/CallTypes.h"

#include <cstdint>
#include <string>

namespace pbr {

/**
 * Shape CallSfuAttach for fan-out after the picker finished AcceptAndAttach.
 * Clears quote_id — hop quotes are single-use; peers must RequestQuote themselves.
 */
CallSfuAttachDetail BuildSfuAttachFanout(const CallSfuAttachDetail& after_local_attach);

/** FNV-1a style publisher stream id from communicating identity (stable across peers). */
uint32_t PublisherStreamIdForIdentity(const std::string& identity);

} // namespace pbr
