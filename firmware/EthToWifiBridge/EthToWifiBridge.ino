/*
 * EthToWifiBridge - neonious one as an Ethernet -> WiFi adapter (NAT router)
 * ---------------------------------------------------------------------------
 * Hardware:  neonious one  (ESP32-D0WDQ6, 4MB flash @ 1.8V)
 *            LAN8720A Ethernet PHY (J1B1211CCD magjack)
 *
 * What it does:
 *   - Connects to your WiFi network as a station (the "internet" side).
 *   - Runs a DHCP server + NAT (NAPT) on the Ethernet port.
 *   - Any Ethernet-only device you plug into the RJ45 gets an IP from this
 *     board and its traffic is routed/NAT'd out over WiFi.
 *   - Hosts a web UI (WiFi scan, subnet/mask/DHCP range, OTA, reset) persisted
 *     in NVS, so settings can change WITHOUT reflashing.
 *
 * Why NAT and not a true bridge:
 *   The ESP32 WiFi stack in STATION mode cannot transparently L2-bridge
 *   foreign MAC addresses onto the WiFi link, so we route at L3 with NAPT
 *   instead (this is the same mechanism used by the ESP32 "WiFi-AP NAT"
 *   examples). Your Ethernet device will live on a separate subnet
 *   (192.168.5.0/24 by default) behind this board.
 *
 * Pin mapping (reverse-engineered from the board):
 *   RMII clock : GPIO0  (external 50MHz clock IN from PHY -> ETH_CLOCK_GPIO0_IN)
 *   MDC        : GPIO1
 *   MDIO       : GPIO23
 *   RMII data  : default ESP32 IO_MUX (GPIO 19,21,22,25,26,27) - fixed, not configurable
 *   PHY power  : none wired -> -1
 *
 * !!! IMPORTANT - NO SERIAL DEBUG !!!
 *   MDC is on GPIO1, which is also UART0 TX (the USB serial line). Once
 *   ETH.begin() reroutes GPIO1 to the EMAC, USB Serial output stops working.
 *   That's why this sketch leans on OTA + the web UI + the recovery AP.
 *
 * Web UI:
 *   Reachable on every interface (HTTP :80):
 *     - WiFi station IP (check your router)
 *     - Setup/recovery AP at http://192.168.4.1  (SSID "neonious-setup")
 *     - From the Ethernet device at http://<ethernet gateway IP>
 *   Login: user "admin", password = the current OTA password.
 *
 * Toolchain: Arduino-ESP32 core 3.x  (ESP-IDF 5.x). Board: "ESP32 Dev Module".
 *
 * OTA: enabled, password "admin" by default. Upload over the network from
 *      Arduino IDE -> Port -> network port, or via espota.
 */

#include <ETH.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <Preferences.h>
#include <stdarg.h>
#include "esp_netif.h"
#include "esp_netif_net_stack.h"     // esp_netif_get_netif_impl()
#include "esp_err.h"
#include "dhcpserver/dhcpserver.h"          // low-level lwIP DHCP server
#include "dhcpserver/dhcpserver_options.h"  // REQUESTED_IP_ADDRESS, lease time
#include "lwip/ip_addr.h"
#include "lwip/def.h"                       // lwip_htonl / lwip_ntohl
#include "lwip/netif.h"                     // struct netif (traffic counters)

// ---------------------- COMPILED-IN DEFAULTS (first boot) ------------------
// These are only used until you save settings via the web UI; after that the
// values stored in NVS win.
//
// WiFi SSID/pass are intentionally BLANK: on first boot (or after a reset) the
// board has no credentials, so it comes up as a setup access point (see the
// "no WiFi configured" path in setup()). Join that AP and enter your WiFi
// details in the web UI -- nothing secret is baked into the firmware image.
#define DEF_WIFI_SSID     ""
#define DEF_WIFI_PASS     ""
#define DEF_OTA_HOSTNAME  "neonious-eth-wifi"
#define DEF_OTA_PASSWORD  "admin"
#define DEF_ETH_IP        "192.168.5.1"      // this board / Ethernet gateway
#define DEF_ETH_MASK      "255.255.255.0"
#define DEF_DHCP_START    "192.168.5.100"
#define DEF_DHCP_END      "192.168.5.199"   // <=100 addrs (DHCPS_MAX_LEASE limit)

// Setup / recovery AP: brought up on first boot (no WiFi configured yet) and as
// a fallback if saved WiFi credentials won't connect, so you can always reach
// the web UI / OTA (there is no Serial console on this board). Change the AP
// password before deploying.
#define RECOVERY_AP_SSID  "neonious-setup"
#define RECOVERY_AP_PASS  "neonious"          // >= 8 chars; change me
#define WIFI_CONNECT_TIMEOUT_MS  20000

// Multi-reset factory wipe: tapping the on-board RESET button this many times in
// quick succession erases all settings and reboots to defaults (-> setup AP).
// This is the out-of-band escape hatch for WiFis with client isolation, where
// the web UI / /reset can't be reached over the air. A boot counter is kept in
// NVS (survives both the RESET button and a power cycle); it auto-clears once
// the board has been up past RESET_TAP_WINDOW_MS, so only *rapid* taps count.
#define RESET_TAP_COUNT      3
#define RESET_TAP_WINDOW_MS  8000
#define RESET_DET_NS         "resetdet"      // NVS namespace for the tap counter

// --------------------------- ETHERNET / PHY PINS ---------------------------
#define ETH_PHY_TYPE_   ETH_PHY_LAN8720
#define ETH_PHY_ADDR_   -1                 // -1 = auto-detect: driver scans the
                                           // MDIO bus for the PHY address.
