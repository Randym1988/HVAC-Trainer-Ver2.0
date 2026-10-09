/*
 * Vexera trainer auto-discovery.
 *
 * Sends "DISCOVER_HVAC_TRAINER" as a UDP broadcast on port 4210 through the native
 * UdpDiscovery plugin and collects the engines that answer. Connection manager:
 *   one server   -> connect automatically
 *   several      -> "Select Your Trainer" dialog
 *   none         -> manual address dialog with Retry Discovery
 */
(function () {
  var DISCOVERY_PORT = 4210;
  var DISCOVERY_REQUEST = "DISCOVER_HVAC_TRAINER";
  var SERVICE_NAME = "HVAC_HIL_TRAINER";
  var SCAN_TIMEOUT_MS = 3000;
  var HOST_KEY = "vexera_engine_host";
  var MQTT_KEY = "vexera_engine_mqtt_port";

  function nativePlugin() {
    var plugins = (window.Capacitor && window.Capacitor.Plugins) || {};
    return plugins.UdpDiscovery || null;
  }

  // Replies are untrusted network input: keep only well-formed, same-service answers.
  function parseReply(entry) {
    var data;
    try {
      data = JSON.parse(entry.payload);
    } catch (e) {
      return null;
    }
    if (!data || data.service !== SERVICE_NAME) return null;
    var httpPort = Number(data.http_port);
    var mqttPort = Number(data.mqtt_port);
    if (!/^\d{1,3}(\.\d{1,3}){3}$/.test(String(entry.ip))) return null;
    if (!Number.isInteger(httpPort) || httpPort < 1 || httpPort > 65535) return null;
    return {
      ip: String(entry.ip),
      http_port: httpPort,
      mqtt_port: Number.isInteger(mqttPort) ? mqttPort : 1883,
      hostname: String(data.hostname || entry.ip).slice(0, 64),
      version: String(data.version || ""),
    };
  }

  var AutoDiscoveryService = {
    /** Resolves to an array of { ip, http_port, mqtt_port, hostname, version }. */
    discover: async function (timeoutMs) {
      var plugin = nativePlugin();
      if (!plugin) return [];
      try {
        var result = await plugin.discover({
          payload: DISCOVERY_REQUEST,
          port: DISCOVERY_PORT,
          timeoutMs: timeoutMs || SCAN_TIMEOUT_MS,
        });
        return (result.servers || []).map(parseReply).filter(Boolean);
      } catch (e) {
        console.warn("Trainer discovery failed:", e);
        return [];
      }
    },
  };

  function el(tag, styles, text) {
    var node = document.createElement(tag);
    if (styles) node.style.cssText = styles;
    if (text !== undefined) node.textContent = text;
    return node;
  }

  var overlay = null;

  function closeDialog() {
    if (overlay) overlay.remove();
    overlay = null;
  }

  function openDialog(title, message) {
    closeDialog();
    overlay = el(
      "div",
      "position:fixed;inset:0;z-index:10000;display:flex;align-items:center;justify-content:center;" +
        "background:rgba(0,0,0,0.75);padding:20px",
    );
    var card = el(
      "div",
      "width:100%;max-width:380px;background:var(--bg-card,#18181b);border:1px solid var(--border-color,#333);" +
        "border-radius:14px;padding:22px;color:var(--text-main,#f4f4f5)",
    );
    card.appendChild(el("div", "font-size:16px;font-weight:700;margin-bottom:8px", title));
    card.appendChild(
      el("div", "font-size:13px;color:var(--text-muted,#a1a1aa);margin-bottom:16px", message),
    );
    overlay.appendChild(card);
    document.body.appendChild(overlay);
    return card;
  }

  function button(label, primary, onClick) {
    var b = el(
      "button",
      "width:100%;padding:13px;margin-top:8px;border-radius:10px;font-size:14px;font-weight:600;cursor:pointer;" +
        "border:1px solid var(--border-color,#333);color:white;background:" +
        (primary ? "var(--accent-blue,#0ea5e9)" : "transparent"),
      label,
    );
    b.onclick = onClick;
    return b;
  }

  function applyServer(server) {
    var host = server.ip + ":" + server.http_port;
    localStorage.setItem(HOST_KEY, host);
    localStorage.setItem(MQTT_KEY, String(server.mqtt_port));
    window.dispatchEvent(new CustomEvent("vexera:server-selected", { detail: server }));
    closeDialog();
  }

  function showSelection(servers) {
    var card = openDialog("Select Your Trainer", "More than one training server answered on this network.");
    servers.forEach(function (server) {
      card.appendChild(
        button(server.hostname + "  (" + server.ip + ")", false, function () {
          applyServer(server);
        }),
      );
    });
    card.appendChild(button("Rescan", false, run));
  }

  function showManual() {
    var card = openDialog(
      "Training server not found",
      "No server answered within 3 seconds. Check that you are on the lab Wi-Fi, or enter the address manually.",
    );
    var input = el(
      "input",
      "width:100%;padding:13px;border-radius:10px;border:1px solid var(--border-color,#333);" +
        "background:rgba(0,0,0,0.5);color:white;font-size:14px;outline:none",
    );
    input.type = "text";
    input.placeholder = "192.168.1.20:8000";
    input.value = localStorage.getItem(HOST_KEY) || "";
    card.appendChild(input);
    var error = el("div", "font-size:12px;color:var(--accent-red,#ef4444);margin-top:6px;min-height:16px", "");
    card.appendChild(error);
    card.appendChild(
      button("Connect", true, function () {
        var value = input.value.trim().replace(/^https?:\/\//i, "").replace(/\/+$/, "");
        if (!/^[^/\s]+(:\d+)?$/.test(value)) {
          error.textContent = "Enter an address like 192.168.1.20:8000";
          return;
        }
        var parts = value.split(":");
        applyServer({
          ip: parts[0],
          http_port: parts[1] ? Number(parts[1]) : 8000,
          mqtt_port: 1883,
          hostname: parts[0],
        });
      }),
    );
    card.appendChild(button("Retry Discovery", false, run));
  }

  function showScanning() {
    openDialog("Looking for your trainer…", "Scanning the local network.");
  }

  var running = false;

  async function run() {
    if (running) return;
    running = true;
    showScanning();
    try {
      var servers = await AutoDiscoveryService.discover();
      if (servers.length === 1) applyServer(servers[0]);
      else if (servers.length > 1) showSelection(servers);
      else showManual();
    } finally {
      running = false;
    }
  }

  window.AutoDiscoveryService = AutoDiscoveryService;
  window.VexeraDiscovery = { run: run };

  window.addEventListener("load", function () {
    var native =
      window.Capacitor &&
      typeof window.Capacitor.getPlatform === "function" &&
      window.Capacitor.getPlatform() !== "web";
    if (native) run();
  });
})();
