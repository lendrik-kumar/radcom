// UI Logic
const ICON_TICK_SINGLE = `<svg viewBox="0 0 24 24" width="16" height="16" fill="currentColor"><path d="M9 16.17L4.83 12l-1.42 1.41L9 19 21 7l-1.41-1.41z"/></svg>`;
const ICON_TICK_DOUBLE = `<svg viewBox="0 0 24 24" width="16" height="16" fill="currentColor"><path d="M18 7l-1.41-1.41-6.34 6.34 1.41 1.41L18 7zm4.24-1.41L11.66 16.17 7.48 12l-1.41 1.41L11.66 19l12-12-1.42-1.41zM.41 13.41L6 19l1.41-1.41L1.83 12 .41 13.41z"/></svg>`;
const ICON_WAITING = `<svg viewBox="0 0 24 24" width="16" height="16" fill="currentColor"><path d="M11.99 2C6.47 2 2 6.48 2 12s4.47 10 9.99 10C17.52 22 22 17.52 22 12S17.52 2 11.99 2zM12 20c-4.42 0-8-3.58-8-8s3.58-8 8-8 8 3.58 8 8-3.58 8-8 8zm.5-13H11v6l5.25 3.15.75-1.23-4.5-2.67z"/></svg>`;

const UI = {
  activeChatPhone: null,
  chatOffset: 0,
  chatHasMore: true,
  isLoadingMessages: false,

  init() {
    this.bindEvents();
    this.checkIdentity();
  },

  bindEvents() {
    // Navigation
    document.getElementById('btn-fab-add').addEventListener('click', () => this.showModal('modal-add-contact'));
    document.getElementById('btn-settings').addEventListener('click', () => {
      this.updateSettingsView();
      this.showModal('modal-settings');
    });
    document.getElementById('btn-back').addEventListener('click', () => this.closeChatView());
    
    // Modals close logic (clicking overlay)
    document.getElementById('modal-overlay').addEventListener('click', () => {
      // Don't close onboarding via overlay
      if (!document.getElementById('modal-onboarding').classList.contains('hidden')) return;
      this.hideAllModals();
    });
    
    document.getElementById('btn-close-settings').addEventListener('click', () => this.hideAllModals());
    document.getElementById('btn-cancel-contact').addEventListener('click', () => this.hideAllModals());
    document.getElementById('btn-logout').addEventListener('click', () => {
      localStorage.removeItem('radcom_identity');
      window.location.reload();
    });

    // Identity Save
    document.getElementById('btn-save-identity').addEventListener('click', () => {
      const phone = document.getElementById('input-own-phone').value.trim();
      const name = document.getElementById('input-own-name').value.trim();
      if (!phone) return alert("Phone number is required");
      
      window.db.setIdentity(phone, name);
      this.hideAllModals();
      window.wsService.connect();
    });

    // Add Contact / Chat
    document.getElementById('btn-save-contact').addEventListener('click', async () => {
      const phone = document.getElementById('input-contact-phone').value.trim();
      const name = document.getElementById('input-contact-name').value.trim();
      if (!phone) return;
      
      if (name) {
        await window.db.saveContact(phone, name);
      }
      this.hideAllModals();
      this.openChatView(phone, name || phone);
      // Reset inputs
      document.getElementById('input-contact-phone').value = '';
      document.getElementById('input-contact-name').value = '';
    });

    // Chat Sending
    const chatInput = document.getElementById('chat-input');
    const sendBtn = document.getElementById('btn-send');
    
    const sendMsg = () => {
      const text = chatInput.value.trim();
      if (!text || !this.activeChatPhone) return;
      
      window.wsService.sendMessage(this.activeChatPhone, text);
      chatInput.value = '';
    };

    sendBtn.addEventListener('click', sendMsg);
    chatInput.addEventListener('keypress', (e) => {
      if (e.key === 'Enter') sendMsg();
    });

    // Pagination Scroll
    document.getElementById('chat-messages').addEventListener('scroll', (e) => {
      if (e.target.scrollTop < 50) {
        this.renderMessages(this.activeChatPhone, false);
      }
    });
  },

  checkIdentity() {
    const id = window.db.getIdentity();
    if (!id) {
      this.showModal('modal-onboarding');
    } else {
      window.wsService.connect();
      this.renderChatList();
    }
  },

  showModal(id) {
    this.hideAllModals();
    document.getElementById('modal-overlay').classList.remove('hidden');
    document.getElementById(id).classList.remove('hidden');
  },

  hideAllModals() {
    document.getElementById('modal-overlay').classList.add('hidden');
    document.querySelectorAll('.modal').forEach(m => m.classList.add('hidden'));
  },

  updateSettingsView() {
    const id = window.db.getIdentity();
    if (id) {
      document.getElementById('settings-phone').textContent = `${id.name ? id.name + ' ' : ''}(${id.phone})`;
    }
    
    const el = document.getElementById('settings-conn-text');
    if (window.wsService.isConnected) {
      el.textContent = "Connected (Node Only)"; // Or check actual status
    } else {
      el.textContent = "Disconnected";
    }
  },

  updateConnectionStatus(status) {
    const badge = document.getElementById('conn-status');
    badge.className = 'status-badge'; // reset
    if (status === 'disconnected') badge.classList.add('state-disconnected');
    else if (status === 'node_only') badge.classList.add('state-node-only');
    else if (status === 'mesh_live') badge.classList.add('state-mesh-live');
    
    const settingsText = document.getElementById('settings-conn-text');
    settingsText.textContent = status.replace('_', ' ').toUpperCase();
  },

  // --- Views Rendering ---
  
  async renderChatList() {
    const listEl = document.getElementById('chats-list');
    const chats = await window.db.getChats();
    
    if (chats.length === 0) {
      listEl.innerHTML = `<div style="padding: 24px; text-align: center; color: var(--on-surface-var);">No chats yet. Tap the button below to start one.</div>`;
      return;
    }
    
    listEl.innerHTML = '';
    
    for (const chat of chats) {
      // Try to get contact name
      const contact = await window.db.getContact(chat.phone);
      const displayName = contact ? contact.display_name : chat.phone;
      
      const item = document.createElement('div');
      item.className = 'chat-item';
      item.onclick = () => this.openChatView(chat.phone, displayName);
      
      const initial = displayName.charAt(0).toUpperCase();
      const timeStr = new Date(chat.last_message_at).toLocaleTimeString([], {hour: '2-digit', minute:'2-digit'});
      
      item.innerHTML = `
        <div class="chat-avatar">${initial}</div>
        <div class="chat-details">
          <div class="chat-title-row">
            <div class="chat-name">${displayName}</div>
            <div class="chat-time">${timeStr}</div>
          </div>
          <div class="chat-msg-row">
            <div class="chat-preview">${chat.last_msg_text || ''}</div>
            ${chat.unread_count > 0 ? `<div class="unread-badge">${chat.unread_count}</div>` : ''}
          </div>
        </div>
      `;
      listEl.appendChild(item);
    }
  },

  async openChatView(phone, name) {
    this.activeChatPhone = phone;
    this.chatOffset = 0;
    this.chatHasMore = true;
    document.getElementById('chat-title').textContent = name;
    
    await window.db.clearChatUnread(phone);
    await this.renderMessages(phone, true);
    
    document.getElementById('view-chat').classList.add('active');
    this.renderChatList(); // Update list to clear unread
  },

  closeChatView() {
    document.getElementById('view-chat').classList.remove('active');
    this.activeChatPhone = null;
    this.renderChatList();
  },

  async renderMessages(phone, initial = false) {
    if (phone !== this.activeChatPhone || this.isLoadingMessages || !this.chatHasMore) return;
    this.isLoadingMessages = true;
    
    const container = document.getElementById('chat-messages');
    if (initial) container.innerHTML = '';
    
    const limit = 50;
    const msgs = await window.db.getMessages(phone, limit, this.chatOffset);
    
    if (msgs.length < limit) {
      this.chatHasMore = false;
    }
    
    const oldScrollHeight = container.scrollHeight;
    
    if (initial) {
      msgs.forEach(msg => this.appendMessageDOM(msg, container));
      this.scrollToBottom();
    } else {
      const fragment = document.createDocumentFragment();
      msgs.forEach(msg => this.appendMessageDOM(msg, fragment));
      container.insertBefore(fragment, container.firstChild);
      container.scrollTop = container.scrollHeight - oldScrollHeight;
    }
    
    this.chatOffset += msgs.length;
    this.isLoadingMessages = false;
  },

  appendMessage(msg) {
    if (msg.chat_phone === this.activeChatPhone) {
      const container = document.getElementById('chat-messages');
      this.appendMessageDOM(msg, container);
      this.scrollToBottom();
      this.chatOffset++;
      window.db.clearChatUnread(msg.chat_phone);
    } else {
      // Just re-render chat list to show unread bump
      this.renderChatList();
    }
  },

  appendMessageDOM(msg, container) {
    const div = document.createElement('div');
    div.className = `msg-bubble ${msg.direction === 'out' ? 'msg-out' : 'msg-in'}`;
    div.id = `msg-${msg.id}`;
    
    const timeStr = new Date(msg.created_at).toLocaleTimeString([], {hour: '2-digit', minute:'2-digit'});
    
    let tickHtml = '';
    if (msg.direction === 'out') {
      let icon = ICON_WAITING;
      if (msg.status === 'delivered') icon = ICON_TICK_DOUBLE;
      else if (msg.status === 'sent') icon = ICON_TICK_SINGLE;
      
      const tickClass = msg.status === 'delivered' ? 'tick delivered' : 'tick';
      tickHtml = `<span class="${tickClass}" id="tick-${msg.id}">${icon}</span>`;
    }
    
    div.innerHTML = `
      <div>${this.escapeHTML(msg.text)}</div>
      <div class="msg-meta">
        <span>${timeStr}</span>
        ${tickHtml}
      </div>
    `;
    container.appendChild(div);
  },

  updateMessageTick(msgId, status) {
    const el = document.getElementById(`tick-${msgId}`);
    if (el) {
      if (status === 'delivered') {
        el.className = 'tick delivered';
        el.innerHTML = ICON_TICK_DOUBLE;
      } else if (status === 'sent') {
        el.className = 'tick';
        el.innerHTML = ICON_TICK_SINGLE;
      }
    }
  },

  scrollToBottom() {
    const c = document.getElementById('chat-messages');
    c.scrollTop = c.scrollHeight;
  },

  escapeHTML(str) {
    const p = document.createElement('p');
    p.appendChild(document.createTextNode(str));
    return p.innerHTML;
  }
};

window.ui = UI;