#define ETH_PHY_MDC_    1                  // GPIO1
#define ETH_PHY_MDIO_   23                 // GPIO23
#define ETH_PHY_POWER_  5                  // enables the external 50MHz oscillator
                                           // (found via the clock/power sweep);
                                           // held low at boot, set high by the
                                           // driver so GPIO0 gets a clean clock.
#define ETH_CLK_MODE_   ETH_CLOCK_GPIO0_IN // external oscillator -> GPIO0 (clean
                                           // clock needed for reliable RMII data
                                           // on ESP32 rev1.0).

// ---------------------------------------------------------------------------
struct Config {
  String    wifiSsid;
  String    wifiPass;
  IPAddress ethIp;
  IPAddress ethMask;
  IPAddress dhcpStart;
  IPAddress dhcpEnd;
  String    otaHost;
  String    otaPass;
};

static Config       g_cfg;
static Preferences  g_prefs;
static WebServer    g_web(80);
static bool         g_ethRouterStarted = false;
static bool         g_recoveryMode     = false;
static bool         g_dnsOffered       = false;    // upstream DNS pushed to DHCP
static IPAddress    g_upstreamDns;                  // primary DNS from WiFi DHCP
static IPAddress    g_upstreamDns2;                 // secondary DNS from WiFi DHCP
                                                    // (cached: ETH coming up wipes
                                                    // the global lwIP DNS servers)
static dhcps_t     *g_dhcps            = nullptr;  // lwIP DHCP server for ETH

// Most-recent DHCP grant + total grant count (for the stats page).
static uint32_t     g_leaseCount = 0;
static IPAddress    g_lastLeaseIp;
static char         g_lastLeaseMac[18] = "";

// Ethernet traffic counters. The lwIP build has MIB2_STATS off, so we count by
// wrapping the ETH netif's input/linkoutput function pointers. Updated from the
// TCP/IP thread; read for display (small races are acceptable for a counter).
static volatile uint64_t  g_rxBytes = 0, g_txBytes = 0;
static volatile uint32_t  g_rxPkts  = 0, g_txPkts  = 0;
static bool               g_statsHooked    = false;
static netif_input_fn      g_origInput      = nullptr;
static netif_linkoutput_fn g_origLinkoutput = nullptr;

static const char *PREFS_NS = "ethwifi";

// --------------------------------- LOGGING ---------------------------------
// In-memory ring buffer viewable in the web UI (Serial is dead once ETH starts).
// logf() may be called from the network-event task and OTA callbacks as well as
// the main loop, so the ring is guarded by a mutex.
#define LOG_LINES    50
#define LOG_LINELEN  112
static char              g_log[LOG_LINES][LOG_LINELEN];
static int               g_logHead  = 0;   // next slot to write
static int               g_logCount = 0;   // valid lines so far
static SemaphoreHandle_t g_logMutex = nullptr;

static void logf(const char *fmt, ...) {
  char msg[LOG_LINELEN];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  char line[LOG_LINELEN];
  unsigned long s = millis() / 1000;
  snprintf(line, sizeof(line), "[%02lu:%02lu:%02lu] %s",
           s / 3600, (s / 60) % 60, s % 60, msg);
  Serial.println(line);   // harmless; invisible after ETH.begin()

  if (g_logMutex) xSemaphoreTake(g_logMutex, portMAX_DELAY);
  strncpy(g_log[g_logHead], line, LOG_LINELEN - 1);
  g_log[g_logHead][LOG_LINELEN - 1] = '\0';
  g_logHead = (g_logHead + 1) % LOG_LINES;
  if (g_logCount < LOG_LINES) g_logCount++;
  if (g_logMutex) xSemaphoreGive(g_logMutex);
}

static String logSnapshot() {
  String out;
  out.reserve(g_logCount * 28 + 16);
  if (g_logMutex) xSemaphoreTake(g_logMutex, portMAX_DELAY);
  int idx = (g_logCount < LOG_LINES) ? 0 : g_logHead;   // oldest line
  for (int i = 0; i < g_logCount; i++) {
    out += g_log[(idx + i) % LOG_LINES];
    out += '\n';
  }
  if (g_logMutex) xSemaphoreGive(g_logMutex);
  return out;
}

// ------------------------------- CONFIG I/O --------------------------------
static IPAddress parseIp(const String &s, const char *fallback) {
  IPAddress ip;
  if (ip.fromString(s)) return ip;
  ip.fromString(fallback);
  return ip;
}

static void loadConfig() {
  g_prefs.begin(PREFS_NS, /*readOnly=*/true);
  g_cfg.wifiSsid  = g_prefs.getString("wifiSsid", DEF_WIFI_SSID);
  g_cfg.wifiPass  = g_prefs.getString("wifiPass", DEF_WIFI_PASS);
  g_cfg.otaHost   = g_prefs.getString("otaHost",  DEF_OTA_HOSTNAME);
  g_cfg.otaPass   = g_prefs.getString("otaPass",  DEF_OTA_PASSWORD);
  g_cfg.ethIp     = parseIp(g_prefs.getString("ethIp",     DEF_ETH_IP),     DEF_ETH_IP);
  g_cfg.ethMask   = parseIp(g_prefs.getString("ethMask",   DEF_ETH_MASK),   DEF_ETH_MASK);
  g_cfg.dhcpStart = parseIp(g_prefs.getString("dhcpStart", DEF_DHCP_START), DEF_DHCP_START);
  g_cfg.dhcpEnd   = parseIp(g_prefs.getString("dhcpEnd",   DEF_DHCP_END),   DEF_DHCP_END);
  g_prefs.end();
}

