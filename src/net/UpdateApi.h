#pragma once
#include <WebServer.h>

// The portal's WebServer, plus one hook the stock class lacks: the stream
// timeout of the client CURRENTLY being parsed (server.client() returns a copy,
// whose timeout doesn't affect the parse loop). See handleFileRaw().
class PortalServer : public WebServer {
  public:
    using WebServer::WebServer;
    void setCurrentReadTimeoutMs(unsigned long ms) { static_cast<Stream &>(_currentClient).setTimeout(ms); }
};

// Phone Update Bridge + data-update HTTP API (/api/v1/*) — protocol in
// docs/WIFI_PORTAL_UPDATE_BRIDGE_PLAN.md §8-§9. Runs entirely inside the web
// task (net/WebPortal.cpp), like every other handler there.
void updateApiRegister(PortalServer &srv); // routes + header collection; call once
void updateApiLoop();                   // deferred reboot / direct-update kick; call every web-task tick
bool updateApiBusy();                   // a phone is mid-update: keep the AP quiet (no scans, no auto-off)
