// IndexedDB Wrapper for Radcom Web
const DB_NAME = 'radcom_db';
const DB_VERSION = 2;

class RadcomDB {
  constructor() {
    this.db = null;
  }

  async init() {
    return new Promise((resolve, reject) => {
      const request = indexedDB.open(DB_NAME, DB_VERSION);

      request.onerror = (event) => {
        console.error("IndexedDB error:", event.target.error);
        reject(event.target.error);
      };

      request.onsuccess = (event) => {
        this.db = event.target.result;
        resolve();
      };

      request.onupgradeneeded = (event) => {
        const db = event.target.result;
        const tx = event.target.transaction;
        
        // Contacts store
        if (!db.objectStoreNames.contains('contacts')) {
          db.createObjectStore('contacts', { keyPath: 'phone' });
        }
        
        // Chats store
        if (!db.objectStoreNames.contains('chats')) {
          const chatStore = db.createObjectStore('chats', { keyPath: 'phone' });
          chatStore.createIndex('last_message_at', 'last_message_at', { unique: false });
        }
        
        // Messages store
        if (!db.objectStoreNames.contains('messages')) {
          const msgStore = db.createObjectStore('messages', { keyPath: 'id' });
          msgStore.createIndex('chat_phone', 'chat_phone', { unique: false });
          msgStore.createIndex('created_at', 'created_at', { unique: false });
          msgStore.createIndex('status', 'status', { unique: false }); // for finding 'pending'
          msgStore.createIndex('chat_phone_created_at', ['chat_phone', 'created_at'], { unique: false });
        } else if (event.oldVersion < 2) {
          // Upgrade existing DB to add the compound index
          const msgStore = tx.objectStore('messages');
          if (!msgStore.indexNames.contains('chat_phone_created_at')) {
            msgStore.createIndex('chat_phone_created_at', ['chat_phone', 'created_at'], { unique: false });
          }
        }
      };
    });
  }

  // Identity is kept in localStorage for simplicity
  getIdentity() {
    const id = localStorage.getItem('radcom_identity');
    return id ? JSON.parse(id) : null;
  }

  setIdentity(phone, name) {
    localStorage.setItem('radcom_identity', JSON.stringify({ phone, name }));
  }

  // Generic transaction wrapper
  _tx(storeName, mode, callback) {
    return new Promise((resolve, reject) => {
      let tx;
      try {
        tx = this.db.transaction(storeName, mode);
      } catch (e) {
        return reject(e);
      }
      const store = tx.objectStore(storeName);
      let result = null;
      
      const req = callback(store);
      if (req) {
        req.onsuccess = (e) => result = e.target.result;
        req.onerror = (e) => reject(e.target.error);
      }

      tx.oncomplete = () => resolve(result);
      tx.onerror = (e) => reject(e.target.error);
    });
  }

  // --- Contacts ---
  async getContact(phone) {
    return this._tx('contacts', 'readonly', s => s.get(phone));
  }

  async saveContact(phone, displayName) {
    const contact = { phone, display_name: displayName, added_at: Date.now() };
    await this._tx('contacts', 'readwrite', s => s.put(contact));
    return contact;
  }

  // --- Chats ---
  async getChats() {
    return new Promise((resolve, reject) => {
      const tx = this.db.transaction('chats', 'readonly');
      const store = tx.objectStore('chats');
      const index = store.index('last_message_at');
      const req = index.openCursor(null, 'prev'); // descending
      
      const chats = [];
      req.onsuccess = (e) => {
        const cursor = e.target.result;
        if (cursor) {
          chats.push(cursor.value);
          cursor.continue();
        } else {
          resolve(chats);
        }
      };
      req.onerror = (e) => reject(e.target.error);
    });
  }

  async updateChatLastMessage(phone, text, timestamp, unreadIncrement = 0) {
    const tx = this.db.transaction('chats', 'readwrite');
    const store = tx.objectStore('chats');
    
    return new Promise((resolve, reject) => {
      const getReq = store.get(phone);
      getReq.onsuccess = () => {
        let chat = getReq.result;
        if (!chat) {
          chat = { phone, last_message_at: timestamp, unread_count: 0, last_msg_text: text };
        } else {
          chat.last_message_at = Math.max(chat.last_message_at, timestamp);
          chat.last_msg_text = text;
        }
        chat.unread_count += unreadIncrement;
        store.put(chat);
        resolve(chat);
      };
      getReq.onerror = (e) => reject(e.target.error);
    });
  }

  async clearChatUnread(phone) {
    const tx = this.db.transaction('chats', 'readwrite');
    const store = tx.objectStore('chats');
    
    return new Promise((resolve, reject) => {
      const getReq = store.get(phone);
      getReq.onsuccess = () => {
        let chat = getReq.result;
        if (chat) {
          chat.unread_count = 0;
          store.put(chat);
        }
        resolve(chat);
      };
      getReq.onerror = (e) => reject(e.target.error);
    });
  }