static void saveConfig() {
  g_prefs.begin(PREFS_NS, /*readOnly=*/false);
  g_prefs.putString("wifiSsid",  g_cfg.wifiSsid);
  g_prefs.putString("wifiPass",  g_cfg.wifiPass);
  g_prefs.putString("otaHost",   g_cfg.otaHost);
  g_prefs.putString("otaPass",   g_cfg.otaPass);
  g_prefs.putString("ethIp",     g_cfg.ethIp.toString());
  g_prefs.putString("ethMask",   g_cfg.ethMask.toString());
  g_prefs.putString("dhcpStart", g_cfg.dhcpStart.toString());
  g_prefs.putString("dhcpEnd",   g_cfg.dhcpEnd.toString());
  g_prefs.end();
}

// ----------------------------- TRAFFIC COUNTERS ----------------------------
// Wrap the ETH netif's RX (input) and TX (linkoutput) paths to tally bytes and
// packets. Called from the TCP/IP thread; counters are plain volatiles.
static err_t statsInput(struct pbuf *p, struct netif *inp) {
  if (p) { g_rxBytes += p->tot_len; g_rxPkts++; }
  return g_origInput(p, inp);
}
static err_t statsLinkoutput(struct netif *n, struct pbuf *p) {
  if (p) { g_txBytes += p->tot_len; g_txPkts++; }
  return g_origLinkoutput(n, p);
}
static void hookEthStats(struct netif *n) {
  if (g_statsHooked || n == nullptr) return;
  g_origInput      = n->input;
  g_origLinkoutput = n->linkoutput;
  n->input         = statsInput;
  n->linkoutput    = statsLinkoutput;
  g_statsHooked    = true;
}

// --------------------------- ETHERNET NAT ROUTER ---------------------------
struct DhcpsCtx {
  struct netif *netif;
  ip4_addr_t    serverIp;
  ip4_addr_t    poolStart;
  ip4_addr_t    poolEnd;
  bool          hasDns;
  ip_addr_t     dns;        // primary
  bool          hasDns2;
  ip_addr_t     dns2;       // secondary
};

