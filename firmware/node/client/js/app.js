// Main App Initialization
document.addEventListener('DOMContentLoaded', async () => {
  try {
    // 1. Initialize IndexedDB
    await window.db.init();
    
    // 2. Initialize UI (binds events, checks identity, renders views)
    window.ui.init();
    
    // 3. Hook up WS events to UI
    window.wsService.onStatusChange = (status) => {
      window.ui.updateConnectionStatus(status);
    };
    
    window.wsService.onMessageReceived = (msg) => {
      window.ui.appendMessage(msg);
    };
    
    window.wsService.onMessageStatus = (msgId, status) => {
      window.ui.updateMessageTick(msgId, status);
    };
    
    // 4. Handle Visibility (Backgrounding)
    document.addEventListener("visibilitychange", () => {
      window.wsService.onVisibilityChange();
    });

    console.log("Radcom Web App Initialized");
  } catch (e) {
    console.error("App init failed:", e);
    document.body.innerHTML = `<div style="padding: 20px; color: red;">Critical Error initializing database: ${e.message}</div>`;
  }
});
