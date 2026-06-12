#include "ui/PanelRegistry.h"

namespace pbr {

std::string PanelRegistry::Body(PanelKind kind) {
  switch (kind) {
  case PanelKind::Sidebar:
    return R"frag(
      <div class="sidebar-panel stack" data-model="shell">
        <button class="sidebar-new-chat" data-event-click="new_chat()">New chat</button>
        <div class="sidebar-sessions">
          <div data-for="session : sessions" class="sidebar-session">
            <p data-rml="session.title"></p>
            <p class="muted sidebar-session-preview" data-rml="session.preview"></p>
          </div>
        </div>
      </div>
    )frag";
  case PanelKind::Chat:
    return R"frag(
      <div class="chat-panel stack" data-model="chat">
        <div class="chat-header">
          <h1>pp-browser</h1>
          <p class="muted">Ask anything. Assistant replies use structured text.</p>
        </div>
        <div class="messages" data-mount-id="messages">
          <div data-for="turn : turns" class="message-group">
            <div class="message-row message-row-user" data-rml="turn.user_content_rml"></div>
            <div class="message-row message-row-assistant" data-if="turn.has_assistant">
              <div class="assistant-message">
                <div class="bubble bubble-assistant" selectable="text" data-rml="turn.assistant_content_rml"></div>
                <div class="suggestion-row">
                  <button class="chat-suggestion" data-for="sug : turn.suggestions" data-attr-message="sug.message" data-event-click="send_suggestion(sug.message)">{{sug.label}}</button>
                </div>
              </div>
            </div>
          </div>
          <p class="muted thinking" data-if="loading">Thinking...</p>
        </div>
        <div class="input-row">
          <textarea id="draft-input" placeholder="Type a message..." rows="2" data-value="draft"></textarea>
          <button data-event-click="send_message()">Send</button>
        </div>
      </div>
    )frag";
  case PanelKind::Preview:
    return R"frag(
      <div class="preview-panel stack" data-model="shell">
        <h2>Preview</h2>
        <p class="muted">Latest assistant reply</p>
        <div class="preview-content" data-if="preview_rml != ''" data-rml="preview_rml"></div>
        <p class="muted" data-if="preview_rml == ''">No assistant reply yet.</p>
      </div>
    )frag";
  case PanelKind::Empty:
    return R"frag(
      <div class="empty-panel stack">
        <p class="muted">Empty panel</p>
        <p class="muted">Use split or close controls in the panel header.</p>
      </div>
    )frag";
  }
  return {};
}

} // namespace pbr