// Fires (in the TCP/IP thread) whenever a client is granted an address.
static void dhcpsLeaseCb(void *arg, u8_t ip[4], u8_t mac[6]) {
  g_leaseCount++;
  g_lastLeaseIp = IPAddress(ip[0], ip[1], ip[2], ip[3]);
  snprintf(g_lastLeaseMac, sizeof(g_lastLeaseMac), "%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  logf("DHCP lease: %s -> %s", g_lastLeaseIp.toString().c_str(), g_lastLeaseMac);
}

// Runs INSIDE the TCP/IP thread via esp_netif_tcpip_exec(). The lwIP dhcps_*
// functions are not thread-safe and panic the board if called from the event
// or loop task, so all of them must happen here.
static esp_err_t dhcpsStartCb(void *ctx) {
  DhcpsCtx *c = (DhcpsCtx *)ctx;
  if (g_dhcps == nullptr) g_dhcps = dhcps_new();
  if (g_dhcps == nullptr) return ESP_ERR_NO_MEM;
  dhcps_stop(g_dhcps, c->netif);                 // no-op if not running

  // Explicit address pool so the server always has leases to hand out.
  dhcps_lease_t pool = {};
  pool.enable        = true;
  pool.start_ip.addr = c->poolStart.addr;
  pool.end_ip.addr   = c->poolEnd.addr;
  dhcps_set_option_info(g_dhcps, REQUESTED_IP_ADDRESS, &pool, sizeof(pool));

  uint32_t leaseTime = 120;   // minutes
  dhcps_set_option_info(g_dhcps, IP_ADDRESS_LEASE_TIME, &leaseTime, sizeof(leaseTime));

  // Offer the upstream (WiFi) DNS to clients instead of our own IP. Without the
  // OFFER_DNS flag, lwIP hands out the server's own address as the DNS, but we
  // run no DNS forwarder, so client name resolution would fail.
  if (c->hasDns) {
    uint8_t offerDns = OFFER_DNS;   // 0x02
    dhcps_set_option_info(g_dhcps, DOMAIN_NAME_SERVER, &offerDns, sizeof(offerDns));
  }

  dhcps_set_new_lease_cb(g_dhcps, dhcpsLeaseCb, nullptr);

  err_t r = dhcps_start(g_dhcps, c->netif, c->serverIp);
  if (r == ERR_OK && c->hasDns) {
    // Offer both DNS servers; the DHCP OFFER appends the backup if it's set.
    dhcps_dns_setserver_by_type(g_dhcps, &c->dns, DNS_TYPE_MAIN);
    if (c->hasDns2) dhcps_dns_setserver_by_type(g_dhcps, &c->dns2, DNS_TYPE_BACKUP);
  }
  return (r == ERR_OK) ? ESP_OK : ESP_FAIL;
}

static void startEthRouter() {
  esp_netif_t *eth = ETH.netif();
  esp_netif_t *sta = WiFi.STA.netif();
  if (eth == nullptr || sta == nullptr) { logf("router: netif NULL, abort"); return; }

  esp_err_t e;

  // Switch ETH from DHCP client to a static router address.
  esp_netif_dhcpc_stop(eth);                 // ok if already stopped

  esp_netif_ip_info_t ipInfo = {};
  ipInfo.ip.addr      = (uint32_t)g_cfg.ethIp;
  ipInfo.gw.addr      = (uint32_t)g_cfg.ethIp;
  ipInfo.netmask.addr = (uint32_t)g_cfg.ethMask;
  e = esp_netif_set_ip_info(eth, &ipInfo);
  logf("router: set_ip %s -> %s", g_cfg.ethIp.toString().c_str(), esp_err_to_name(e));

  // The esp_netif ETH interface is created as a DHCP *client* and has no server
  // instance, so esp_netif_dhcps_start() fails on it (DHCPS_START_FAILED).
  // Instead run the lwIP DHCP server directly on the ETH netif; the lease pool
  // is auto-derived from the interface IP/netmask set above.
  struct netif *ethLwip = (struct netif *)esp_netif_get_netif_impl(eth);
  if (ethLwip == nullptr) { logf("router: no lwip netif, abort"); return; }
  hookEthStats(ethLwip);                      // start tallying traffic (once)

  // Gather params, then start the DHCP server inside the TCP/IP thread.
  DhcpsCtx ctx = {};
  ctx.netif         = ethLwip;
  ctx.serverIp.addr = (uint32_t)g_cfg.ethIp;

  // Lease pool: use the configured range if it's in the gateway's subnet,
  // otherwise derive x.x.x.100-200 from the gateway (assumes /24 default).
  uint32_t mask = (uint32_t)g_cfg.ethMask;
  bool rangeOk = (((uint32_t)g_cfg.dhcpStart & mask) == ((uint32_t)g_cfg.ethIp & mask)) &&
                 (((uint32_t)g_cfg.dhcpEnd   & mask) == ((uint32_t)g_cfg.ethIp & mask));
  IPAddress gw = g_cfg.ethIp;
  IPAddress ps = rangeOk ? g_cfg.dhcpStart : IPAddress(gw[0], gw[1], gw[2], 100);
  IPAddress pe = rangeOk ? g_cfg.dhcpEnd   : IPAddress(gw[0], gw[1], gw[2], 199);

  // dhcps rejects (and silently replaces) any pool larger than DHCPS_MAX_LEASE
  // (100 addresses), so clamp the span to keep the configured range in effect.
  uint32_t sHost = lwip_ntohl((uint32_t)ps);
  uint32_t eHost = lwip_ntohl((uint32_t)pe);
  if (eHost < sHost) eHost = sHost;
  if (eHost - sHost + 1 > 100) {
    eHost = sHost + 99;
    pe = IPAddress(lwip_htonl(eHost));
    logf("router: dhcp range >100 addrs, clamped end to %s", pe.toString().c_str());
  }
  ctx.poolStart.addr = (uint32_t)ps;
  ctx.poolEnd.addr   = (uint32_t)pe;
  logf("router: dhcp pool %s - %s", ps.toString().c_str(), pe.toString().c_str());

  // Offer the DNS server(s) the ESP32 itself got from the WiFi router's DHCP
  // (cached at WiFi-connect time, since bringing ETH up clears the live value).
  if ((uint32_t)g_upstreamDns != 0) {
    ip_addr_set_ip4_u32(&ctx.dns, (uint32_t)g_upstreamDns);
    ctx.hasDns = true;
    if ((uint32_t)g_upstreamDns2 != 0 && g_upstreamDns2 != g_upstreamDns) {
      ip_addr_set_ip4_u32(&ctx.dns2, (uint32_t)g_upstreamDns2);
      ctx.hasDns2 = true;
    }
    logf("router: offering upstream DNS %s%s%s to clients",
         g_upstreamDns.toString().c_str(),
         ctx.hasDns2 ? " + " : "",
         ctx.hasDns2 ? g_upstreamDns2.toString().c_str() : "");
  } else {
    logf("router: upstream DNS not ready yet (will retry)");
  }
  esp_err_t de = esp_netif_tcpip_exec(dhcpsStartCb, &ctx);
  logf("router: dhcps_start -> %s", esp_err_to_name(de));
  g_dnsOffered = (de == ESP_OK && ctx.hasDns);

  // Keep WiFi as the default route to the internet.
  esp_netif_set_default_netif(sta);

  // Enable NAT (NAPT) so Ethernet-side traffic is translated out over WiFi.
  e = esp_netif_napt_enable(eth);
  logf("router: napt_enable -> %s", esp_err_to_name(e));

  g_ethRouterStarted = true;
}

// Bring up the Ethernet router once both WiFi and the ETH link are ready.
static void onNetworkEvent(arduino_event_id_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      logf("WiFi: associated to AP"); break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      logf("WiFi: disconnected"); break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      logf("WiFi: got IP %s", WiFi.localIP().toString().c_str()); break;
    case ARDUINO_EVENT_ETH_START:
      logf("ETH: interface started"); break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      logf("ETH: link up"); break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      logf("ETH: link down");
      g_ethRouterStarted = false;   // reapply router config on next link-up
      g_dnsOffered = false;
      break;
    default:
      break;
  }

  // Bring up the router once both WiFi (with DNS) and the ETH link are ready.
  if ((event == ARDUINO_EVENT_ETH_CONNECTED ||
       event == ARDUINO_EVENT_WIFI_STA_GOT_IP) &&
      WiFi.STA.hasIP() && ETH.linkUp()) {
    startEthRouter();
  }
}

// --------------------------------- WEB UI ----------------------------------
static String htmlEscape(const String &s) {
  String o;
  o.reserve(s.length());
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;";  break;
      case '<': o += "&lt;";   break;
      case '>': o += "&gt;";   break;
      case '"': o += "&quot;"; break;
      default:  o += c;        break;
    }
  }
  return o;
}

static String jsonEscape(const String &s) {
  String o;
  o.reserve(s.length() + 2);
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c >= 0x20)        { o += c; }
  }
  return o;
}

