class RadcomWS {
  constructor() {
    this.ws = null;
    this.isConnected = false;
    this.reconnectAttempts = 0;
    this.maxReconnectAttempts = 5;
    this.reconnectTimeout = null;
    
    // Status callbacks
    this.onStatusChange = (statusStr) => {}; // 'disconnected', 'node_only', 'mesh_live'
    this.onMessageReceived = (msg) => {};
    this.onMessageStatus = (msgId, status) => {};
  }

  connect() {
    const identity = window.db.getIdentity();
    if (!identity) return; // Can't connect without identity

    if (this.ws && (this.ws.readyState === WebSocket.OPEN || this.ws.readyState === WebSocket.CONNECTING)) {
      return;
    }

    // Determine WS URL. If hosted on node, it's window.location.hostname
    // Fallback to 192.168.4.1 if testing locally without server
    let host = window.location.host || '192.168.4.1';
    if (host.startsWith('localhost') || host.startsWith('127.0.0.1')) {
      host = '192.168.4.1'; // Fallback for local dev
    }
    const wsUrl = `ws://${host}/ws`;

    try {
      this.ws = new WebSocket(wsUrl);
    } catch (e) {
      console.error("WS init failed:", e);
      this.scheduleReconnect();
      return;
    }

    this.ws.onopen = () => {
      console.log("WS Connected");
      this.isConnected = true;
      this.reconnectAttempts = 0;
      this.onStatusChange('node_only'); // Assume node only until told otherwise
      
      // Attach identity
      this.sendRaw({
        type: 'attach',
        phone: identity.phone
      });
      
      // Flush pending queue
      this.flushPending();
    };

    this.ws.onmessage = (event) => {
      try {
        const data = JSON.parse(event.data);
        this.handleFrame(data);
      } catch (e) {
        console.error("Failed to parse WS msg", event.data);
      }
    };

    this.ws.onclose = () => {
      console.log("WS Disconnected");
      this.isConnected = false;
      this.onStatusChange('disconnected');
      this.scheduleReconnect();
    };

    this.ws.onerror = (err) => {
      console.error("WS Error", err);
      // onclose will fire after this
    };
  }

  handleFrame(frame) {
    if (frame.type === 'status') {
      // {"type":"status","online":true/false}
      if (frame.online) {
        this.onStatusChange('mesh_live');
      } else {
        this.onStatusChange('node_only');
      }
    } 
    else if (frame.type === 'msg') {
      // {"type":"msg","from":"91...","text":"..."}
      // Or possibly a delivery ack {"type":"status", "msg_id": "...", "status": "delivered"} 
      // based on firmware spec. For now, assuming basic msg.
      
      if (frame.from) {
        const identity = window.db.getIdentity();
        if (identity && frame.from === identity.phone) {
          console.warn("Ignoring loopback message from own identity");
          return;
        }

        // Incoming message
        const msg = {
          id: window.db.generateId(), // If firm doesn't send msg_id, generate one
          chat_phone: frame.from,
          direction: 'in',
          text: frame.text,
          status: 'delivered',
          created_at: Date.now()
        };
        
        window.db.saveMessage(msg).then(() => {
          this.onMessageReceived(msg);
        });
      }
    }
    else if (frame.type === 'ack') {
      // If firmware sends acks: {"type":"ack","msg_id":"...","status":"sent|delivered"}
      window.db.updateMessageStatus(frame.msg_id, frame.status).then(() => {
        this.onMessageStatus(frame.msg_id, frame.status);
      });
    }
  }

  sendRaw(obj) {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify(obj));
      return true;
    }
    return false;
  }

  sendMessage(toPhone, text) {
    const msg = {
      id: window.db.generateId(),
      chat_phone: toPhone,
      direction: 'out',
      text: text,
      status: 'pending',
      created_at: Date.now()
    };
    
    // Save to DB immediately
    window.db.saveMessage(msg).then(() => {
      this.onMessageReceived(msg); // to render locally
      this.trySendPending(msg);
    });
  }
  
  async trySendPending(msg) {
    if (this.isConnected) {
      const success = this.sendRaw({
        type: 'msg',
        to: msg.chat_phone,
        text: msg.text,
        msg_id: msg.id // Send ID so firmware can ack it
      });
      
      if (success) {
        // Optimistically set to sent, or wait for ack if firmware supports it
        // For v1.5 we'll just set it to 'sent' if it went over the socket
        msg.status = 'sent';
        await window.db.updateMessageStatus(msg.id, 'sent');
        this.onMessageStatus(msg.id, 'sent');
      }
    }
  }

  async flushPending() {
    const pending = await window.db.getPendingMessages();
    for (let msg of pending) {
      this.trySendPending(msg);
    }
  }

  scheduleReconnect() {
    if (this.reconnectTimeout) clearTimeout(this.reconnectTimeout);
    
    // Don't aggressively reconnect if tab is hidden
    if (document.visibilityState === 'hidden') {
      return; 
    }

    if (this.reconnectAttempts < this.maxReconnectAttempts) {
      const delay = Math.pow(2, this.reconnectAttempts) * 1000;
      this.reconnectAttempts++;
      console.log(`Scheduling reconnect in ${delay}ms`);
      this.reconnectTimeout = setTimeout(() => this.connect(), delay);
    }
  }

  onVisibilityChange() {
    if (document.visibilityState === 'visible') {
      this.reconnectAttempts = 0;
      if (!this.isConnected) {
        this.connect();
      } else {
        this.flushPending(); // Just in case
      }
    }
  }
}

window.wsService = new RadcomWS();