  // --- Messages ---
  generateId() {
    return typeof crypto !== 'undefined' && crypto.randomUUID 
      ? crypto.randomUUID() 
      : 'msg_' + Date.now().toString(36) + Math.random().toString(36).substr(2, 5);
  }

  async saveMessage(msg) {
    // msg format: { id, chat_phone, direction, text, status, created_at }
    if (!msg.id) msg.id = this.generateId();
    if (!msg.created_at) msg.created_at = Date.now();
    await this._tx('messages', 'readwrite', s => s.put(msg));
    
    // Update chat automatically
    const unreadInc = msg.direction === 'in' ? 1 : 0;
    await this.updateChatLastMessage(msg.chat_phone, msg.text, msg.created_at, unreadInc);
    
    // Background quota cleanup
    this.cleanupQuota(msg.chat_phone).catch(e => console.error("Quota cleanup failed:", e));
    
    return msg;
  }

  async updateMessageStatus(id, status) {
    const tx = this.db.transaction('messages', 'readwrite');
    const store = tx.objectStore('messages');
    
    return new Promise((resolve, reject) => {
      const getReq = store.get(id);
      getReq.onsuccess = () => {
        let msg = getReq.result;
        if (msg) {
          msg.status = status;
          store.put(msg);
          resolve(msg);
        } else {
          resolve(null);
        }
      };
      getReq.onerror = (e) => reject(e.target.error);
    });
  }

  async getMessages(chatPhone, limit = 50, offset = 0) {
    return new Promise((resolve, reject) => {
      const tx = this.db.transaction('messages', 'readonly');
      const store = tx.objectStore('messages');
      
      // Fallback if index isn't ready for some reason
      if (!store.indexNames.contains('chat_phone_created_at')) {
        const index = store.index('chat_phone');
        const req = index.openCursor(IDBKeyRange.only(chatPhone));
        const msgs = [];
        req.onsuccess = (e) => {
          const cursor = e.target.result;
          if (cursor) { msgs.push(cursor.value); cursor.continue(); }
          else {
            msgs.sort((a, b) => a.created_at - b.created_at);
            resolve(msgs.slice(Math.max(0, msgs.length - limit - offset), msgs.length - offset));
          }
        };
        req.onerror = (e) => reject(e.target.error);
        return;
      }

      const index = store.index('chat_phone_created_at');
      const range = IDBKeyRange.bound([chatPhone, 0], [chatPhone, Date.now()]);
      const req = index.openCursor(range, 'prev'); // descending
      
      const msgs = [];
      let advanced = false;

      req.onsuccess = (e) => {
        const cursor = e.target.result;
        if (!cursor) {
          msgs.sort((a, b) => a.created_at - b.created_at);
          return resolve(msgs);
        }
        
        if (offset > 0 && !advanced) {
          advanced = true;
          cursor.advance(offset);
          return;
        }

        msgs.push(cursor.value);
        if (msgs.length < limit) {
          cursor.continue();
        } else {
          msgs.sort((a, b) => a.created_at - b.created_at);
          resolve(msgs);
        }
      };
      req.onerror = (e) => reject(e.target.error);
    });
  }

  async cleanupQuota(chatPhone, max = 1000) {
    return new Promise((resolve, reject) => {
      const tx = this.db.transaction('messages', 'readwrite');
      const store = tx.objectStore('messages');
      if (!store.indexNames.contains('chat_phone_created_at')) return resolve();
      
      const index = store.index('chat_phone_created_at');
      const range = IDBKeyRange.bound([chatPhone, 0], [chatPhone, Date.now()]);
      const req = index.openCursor(range, 'prev');
      
      let count = 0;
      req.onsuccess = (e) => {
        const cursor = e.target.result;
        if (!cursor) return resolve();
        
        count++;
        if (count > max) {
          store.delete(cursor.primaryKey);
        }
        cursor.continue();
      };
      req.onerror = (e) => reject(e.target.error);
    });
  }

  async getPendingMessages() {
    return new Promise((resolve, reject) => {
      const tx = this.db.transaction('messages', 'readonly');
      const store = tx.objectStore('messages');
      const index = store.index('status');
      const req = index.openCursor(IDBKeyRange.only('pending'));
      
      const msgs = [];
      req.onsuccess = (e) => {
        const cursor = e.target.result;
        if (cursor) {
          msgs.push(cursor.value);
          cursor.continue();
        } else {
          resolve(msgs);
        }
      };
      req.onerror = (e) => reject(e.target.error);
    });
  }
}

window.db = new RadcomDB();