static String humanBytes(uint64_t b) {
  const char *u[] = {"B", "KB", "MB", "GB", "TB"};
  double v = (double)b;
  int i = 0;
  while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
  char buf[32];
  snprintf(buf, sizeof(buf), (i == 0) ? "%.0f %s" : "%.2f %s", v, u[i]);
  return String(buf);
}

static bool requireAuth() {
  // HTTP Basic auth: user "admin", password = current OTA password.
  if (!g_web.authenticate("admin", g_cfg.otaPass.c_str())) {
    g_web.requestAuthentication();
    return false;
  }
  return true;
}

static String textInput(const char *name, const String &value) {
  return "<input name='" + String(name) + "' value='" + htmlEscape(value) + "'>";
}

static void handleRoot() {
  if (!requireAuth()) return;

  String status;
  if (g_recoveryMode) {
    status = "Setup AP active (" + String(RECOVERY_AP_SSID) +
             ") - enter your WiFi details below and save to start bridging.";
  } else {
    status = "WiFi connected: " + htmlEscape(WiFi.SSID()) +
             " (IP " + WiFi.localIP().toString() + ")<br>"
             "Ethernet router: " + (g_ethRouterStarted ? "up" : "starting") +
             " (gateway " + g_cfg.ethIp.toString() + ")<br>"
             "Ethernet link: " + (ETH.linkUp() ? "connected" : "down");
  }

  String html;
  html += F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>neonious Eth-WiFi adapter</title>"
            "<style>body{font-family:system-ui,sans-serif;max-width:520px;"
            "margin:24px auto;padding:0 16px;color:#222}"
            "h1{font-size:1.3rem}label{display:block;margin:14px 0 4px;"
            "font-weight:600}input{width:100%;box-sizing:border-box;padding:8px;"
            "font-size:1rem;border:1px solid #bbb;border-radius:6px}"
            ".hint{color:#666;font-size:.85rem;margin-top:4px}"
            ".row{display:flex;gap:10px}.row>div{flex:1}"
            ".card{background:#f6f6f6;border-radius:10px;padding:10px 16px;"
            "margin-bottom:18px}button{margin-top:10px;padding:10px 18px;"
            "font-size:1rem;border:0;border-radius:6px;background:#2563eb;"
            "color:#fff;cursor:pointer}button.sec{background:#555}"
            "button.danger{background:#dc2626}</style></head><body>");
  html += F("<h1>neonious Ethernet &rarr; WiFi adapter</h1>");
  html += "<div class='card'>" + status + "</div>";
  html += F("<div class='card'><b>Ethernet traffic</b>"
            "<div id='stats' style='font-family:monospace;margin-top:6px'>"
            "loading...</div></div>");
  html += F("<p><a href='/log'>View live log &rarr;</a></p>");

  html += F("<form method='POST' action='/save'>");

  // ---- WiFi ----
  html += F("<label>WiFi SSID</label>"
            "<input name='wifiSsid' list='nets' value='");
  html += htmlEscape(g_cfg.wifiSsid);
  html += F("'><datalist id='nets'></datalist>"
            "<button type='button' class='sec' onclick='scan(this)'>"
            "Scan WiFi</button>");

  html += F("<label>WiFi password</label>"
            "<input name='wifiPass' type='password' placeholder='(unchanged)'>"
            "<div class='hint'>Leave blank to keep the current password.</div>");

  // ---- Ethernet subnet ----
  html += F("<label>Ethernet gateway IP</label>");
  html += textInput("ethIp", g_cfg.ethIp.toString());
  html += F("<label>Subnet mask</label>");
  html += textInput("ethMask", g_cfg.ethMask.toString());

  html += F("<div class='row'><div><label>DHCP range start</label>");
  html += textInput("dhcpStart", g_cfg.dhcpStart.toString());
  html += F("</div><div><label>DHCP range end</label>");
  html += textInput("dhcpEnd", g_cfg.dhcpEnd.toString());
  html += F("</div></div>"
            "<div class='hint'>Start/end must be inside the gateway's subnet, "
            "else a default pool is used.</div>");

  // ---- OTA ----
  html += F("<label>OTA / web hostname</label>");
  html += textInput("otaHost", g_cfg.otaHost);
  html += F("<label>OTA &amp; web password</label>"
            "<input name='otaPass' type='password' placeholder='(unchanged)'>"
            "<div class='hint'>Used for OTA uploads and to log in here. "
            "Leave blank to keep current.</div>");

  html += F("<button type='submit'>Save &amp; reboot</button></form>");

  // ---- Reset ----
  html += F("<form method='POST' action='/reset' style='margin-top:24px' "
            "onsubmit='return confirm(\"Erase ALL saved settings and reboot "
            "to compiled defaults?\")'>"
            "<button type='submit' class='danger'>Reset to defaults</button>"
            "</form>");

  // ---- Scan script ----
  html += F("<script>function scan(b){b.disabled=true;b.textContent="
            "'Scanning...';fetch('/scan').then(r=>r.json()).then(j=>{"
            "var dl=document.getElementById('nets');dl.innerHTML='';"
            "j.sort((a,c)=>c.rssi-a.rssi).forEach(n=>{var o="
            "document.createElement('option');o.value=n.ssid;o.label="
            "n.rssi+' dBm'+(n.enc?'':' (open)');dl.appendChild(o);});"
            "b.disabled=false;b.textContent='Scan WiFi ('+j.length+')';"
            "}).catch(e=>{b.disabled=false;b.textContent='Scan failed';});}"
            "function us(){fetch('/stats.json').then(r=>r.json()).then(s=>{"
            "document.getElementById('stats').innerHTML="
            "'link: '+(s.link?('up '+s.mbps+' Mbps '+s.duplex):'down')+"
            "'<br>uptime: '+s.up+"
            "'<br>running since: '+s.since+"
            "'<br>now: '+s.now+"
            "'<br>RX: '+s.rxh+' ('+s.rxp+' pkts)'+"
            "'<br>TX: '+s.txh+' ('+s.txp+' pkts)'+"
            "'<br>DHCP clients: '+s.clients+', last: '+s.last+"
            "'<br>WiFi: '+s.ssid+' '+s.wifi+' ('+s.rssi+' dBm)'+"
            "'<br>LAN gateway: '+s.gw+"
            "'<br>free heap: '+s.heap+' B'+"
            "'<br>firmware: '+s.fw;});}"
            "us();setInterval(us,2000);"
            "</script>");

  html += F("</body></html>");
  g_web.send(200, "text/html", html);
}

static String uptimeStr(uint32_t s) {
  char b[24];
  snprintf(b, sizeof(b), "%lud %02lu:%02lu:%02lu", (unsigned long)(s / 86400),
           (unsigned long)((s / 3600) % 24), (unsigned long)((s / 60) % 60),
           (unsigned long)(s % 60));
  return String(b);
}

static void handleStats() {
  if (!requireAuth()) return;

  uint32_t up = millis() / 1000;
  time_t now = time(nullptr);
  bool synced = now > 1700000000;          // SNTP gives a post-2023 epoch
  String since = "since boot (no NTP yet)";
  String nowStr = "n/a";
  if (synced) {
    time_t boot = now - up;
    struct tm tmv;
    char tb[40];
    gmtime_r(&boot, &tmv); strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S UTC", &tmv);
    since = tb;
    gmtime_r(&now, &tmv);  strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S UTC", &tmv);
    nowStr = tb;
  }

  String last = g_leaseCount ? (g_lastLeaseIp.toString() + " (" + g_lastLeaseMac + ")")
                             : String("none");

  String j = "{";
  j += "\"link\":" + String(ETH.linkUp() ? "true" : "false");
  j += ",\"mbps\":" + String((int)ETH.linkSpeed());
  j += ",\"duplex\":\"" + String(ETH.fullDuplex() ? "full" : "half") + "\"";
  j += ",\"rx\":" + String((unsigned long long)g_rxBytes);
  j += ",\"tx\":" + String((unsigned long long)g_txBytes);
  j += ",\"rxp\":" + String((unsigned long)g_rxPkts);
  j += ",\"txp\":" + String((unsigned long)g_txPkts);
  j += ",\"rxh\":\"" + humanBytes(g_rxBytes) + "\"";
  j += ",\"txh\":\"" + humanBytes(g_txBytes) + "\"";
  j += ",\"up\":\"" + uptimeStr(up) + "\"";
  j += ",\"since\":\"" + since + "\"";
  j += ",\"now\":\"" + nowStr + "\"";
  j += ",\"clients\":" + String((unsigned long)g_leaseCount);
  j += ",\"last\":\"" + jsonEscape(last) + "\"";
  j += ",\"rssi\":" + String(WiFi.RSSI());
  j += ",\"ssid\":\"" + jsonEscape(WiFi.SSID()) + "\"";
  j += ",\"wifi\":\"" + WiFi.localIP().toString() + "\"";
  j += ",\"gw\":\"" + g_cfg.ethIp.toString() + "\"";
  j += ",\"heap\":" + String((unsigned long)ESP.getFreeHeap());
  j += ",\"fw\":\"" __DATE__ " " __TIME__ "\"";
  j += "}";
  g_web.send(200, "application/json", j);
}

static void handleScan() {
  if (!requireAuth()) return;

  int n = WiFi.scanNetworks();   // synchronous; briefly disrupts the uplink
  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i) json += ",";
    json += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) +
            "\",\"rssi\":" + String(WiFi.RSSI(i)) +
            ",\"enc\":" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "false" : "true") +
            "}";
  }
  json += "]";
  WiFi.scanDelete();
  logf("WiFi scan: %d network(s) found", n);
  g_web.send(200, "application/json", json);
}

static void handleSave() {
  if (!requireAuth()) return;

  if (g_web.hasArg("wifiSsid"))
    g_cfg.wifiSsid = g_web.arg("wifiSsid");
  if (g_web.hasArg("wifiPass") && g_web.arg("wifiPass").length() > 0)
    g_cfg.wifiPass = g_web.arg("wifiPass");
  if (g_web.hasArg("otaHost") && g_web.arg("otaHost").length() > 0)
    g_cfg.otaHost = g_web.arg("otaHost");
  if (g_web.hasArg("otaPass") && g_web.arg("otaPass").length() > 0)
    g_cfg.otaPass = g_web.arg("otaPass");

  IPAddress parsed;
  if (g_web.hasArg("ethIp")     && parsed.fromString(g_web.arg("ethIp")))     g_cfg.ethIp     = parsed;
  if (g_web.hasArg("ethMask")   && parsed.fromString(g_web.arg("ethMask")))   g_cfg.ethMask   = parsed;
  if (g_web.hasArg("dhcpStart") && parsed.fromString(g_web.arg("dhcpStart"))) g_cfg.dhcpStart = parsed;
  if (g_web.hasArg("dhcpEnd")   && parsed.fromString(g_web.arg("dhcpEnd")))   g_cfg.dhcpEnd   = parsed;

  saveConfig();
  logf("config saved via web UI, rebooting");

  g_web.send(200, "text/html",
             F("<!DOCTYPE html><html><body style='font-family:sans-serif'>"
               "<h2>Saved. Rebooting...</h2>"
               "<p>The board will restart and apply the new settings. "
               "Reconnect, then browse back to it.</p></body></html>"));
  delay(800);
  ESP.restart();
}

static void handleReset() {
  if (!requireAuth()) return;

  g_prefs.begin(PREFS_NS, /*readOnly=*/false);
  g_prefs.clear();
  g_prefs.end();
  logf("settings erased via web UI, rebooting to defaults");

  g_web.send(200, "text/html",
             F("<!DOCTYPE html><html><body style='font-family:sans-serif'>"
               "<h2>Settings erased. Rebooting to defaults...</h2></body></html>"));
  delay(800);
  ESP.restart();
}

static void handleLogTxt() {
  if (!requireAuth()) return;
  g_web.send(200, "text/plain", logSnapshot());
}

static void handleLogPage() {
  if (!requireAuth()) return;
  g_web.send(200, "text/html", F(
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>neonious log</title><style>body{font-family:system-ui,sans-serif;"
    "max-width:760px;margin:16px auto;padding:0 12px}"
    "pre{background:#111;color:#3f3;padding:12px;border-radius:8px;"
    "overflow:auto;font-size:.82rem;white-space:pre-wrap;line-height:1.35}"
    "a{color:#2563eb}label{font-size:.9rem}</style></head><body>"
    "<p><a href='/'>&larr; settings</a> &nbsp;&nbsp;"
    "<label><input type='checkbox' id='au' checked> auto-refresh (2s)</label></p>"
    "<pre id='l'>loading...</pre>"
    "<script>function u(){fetch('/log.txt').then(r=>r.text()).then(t=>{"
    "var p=document.getElementById('l');"
    "var atBottom=p.scrollHeight-p.scrollTop-p.clientHeight<24;"
    "p.textContent=t;if(atBottom)p.scrollTop=p.scrollHeight;});}"
    "u();setInterval(function(){if(document.getElementById('au').checked)u();}"
    ",2000);</script></body></html>"));
}

static void setupWeb() {
  g_web.on("/", HTTP_GET, handleRoot);
  g_web.on("/scan", HTTP_GET, handleScan);
  g_web.on("/stats.json", HTTP_GET, handleStats);
  g_web.on("/save", HTTP_POST, handleSave);
  g_web.on("/reset", HTTP_POST, handleReset);
  g_web.on("/log", HTTP_GET, handleLogPage);
  g_web.on("/log.txt", HTTP_GET, handleLogTxt);
  g_web.onNotFound([]() { g_web.send(404, "text/plain", "Not found"); });
  g_web.begin();
}

// --------------------------------- OTA -------------------------------------
static void setupOTA() {
  ArduinoOTA.setHostname(g_cfg.otaHost.c_str());
  ArduinoOTA.setPassword(g_cfg.otaPass.c_str());

  ArduinoOTA.onStart([]() { logf("OTA: update started"); });
  ArduinoOTA.onEnd([]()   { logf("OTA: update complete, rebooting"); });
  ArduinoOTA.onError([](ota_error_t e) { logf("OTA: error %u", (unsigned)e); });
  ArduinoOTA.onProgress([](unsigned int cur, unsigned int total) {
    static int lastPct = -1;
    int pct = total ? (int)(cur * 100 / total) : 0;
    if (pct / 25 != lastPct / 25) { lastPct = pct; logf("OTA: %d%%", pct); }
  });

  ArduinoOTA.begin();
  logf("OTA ready: host '%s'", g_cfg.otaHost.c_str());
}

// Bring up the setup/recovery access point so the web UI + OTA stay reachable
// without a Serial console. AP_STA mode keeps the AP up while still allowing
// WiFi scans from the config page.
static void startSetupAp(const char *reason) {
  g_recoveryMode = true;
  logf("%s; starting setup AP '%s' (192.168.4.1)", reason, RECOVERY_AP_SSID);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(RECOVERY_AP_SSID, RECOVERY_AP_PASS);
  setupOTA();
  setupWeb();
}

// Count this boot toward the "tap RESET N times to factory-reset" gesture, and
// wipe settings if the threshold is reached. Runs very early in setup() (before
// loadConfig) so a triggered reset takes effect on this same boot. The counter
// is cleared again from loop() once the board has run past RESET_TAP_WINDOW_MS.
static bool checkMultiResetFactory() {
  Preferences rd;
  rd.begin(RESET_DET_NS, /*readOnly=*/false);
  uint32_t taps = rd.getUInt("cnt", 0) + 1;

  if (taps >= RESET_TAP_COUNT) {
    rd.putUInt("cnt", 0);
    rd.end();
    logf("RESET tapped %ux -> FACTORY RESET (erasing settings)", (unsigned)taps);
    g_prefs.begin(PREFS_NS, /*readOnly=*/false);
    g_prefs.clear();
    g_prefs.end();
    return true;
  }

  rd.putUInt("cnt", taps);
  rd.end();
  logf("boot tap %u/%u - tap RESET again within %us to factory-reset",
       (unsigned)taps, (unsigned)RESET_TAP_COUNT, (unsigned)(RESET_TAP_WINDOW_MS / 1000));
  return false;
}

// Clear the multi-reset tap counter after the board has stayed up long enough
// that this is clearly a normal boot, not part of a rapid reset sequence.
static void clearResetTapCounterOnce() {
  static bool done = false;
  if (done || millis() < RESET_TAP_WINDOW_MS) return;
  done = true;
  Preferences rd;
  rd.begin(RESET_DET_NS, /*readOnly=*/false);
  if (rd.getUInt("cnt", 0) != 0) {
    rd.putUInt("cnt", 0);
    logf("reset-tap counter cleared (normal uptime reached)");
  }
  rd.end();
}

// --------------------------------- SETUP -----------------------------------
void setup() {
  // Serial output dies after ETH.begin() (GPIO1 = MDC), but boot banner is fine.
  Serial.begin(115200);

  g_logMutex = xSemaphoreCreateMutex();
  logf("boot: neonious Ethernet -> WiFi adapter");

  // Out-of-band recovery: N rapid RESET taps wipe settings before we load them.
  checkMultiResetFactory();

  loadConfig();
  logf("config loaded, target SSID '%s'", g_cfg.wifiSsid.c_str());
  Network.onEvent(onNetworkEvent);

  // First boot / after reset: no WiFi credentials yet -> come up as a setup AP
  // so the user can enter their network details in the web UI. (No credentials
  // are compiled into the firmware.)
  if (g_cfg.wifiSsid.length() == 0) {
    startSetupAp("no WiFi configured");
    return;                          // no bridging until WiFi is set
  }

  // ---- WiFi (internet side) ----
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);            // keep latency/throughput sane for routing
  logf("WiFi: connecting to '%s'...", g_cfg.wifiSsid.c_str());
  WiFi.begin(g_cfg.wifiSsid.c_str(), g_cfg.wifiPass.c_str());

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED &&
         (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
  }

  if (WiFi.status() != WL_CONNECTED) {
    // Could not join the configured WiFi: fall back to the setup/recovery AP so
    // the web UI / OTA still work and the credentials can be fixed.
    startSetupAp("WiFi connect FAILED");
    return;                        // no bridging; recovery mode only
  }

  logf("WiFi: connected, IP %s", WiFi.localIP().toString().c_str());
  // Cache the upstream DNS now, while it's still valid (ETH init wipes it).
  g_upstreamDns  = WiFi.dnsIP(0);
  g_upstreamDns2 = WiFi.dnsIP(1);
  logf("WiFi: gw %s, dns %s / %s", WiFi.gatewayIP().toString().c_str(),
       g_upstreamDns.toString().c_str(), g_upstreamDns2.toString().c_str());

  // Start SNTP so the stats page can show wall-clock boot/current time.
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");

  // Bring OTA + web up first so the board is always reachable, even if the
  // Ethernet init below misbehaves.
  setupOTA();
  setupWeb();

  // ---- Ethernet (device side) ----
  // After this call, USB Serial (GPIO1/TX) is no longer usable.
  const char *clkName =
      (ETH_CLK_MODE_ == ETH_CLOCK_GPIO0_IN)   ? "GPIO0_IN"  :
      (ETH_CLK_MODE_ == ETH_CLOCK_GPIO0_OUT)  ? "GPIO0_OUT" :
      (ETH_CLK_MODE_ == ETH_CLOCK_GPIO16_OUT) ? "GPIO16_OUT":
      (ETH_CLK_MODE_ == ETH_CLOCK_GPIO17_OUT) ? "GPIO17_OUT": "?";
  logf("ETH: starting LAN8720 (PHY addr %d, clk %s, MDC=%d MDIO=%d)...",
       ETH_PHY_ADDR_, clkName, ETH_PHY_MDC_, ETH_PHY_MDIO_);
  bool ethOk = ETH.begin(ETH_PHY_TYPE_, ETH_PHY_ADDR_, ETH_PHY_MDC_,
                         ETH_PHY_MDIO_, ETH_PHY_POWER_, ETH_CLK_MODE_);
  logf("ETH.begin() = %s",
       ethOk ? "OK (PHY found on MDIO)" : "FAILED (no PHY response on MDIO)");

  // If the link is already up by now, configure immediately; otherwise the
  // network event handler will do it when ETH_CONNECTED fires.
  if (ETH.linkUp()) startEthRouter();
}

void loop() {
  ArduinoOTA.handle();
  g_web.handleClient();

  // Once we've been up a while, this boot wasn't part of a rapid RESET-tap
  // sequence, so reset the factory-wipe tap counter.
  clearResetTapCounterOnce();

  // Belt-and-suspenders: if events were missed, start the router once ready.
  if (!g_recoveryMode && !g_ethRouterStarted &&
      WiFi.status() == WL_CONNECTED && ETH.linkUp()) {
    startEthRouter();
  }

  // The upstream DNS often isn't populated at link-up; once it appears,
  // re-apply the router config so clients are offered the real DNS server.
  if (!g_recoveryMode && g_ethRouterStarted && !g_dnsOffered) {
    static uint32_t lastDnsTry = 0;
    if (millis() - lastDnsTry > 2000) {
      lastDnsTry = millis();
      // Re-cache from the live value if we never got one (best effort).
      if ((uint32_t)g_upstreamDns == 0 && WiFi.status() == WL_CONNECTED) {
        g_upstreamDns  = WiFi.dnsIP(0);
        g_upstreamDns2 = WiFi.dnsIP(1);
      }
      if ((uint32_t)g_upstreamDns != 0) {
        logf("router: upstream DNS now available, re-applying");
        g_ethRouterStarted = false;
        startEthRouter();
      }
    }
  }

  // Poll Ethernet link state and log transitions (catches missed events and
  // shows the negotiated speed/duplex for diagnostics).
  if (!g_recoveryMode) {
    static bool     lastLink = false;
    static uint32_t lastPoll = 0;
    if (millis() - lastPoll > 3000) {
      lastPoll = millis();
      bool lk = ETH.linkUp();
      if (lk != lastLink) {
        lastLink = lk;
        logf("ETH: link %s (%d Mbps, %s)", lk ? "UP" : "DOWN",
             ETH.linkSpeed(), ETH.fullDuplex() ? "full" : "half");
      }
    }
  }
}
