#include "web_server.h"
#include "sdkconfig.h"
#include "router_core.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "psa/crypto.h"
#include "lwip/opt.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include <net/if.h>
#include "lwip/inet.h"
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <stdbool.h>
#include <ctype.h>
#include <math.h>
#include "driver/gpio.h"
#include "system_metrics.h"
#include "router_log.h"

#define TAG "web"
#define NS "router"
#define KEY_AUTH_HASH "auth_hash"
#define KEY_AUTH_SALT "auth_salt"
#define KEY_OTA_HASH "ota_hash"
#define KEY_OTA_SALT "ota_salt"
#define SESSION_TTL_US (30LL * 60LL * 1000000LL)
#define DNS_PORT 53
#define DNS_BUF 512
#define LED_RED 4
#define LED_YELLOW 5
#define LED_GREEN 6

static httpd_handle_t s_http;
static char s_session[65];
static int64_t s_session_expiry;
static uint8_t s_login_failures;
static int64_t s_login_lock_until;
static TaskHandle_t s_metrics_task;

static const char INDEX_HTML[] =
"<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1,viewport-fit=cover\"><meta name=\"theme-color\" content=\"#101112\"><title>Router</title><style>\n"
":root{--bg:#f1f1ef;--ink:#101112;--panel:#fff;--soft:#f7f7f5;--line:#deded9;--muted:#70736f;--blue:#315cf5;--good:#1d9b61;--warn:#b88408;--bad:#d54848;--radius:18px}*{box-sizing:border-box}html{background:var(--bg)}body{margin:0;background:var(--bg);color:var(--ink);font:14px/1.45 system-ui,-apple-system,BlinkMacSystemFont,\"Segoe UI\",sans-serif;-webkit-font-smoothing:antialiased}button,input{font:inherit}button{cursor:pointer}.shell{max-width:1180px;margin:auto;padding:18px 18px 42px}.top{display:flex;align-items:center;gap:12px;margin-bottom:18px}.menu{width:42px;height:42px;border:1px solid #2a2b2c;background:var(--ink);color:#fff;border-radius:13px;font-size:19px;display:grid;place-items:center}.brand{display:flex;align-items:center;gap:11px;flex:1}.mark{width:40px;height:40px;border-radius:13px;background:#fff;border:1px solid var(--line);display:grid;place-items:center;font-weight:800}.title{font-weight:750;letter-spacing:.1px}.sub{font-size:12px;color:var(--muted)}.grid{display:grid;grid-template-columns:repeat(4,1fr);gap:10px}.card{background:var(--panel);border:1px solid var(--line);border-radius:var(--radius);padding:16px;box-shadow:0 1px 0 rgba(0,0,0,.02)}.wide{grid-column:span 2}.label{font-size:11px;color:var(--muted);text-transform:uppercase;letter-spacing:.08em;margin-bottom:6px}.value{font-size:23px;font-weight:760;letter-spacing:-.3px}.row{display:flex;justify-content:space-between;align-items:center;gap:12px}.meter{height:6px;background:#e8e8e4;border-radius:99px;overflow:hidden;margin-top:11px}.meter i{display:block;height:100%;width:0;background:var(--good);transition:width .22s ease}.table{width:100%;border-collapse:collapse}.table td{padding:8px 0;border-bottom:1px solid var(--line)}.table tr:last-child td{border-bottom:0}.table td:last-child{text-align:right;color:var(--muted);max-width:60%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.pill{display:inline-flex;align-items:center;gap:7px;padding:5px 9px;border-radius:99px;background:var(--soft);border:1px solid var(--line);font-size:11px}.dot{width:7px;height:7px;border-radius:50%;background:#9a9d99}.dot.good{background:var(--good)}.dot.warn{background:var(--warn)}.dot.bad{background:var(--bad)}.authbox{max-width:430px;margin:14px auto 0}.inputwrap{position:relative}.input{width:100%;padding:11px 42px 11px 12px;background:var(--soft);border:1px solid var(--line);border-radius:11px;color:var(--ink);outline:0;margin-bottom:9px}.input:focus{border-color:#9da5b5;box-shadow:0 0 0 3px rgba(49,92,245,.08)}.eye{position:absolute;right:7px;top:5px;width:34px;height:34px;border:0;background:transparent;color:var(--muted);border-radius:9px}.btn{padding:10px 13px;border:1px solid var(--line);background:#fff;color:var(--ink);border-radius:11px;transition:transform .12s ease,background .12s ease}.btn:active{transform:scale(.98)}.btn.primary{background:var(--ink);border-color:var(--ink);color:#fff}.btn.danger{color:var(--bad);border-color:#edcaca;background:#fff}.actions{display:flex;gap:8px;flex-wrap:wrap}.private.hidden{display:none}.notice{padding:11px 12px;border:1px solid var(--line);border-radius:11px;background:var(--soft);color:var(--muted);font-size:12px}.overlay{position:fixed;inset:0;background:rgba(16,17,18,.38);backdrop-filter:blur(2px);display:none;align-items:stretch;justify-content:flex-start;z-index:20}.drawer{height:100%;width:min(450px,94vw);background:#101112;color:#f7f7f5;border-right:1px solid #2a2b2c;padding:18px;overflow:auto;box-shadow:12px 0 35px rgba(0,0,0,.18)}.drawer .sub,.drawer .section h3{color:#9b9d9a}.drawer .input{background:#18191a;border-color:#2b2c2e;color:#f7f7f5}.drawer .btn{background:#18191a;border-color:#2b2c2e;color:#f7f7f5}.drawer .btn.primary{background:#f7f7f5;color:#101112;border-color:#f7f7f5}.drawer .btn.danger{color:#ff9b9b;border-color:#5b3030}.drawer .tab{background:#18191a;border-color:#2b2c2e;color:#f7f7f5}.drawer .tab.active{background:#f7f7f5;color:#101112;border-color:#f7f7f5}.drawer .notice{background:#18191a;border-color:#2b2c2e;color:#9b9d9a}.section{margin-top:20px}.section h3{font-size:11px;margin:0 0 9px;color:var(--muted);font-weight:700;text-transform:uppercase;letter-spacing:.1em}.tabs{display:flex;gap:7px;margin:14px 0 4px}.tab{flex:1;padding:9px;border:1px solid var(--line);background:var(--soft);border-radius:10px}.tab.active{background:var(--ink);color:#fff;border-color:var(--ink)}.hidden{display:none!important}.statusmsg{min-height:18px;margin-top:7px}.kicker{margin:16px 0 10px;text-align:center;color:var(--muted);font-size:12px}@media(max-width:820px){.grid{grid-template-columns:repeat(2,1fr)}.wide{grid-column:span 2}}@media(max-width:520px){.shell{padding:12px 12px 30px}.grid{grid-template-columns:1fr}.wide{grid-column:span 1}.value{font-size:21px}}\n"
"</style></head><body><div class=\"shell\"><div class=\"top\"><button class=\"menu\" id=\"menuBtn\" onclick=\"openDrawer()\" aria-label=\"Settings\">☰</button><div class=\"brand\"><div class=\"mark\">R</div><div><div class=\"title\">Router</div><div class=\"sub\" id=\"mode\">Local management</div></div></div><div class=\"pill\" id=\"authState\">Public</div></div><main id=\"dash\"><div class=\"grid\"><div class=\"card\"><div class=\"label\">Uplink</div><div class=\"row\"><div class=\"value\" id=\"staState\">—</div><span id=\"staPill\" class=\"pill\">●</span></div></div><div class=\"card\"><div class=\"label\">Signal</div><div class=\"value\"><span id=\"rssi\">—</span> dBm</div><div class=\"meter\"><i id=\"sigbar\"></i></div></div><div class=\"card\"><div class=\"label\">Clients</div><div class=\"value\" id=\"clients\">—</div></div><div class=\"card\"><div class=\"label\">Router uptime</div><div class=\"value\" id=\"uptime\">—</div></div><div class=\"card wide\"><div class=\"label\">STA network</div><table class=\"table\"><tr><td>SSID</td><td id=\"ssid\">—</td></tr><tr><td>IP</td><td id=\"ip\">—</td></tr><tr><td>Channel</td><td id=\"channel\">—</td></tr><tr><td>Estimated distance</td><td id=\"distance\">—</td></tr></table></div><div class=\"card private hidden\"><div class=\"label\">Internet traffic</div><div class=\"row\"><div><div class=\"sub\">Download</div><div class=\"value\" id=\"down\">—</div></div><div><div class=\"sub\">Upload</div><div class=\"value\" id=\"up\">—</div></div><div><div class=\"sub\">Total</div><div class=\"value\" id=\"total\">—</div></div></div></div><div class=\"card private hidden\"><div class=\"label\">Device</div><table class=\"table\"><tr><td>Temperature</td><td id=\"temp\">—</td></tr><tr><td>Free heap</td><td id=\"heap\">—</td></tr><tr><td>Minimum heap</td><td id=\"minheap\">—</td></tr><tr><td>CPU load</td><td id=\"cpu\">—</td></tr><tr><td>CPU0 / CPU1</td><td id=\"cpu12\">—</td></tr><tr><td>PSRAM free</td><td id=\"psram\">—</td></tr><tr><td>Performance</td><td id=\"perfstat\">—</td></tr></table></div><div class=\"card wide private hidden\"><div class=\"label\">Uplink details</div><table class=\"table\"><tr><td>Gateway</td><td id=\"gw\">—</td></tr><tr><td>DNS</td><td id=\"dns\">—</td></tr><tr><td>Link uptime</td><td id=\"linkup\">—</td></tr><tr><td>NAT</td><td id=\"napt\">—</td></tr><tr><td>State</td><td id=\"reason\">—</td></tr></table></div></div><section id=\"authPanel\" class=\"authbox\"><div id=\"setup\" class=\"card hidden\"><div class=\"title\">Create administrator password</div><div class=\"sub\">Set the management password once.</div><div class=\"inputwrap\"><input id=\"sp1\" class=\"input\" type=\"password\" autocomplete=\"new-password\" placeholder=\"New password\"><button class=\"eye\" onclick=\"toggleOne('sp1')\" aria-label=\"Show password\">◉</button></div><div class=\"inputwrap\"><input id=\"sp2\" class=\"input\" type=\"password\" autocomplete=\"new-password\" placeholder=\"Confirm password\"><button class=\"eye\" onclick=\"toggleOne('sp2')\" aria-label=\"Show password\">◉</button></div><div class=\"row\"><span class=\"sub\">8–63 characters</span><button class=\"btn primary\" onclick=\"setupPass()\">Create</button></div><div id=\"setupMsg\" class=\"sub statusmsg\"></div></div><div id=\"login\" class=\"card hidden\"><div class=\"title\">Administrator login</div><div class=\"sub\">Login unlocks configuration, diagnostics and OTA.</div><div class=\"inputwrap\"><input id=\"lp\" class=\"input\" type=\"password\" autocomplete=\"current-password\" placeholder=\"Password\"><button class=\"eye\" onclick=\"toggleOne('lp')\" aria-label=\"Show password\">◉</button></div><div class=\"row\"><span class=\"sub\">Local management only</span><button class=\"btn primary\" onclick=\"login()\">Login</button></div><div id=\"loginMsg\" class=\"sub statusmsg\"></div></div></section></main></div><div id=\"drawer\" class=\"overlay\" onclick=\"if(event.target===this)closeDrawer()\"><div class=\"drawer\"><div class=\"row\"><div><div class=\"title\">Router control</div><div class=\"sub\">Configuration and maintenance</div></div><button class=\"menu\" onclick=\"closeDrawer()\" aria-label=\"Close\">×</button></div><div class=\"tabs\"><button class=\"tab active\" id=\"tabConfig\" onclick=\"showTab('config')\">Settings</button><button class=\"tab\" id=\"tabSecurity\" onclick=\"showTab('security')\">Security</button></div><div id=\"configTab\"><div class=\"section\"><h3>Access point</h3><input id=\"apssid\" class=\"input\" placeholder=\"SSID\"><input id=\"appass\" class=\"input\" type=\"password\" placeholder=\"Password (8–63 chars)\"><input id=\"apch\" class=\"input\" type=\"number\" min=\"1\" max=\"13\" placeholder=\"Channel\"><button class=\"btn primary\" onclick=\"saveAP()\">Apply AP</button><div id=\"apmsg\" class=\"sub statusmsg\"></div></div><div class=\"section\"><h3>STA uplink</h3><input id=\"stassid\" class=\"input\" placeholder=\"Wi-Fi SSID\"><input id=\"stapass\" class=\"input\" type=\"password\" placeholder=\"Wi-Fi password\"><button class=\"btn primary\" onclick=\"saveSTA()\">Connect</button><div id=\"stamsg\" class=\"sub statusmsg\"></div></div><div class=\"section\"><h3>OTA update</h3><input id=\"otapass\" class=\"input\" type=\"password\" placeholder=\"OTA password\"><input id=\"fw\" class=\"input\" type=\"file\" accept=\".bin\"><button class=\"btn primary\" onclick=\"ota()\">Upload firmware</button><div id=\"otamsg\" class=\"sub statusmsg\"></div></div></div><div id=\"securityTab\" class=\"hidden\"><div class=\"section\"><h3>Administrator password</h3><input id=\"oldp\" class=\"input\" type=\"password\" placeholder=\"Old password\"><input id=\"newp\" class=\"input\" type=\"password\" placeholder=\"New password\"><input id=\"newp2\" class=\"input\" type=\"password\" placeholder=\"Confirm new password\"><button class=\"btn primary\" onclick=\"changePass()\">Change password</button><div id=\"passmsg\" class=\"sub statusmsg\"></div></div><div class=\"section\"><h3>OTA password</h3><input id=\"otao\" class=\"input\" type=\"password\" placeholder=\"Old OTA password\"><input id=\"otan\" class=\"input\" type=\"password\" placeholder=\"New OTA password\"><input id=\"otan2\" class=\"input\" type=\"password\" placeholder=\"Confirm new OTA password\"><button class=\"btn primary\" onclick=\"changeOtaPass()\">Change OTA password</button><div id=\"otapmsg\" class=\"sub statusmsg\"></div></div><div class=\"section\"><h3>Performance</h3><div class=\"notice\">100% means performance-first: maximum TX power and Wi-Fi power-save disabled. Lower values trade responsiveness for lower power.</div><input id=\"perf\" class=\"input\" type=\"range\" min=\"10\" max=\"100\" step=\"5\" value=\"100\" oninput=\"$('perfv').textContent=this.value+'%'\"><div class=\"row\"><span class=\"sub\">Efficiency profile</span><strong id=\"perfv\">100%</strong></div><button class=\"btn primary\" onclick=\"savePerf()\">Apply performance</button><div id=\"perfmsg\" class=\"sub statusmsg\"></div></div><div class=\"section\"><h3>Diagnostics</h3><button class=\"btn\" onclick=\"loadLogs()\">View logs</button><button class=\"btn\" onclick=\"clearLogs()\">Clear logs</button><pre id=\"logs\" class=\"notice\" style=\"max-height:220px;overflow:auto;white-space:pre-wrap;margin-top:8px\"></pre></div><div class=\"section\"><h3>Maintenance</h3><div class=\"actions\"><button class=\"btn\" onclick=\"restart()\">Restart</button><button class=\"btn danger\" onclick=\"resetAll()\">Factory reset</button><button class=\"btn\" onclick=\"logout()\">Logout</button></div></div><div class=\"section\"><div class=\"notice\">Management is available at 192.168.4.1. Automatic redirection is used only while the uplink is unavailable.</div></div></div></div></div><script>\n"
"const $=id=>document.getElementById(id);\n"
"let timer=0;\n"
"async function api(u,o){const r=await fetch(u,o);let x={};try{x=await r.json()}catch(e){}return x}\n"
"function toggleOne(id){const e=$(id);e.type=e.type==='password'?'text':'password'}\n"
"function openDrawer(){if($('authState').textContent!=='Admin')return; $('drawer').style.display='flex';loadConfig()}\n"
"function closeDrawer(){$('drawer').style.display='none'}\n"
"function showTab(t){$('configTab').classList.toggle('hidden',t!=='config');$('securityTab').classList.toggle('hidden',t!=='security');$('tabConfig').classList.toggle('active',t==='config');$('tabSecurity').classList.toggle('active',t==='security')}\n"
"function fmt(x){x=Math.max(0,Math.floor(x||0));const d=Math.floor(x/86400),h=Math.floor(x%86400/3600),m=Math.floor(x%3600/60),s=x%60;return(d?d+'d ':'')+h+'h '+m+'m '+s+'s'}\n"
"function bytes(n){n=Number(n)||0;if(n<1024)return n+' B';const u=['KB','MB','GB','TB'],i=Math.min(u.length-1,Math.floor(Math.log(n)/Math.log(1024))-0);return(n/Math.pow(1024,i+1)).toFixed(i>1?2:1)+' '+u[i]}\n"
"function kb(n){return((Number(n)||0)/1024).toFixed(0)+' KB'}\n"
"function setPrivate(v){document.querySelectorAll('.private').forEach(e=>e.classList.toggle('hidden',!v));$('authState').textContent=v?'Admin':'Public'}\n"
"async function boot(){const s=await api('/api/auth');$('setup').classList.toggle('hidden',!s.setup);$('login').classList.toggle('hidden',s.setup||s.auth);setPrivate(!!s.auth);$('menuBtn').style.visibility=s.auth?'visible':'hidden';if(!timer){update();timer=setInterval(update,1200)}}\n"
"async function update(){try{const s=await api('/api/status');const online=!!s.sta.ip;$('staState').textContent=online?'Online':'Offline';$('rssi').textContent=s.sta.rssi<=-126?'—':s.sta.rssi;$('sigbar').style.width=(s.sta.signal||0)+'%';$('clients').textContent=s.ap.clients||'0';$('uptime').textContent=fmt(s.uptime);$('ssid').textContent=s.sta.ssid||'—';$('ip').textContent=s.sta.ip||'—';$('channel').textContent=s.sta.channel||'—';$('distance').textContent=s.sta.distance>0?s.sta.distance.toFixed(1)+' m':'—';$('mode').textContent=s.captive?'Configuration portal':'Local management';const good=online&&s.sta.signal>=70,warn=online&&s.sta.signal>=40;$('staPill').textContent=online?'● '+(good?'Good':warn?'Fair':'Weak'):'● Offline';$('staPill').style.color=good?'var(--good)':warn?'var(--warn)':'var(--bad)';if(s.auth){$('gw').textContent=s.sta.gw||'—';$('dns').textContent=s.sta.dns||'—';$('linkup').textContent=fmt(s.link_uptime);$('napt').textContent=s.napt?'Enabled':'Disabled';$('reason').textContent=s.reason||'—';$('temp').textContent=s.temp>=0?s.temp.toFixed(1)+' °C':'—';$('heap').textContent=kb(s.heap);$('minheap').textContent=kb(s.minheap);$('cpu').textContent=s.cpu_load+'%';$('cpu12').textContent=s.cpu0+'% / '+s.cpu1+'%';$('psram').textContent=kb(s.psram);$('perfstat').textContent=s.performance+'%';$('down').textContent=bytes(s.traffic.down);$('up').textContent=bytes(s.traffic.up);$('total').textContent=bytes((Number(s.traffic.down)||0)+(Number(s.traffic.up)||0))}}catch(e){}}\n"
"async function setupPass(){const a=$('sp1').value,b=$('sp2').value;if(a.length<8||a.length>63)return $('setupMsg').textContent='Password must be 8–63 characters';if(a!==b)return $('setupMsg').textContent='Passwords do not match';const r=await api('/api/setup-password',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({password:a,confirm:b})});$('setupMsg').textContent=r.message||'Unable to create password';if(r.ok)setTimeout(boot,500)}\n"
"async function login(){const r=await api('/api/login',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({password:$('lp').value})});$('loginMsg').textContent=r.message||'Login failed';if(r.ok){$('lp').value='';boot()}}\n"
"async function logout(){await api('/api/logout',{method:'POST'});closeDrawer();boot()}\n"
"async function loadConfig(){const c=await api('/api/config');if(!c.ap)return;$('apssid').value=c.ap.ssid||'';$('apch').value=c.ap.channel||1;$('stassid').value=c.sta.ssid||''}\n"
"async function saveAP(){const r=await api('/api/config/ap',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:$('apssid').value,password:$('appass').value,channel:+$('apch').value})});$('apmsg').textContent=r.message||'Unable to apply';if(r.ok)await loadConfig()}\n"
"async function saveSTA(){const r=await api('/api/config/sta',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:$('stassid').value,password:$('stapass').value})});$('stamsg').textContent=r.message||'Unable to connect'}\n"
"async function ota(){const f=$('fw').files[0];if(!f)return $('otamsg').textContent='Select a firmware image';$('otamsg').textContent='Uploading…';const r=await fetch('/api/ota',{method:'POST',headers:{'Content-Type':'application/octet-stream','X-OTA-Password':$('otapass').value},body:f});let x={};try{x=await r.json()}catch(e){}$('otamsg').textContent=x.message||'OTA failed'}\n"
"async function changePass(){const r=await api('/api/change-password',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({old:$('oldp').value,new:$('newp').value,confirm:$('newp2').value})});$('passmsg').textContent=r.message||'Unable to change password';if(r.ok){$('oldp').value='';$('newp').value='';$('newp2').value=''}}\n"
"async function changeOtaPass(){const r=await api('/api/change-ota-password',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({old:$('otao').value,new:$('otan').value,confirm:$('otan2').value})});$('otapmsg').textContent=r.message||'Unable to change OTA password';if(r.ok){$('otao').value='';$('otan').value='';$('otan2').value=''}}\n"
"async function restart(){await api('/api/restart',{method:'POST'});$('passmsg').textContent='Restarting…'}\n"
"async function resetAll(){if(!confirm('Erase all router settings and restart?'))return;await api('/api/reset',{method:'POST'});$('passmsg').textContent='Factory reset…'}\n"
"async function savePerf(){const r=await api('/api/performance',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({percent:+$('perf').value})});$('perfmsg').textContent=r.message||'Unable to apply';}\n"
"async function loadLogs(){const r=await api('/api/logs');$('logs').textContent=r.logs||'No logs';}\n"
"async function clearLogs(){const r=await api('/api/logs/clear',{method:'POST'});$('logs').textContent=r.message||'';}\n"
"boot();\n"
"</script></body></html>";

static esp_err_t json_reply(httpd_req_t *req, const char *json, const char *status)
{
    httpd_resp_set_type(req, "application/json");
    if (status) httpd_resp_set_status(req, status);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static bool nvs_blob_exists(const char *key)
{
    nvs_handle_t h; size_t n = 0; bool ok = false;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) { ok = nvs_get_blob(h,key,NULL,&n)==ESP_OK && n > 0; nvs_close(h); }
    return ok;
}

static bool hash_password(const uint8_t *salt, size_t salt_len, const char *pass, uint8_t out[32])
{
    size_t pass_len = strlen(pass);
    if (salt_len > 16 || pass_len > 63) return false;

    uint8_t input[79];
    memcpy(input, salt, salt_len);
    memcpy(input + salt_len, pass, pass_len);

    if (psa_crypto_init() != PSA_SUCCESS) return false;

    size_t output_len = 0;
    psa_status_t status = psa_hash_compute(
        PSA_ALG_SHA_256,
        input,
        salt_len + pass_len,
        out,
        32,
        &output_len
    );

    memset(input, 0, sizeof(input));
    return status == PSA_SUCCESS && output_len == 32;
}

static bool constant_eq(const uint8_t *a,const uint8_t*b,size_t n){uint8_t x=0;for(size_t i=0;i<n;i++)x|=a[i]^b[i];return x==0;}

static bool verify_password(const char *key_hash, const char *key_salt, const char *pass)
{
    uint8_t salt[16], stored[32], got[32]; size_t sl=sizeof(salt), hl=sizeof(stored); nvs_handle_t h;
    if(nvs_open(NS,NVS_READONLY,&h)!=ESP_OK)return false;
    bool ok=nvs_get_blob(h,key_salt,salt,&sl)==ESP_OK && nvs_get_blob(h,key_hash,stored,&hl)==ESP_OK; nvs_close(h);
    if(!ok||sl!=16||hl!=32)return false;
    if(!hash_password(salt,16,pass,got))return false;
    return constant_eq(stored,got,32);
}

static bool set_password(const char *key_hash,const char *key_salt,const char *pass)
{
    if (!pass || strlen(pass) < 8 || strlen(pass) > 63) {
        return false;
    }
    uint8_t salt[16];
    uint8_t hash[32];
    esp_fill_random(salt, sizeof(salt));
    if (!hash_password(salt, sizeof(salt), pass, hash)) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t e = nvs_set_blob(h, key_salt, salt, sizeof(salt));
    if (e == ESP_OK) {
        e = nvs_set_blob(h, key_hash, hash, sizeof(hash));
    }
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    return e == ESP_OK;
}

static void new_session(void)
{
    uint8_t t[32]; esp_fill_random(t,sizeof(t)); for(int i=0;i<32;i++)snprintf(&s_session[i*2],3,"%02x",t[i]); s_session[64]=0; s_session_expiry=esp_timer_get_time()+SESSION_TTL_US;
}
static bool authenticated(httpd_req_t *req)
{
    char cookie[100]; if(httpd_req_get_hdr_value_str(req,"Cookie",cookie,sizeof(cookie))!=ESP_OK)return false; const char *p=strstr(cookie,"ESPSESSID="); if(!p)return false; p+=10; char tok[65]={0}; for(int i=0;i<64&&p[i]&&p[i]!=';' ;i++)tok[i]=p[i]; if(esp_timer_get_time()>s_session_expiry)return false; return strlen(s_session)==64&&constant_eq((const uint8_t*)tok,(const uint8_t*)s_session,64);
}
static void require_auth(httpd_req_t *req){json_reply(req,"{\"ok\":false,\"message\":\"Authentication required\"}","401 Unauthorized");}

static int read_body(httpd_req_t *req, char *buf, size_t cap)
{
    if (!req || !buf || req->content_len <= 0 || req->content_len >= cap) return -1;
    size_t total = 0;
    while (total < req->content_len) {
        int n = httpd_req_recv(req, buf + total, req->content_len - total);
        if (n <= 0) return -1;
        total += (size_t)n;
    }
    buf[total] = 0;
    return (int)total;
}
static const char *json_str(const char *body,const char *key,char *out,size_t cap)
{
    char needle[40];snprintf(needle,sizeof(needle),"\"%s\"",key);const char*p=strstr(body,needle);if(!p)return NULL;p=strchr(p,':');if(!p)return NULL;p++;while(*p&&isspace((unsigned char)*p))p++;if(*p!='\"')return NULL;p++;size_t i=0;while(*p&&*p!='\"'&&i+1<cap){if(*p=='\\'&&p[1])p++;out[i++]=*p++;}out[i]=0;return out;
}
static int json_int(const char *body,const char *key,int fallback){
    char needle[40]; snprintf(needle,sizeof(needle),"\"%s\"",key); const char *p=strstr(body,needle); if(!p)return fallback;
    p=strchr(p,':'); if(!p)return fallback; p++;
    while(*p==' '||*p=='\t') p++;
    char *end=NULL; long v=strtol(p,&end,10); if(end==p||v<INT_MIN||v>INT_MAX)return fallback; return (int)v;
}

static void json_escape(const char *in, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    size_t j = 0;
    if (!in) in = "";
    for (size_t i = 0; in[i] && j + 1 < cap; ++i) {
        unsigned char c = (unsigned char)in[i];
        if ((c == '"' || c == '\\') && j + 2 < cap) {
            out[j++] = '\\';
            out[j++] = (char)c;
        } else if (c >= 32 && c != 127) {
            out[j++] = (char)c;
        } else if (j + 6 < cap) {
            snprintf(out + j, cap - j, "\\u%04x", c);
            j += 6;
        }
    }
    out[j] = 0;
}

static void get_ipv4(esp_netif_t *n, char *out, size_t cap){esp_netif_ip_info_t i={0};if(n&&esp_netif_get_ip_info(n,&i)==ESP_OK)snprintf(out,cap,IPSTR,IP2STR(&i.ip));else strlcpy(out,"",cap);}

static uint64_t counter_for_ip(struct netif *n, bool out)
{
#if MIB2_STATS
    if (n != NULL) {
        return (uint64_t)(out ? n->mib2_counters.ifoutoctets
                              : n->mib2_counters.ifinoctets);
    }
#else
    (void)n;
    (void)out;
#endif
    return 0;
}
static struct netif *find_netif(esp_netif_t *en)
{
    if (!en) {
        return NULL;
    }
    char name[6] = {0};
    if (esp_netif_get_netif_impl_name(en, name) != ESP_OK) {
        return NULL;
    }
    struct netif *n=netif_list; while(n){if(n->name[0]==name[0]&&n->name[1]==name[1])return n;n=n->next;} return NULL;
}
static uint64_t s_down_base,s_up_base;
static uint64_t traffic_down(void){struct netif *sta=find_netif(router_get_sta_netif());return counter_for_ip(sta,false)+s_down_base;}
static uint64_t traffic_up(void){struct netif *sta=find_netif(router_get_sta_netif());return counter_for_ip(sta,true)+s_up_base;}

static esp_err_t index_get(httpd_req_t *req){
    bool captive=!router_sta_has_ip();
    if(captive && strcmp(req->uri,"/")!=0){httpd_resp_set_status(req,"302 Found");httpd_resp_set_hdr(req,"Location","/");httpd_resp_sendstr(req,"redirect");return ESP_OK;}
    httpd_resp_set_type(req,"text/html; charset=utf-8");httpd_resp_set_hdr(req,"Cache-Control","no-store");return httpd_resp_send(req,INDEX_HTML,HTTPD_RESP_USE_STRLEN);
}
static esp_err_t auth_get(httpd_req_t *req){char j[96];snprintf(j,sizeof(j),"{\"setup\":%s,\"auth\":%s}",nvs_blob_exists(KEY_AUTH_HASH)?"false":"true",authenticated(req)?"true":"false");json_reply(req,j,NULL);return ESP_OK;}
static esp_err_t setup_post(httpd_req_t *req)
{
    char b[512], p[80], c[80];
    if (read_body(req, b, sizeof(b)) < 0 || !json_str(b, "password", p, sizeof(p)) || !json_str(b, "confirm", c, sizeof(c)))
        return json_reply(req, "{\"ok\":false,\"message\":\"Invalid request\"}", "400 Bad Request");
    if (nvs_blob_exists(KEY_AUTH_HASH))
        return json_reply(req, "{\"ok\":false,\"message\":\"Already configured\"}", "409 Conflict");
    if (strcmp(p, c) != 0)
        return json_reply(req, "{\"ok\":false,\"message\":\"Passwords do not match\"}", "400 Bad Request");
    if (!set_password(KEY_AUTH_HASH, KEY_AUTH_SALT, p) || !set_password(KEY_OTA_HASH, KEY_OTA_SALT, p)) {
        nvs_handle_t h;
        if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_key(h, KEY_AUTH_HASH);
            nvs_erase_key(h, KEY_AUTH_SALT);
            nvs_erase_key(h, KEY_OTA_HASH);
            nvs_erase_key(h, KEY_OTA_SALT);
            nvs_commit(h);
            nvs_close(h);
        }
        return json_reply(req, "{\"ok\":false,\"message\":\"Password setup failed\"}", "500 Internal Server Error");
    }
    json_reply(req, "{\"ok\":true,\"message\":\"Password created. Login to continue.\"}", NULL);
    return ESP_OK;
}static esp_err_t login_post(httpd_req_t *req)
{
    if (esp_timer_get_time() < s_login_lock_until)
        return json_reply(req, "{\"ok\":false,\"message\":\"Try again shortly\"}", "429 Too Many Requests");
    char b[512], p[80];
    if (read_body(req, b, sizeof(b)) < 0 || !json_str(b, "password", p, sizeof(p)))
        return json_reply(req, "{\"ok\":false,\"message\":\"Invalid request\"}", "400 Bad Request");
    if (!verify_password(KEY_AUTH_HASH, KEY_AUTH_SALT, p)) {
        if (++s_login_failures >= 5) {
            s_login_failures = 0;
            s_login_lock_until = esp_timer_get_time() + 5000000LL;
        }
        return json_reply(req, "{\"ok\":false,\"message\":\"Invalid password\"}", "401 Unauthorized");
    }
    s_login_failures = 0;
    s_login_lock_until = 0;
    new_session();
    char c[128];
    snprintf(c, sizeof(c), "ESPSESSID=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=1800", s_session);
    httpd_resp_set_hdr(req, "Set-Cookie", c);
    json_reply(req, "{\"ok\":true,\"message\":\"Logged in\"}", NULL);
    return ESP_OK;
}static esp_err_t logout_post(httpd_req_t *req)
{
    memset(s_session, 0, sizeof(s_session));
    s_session_expiry = 0;
    httpd_resp_set_hdr(req, "Set-Cookie", "ESPSESSID=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    json_reply(req, "{\"ok\":true}", NULL);
    return ESP_OK;
}

static esp_err_t status_get(httpd_req_t *req)
{
    bool auth = authenticated(req);
    char ip[20] = "", gw[20] = "", dns[20] = "", ssid[33] = "", ssid_json[100] = "";
    get_ipv4(router_get_sta_netif(), ip, sizeof(ip));
    esp_netif_ip_info_t ii = {0};
    if (esp_netif_get_ip_info(router_get_sta_netif(), &ii) == ESP_OK) {
        snprintf(gw, sizeof(gw), IPSTR, IP2STR(&ii.gw));
    }
    router_get_sta_config(ssid, sizeof(ssid), NULL, 0);
    json_escape(ssid, ssid_json, sizeof(ssid_json));
    esp_netif_dns_info_t di = {0};
    if (esp_netif_get_dns_info(router_get_sta_netif(), ESP_NETIF_DNS_MAIN, &di) == ESP_OK && di.ip.type == ESP_IPADDR_TYPE_V4)
        snprintf(dns, sizeof(dns), IPSTR, IP2STR(&di.ip.u_addr.ip4));
    int rssi = router_sta_rssi();
    uint8_t signal = router_sta_signal_percent();
    uint8_t channel = 0;
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) channel = ap.primary;
    if (!auth) {
        char j[700];
        snprintf(j, sizeof(j),
            "{\"auth\":false,\"captive\":%s,\"uptime\":%u,\"sta\":{\"ip\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,\"signal\":%u,\"distance\":%.2f,\"channel\":%u},\"ap\":{\"clients\":%u}}",
            !router_sta_has_ip() ? "true" : "false",
            (unsigned)(esp_timer_get_time() / 1000000ULL),
            ip, ssid_json, rssi, (unsigned)signal, router_sta_distance_m(),
            (unsigned)channel, (unsigned)router_ap_client_count());
        json_reply(req, j, NULL);
        return ESP_OK;
    }
    char j[1400];
    snprintf(j, sizeof(j),
        "{\"auth\":true,\"captive\":%s,\"uptime\":%u,\"link_uptime\":%u,\"heap\":%u,\"minheap\":%u,\"cpu_load\":%u,\"cpu0\":%u,\"cpu1\":%u,\"psram\":%u,\"performance\":%u,\"temp\":%.1f,\"napt\":%s,\"reason\":\"%s\",\"ap\":{\"clients\":%u},\"sta\":{\"ip\":\"%s\",\"gw\":\"%s\",\"dns\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,\"signal\":%u,\"distance\":%.2f,\"channel\":%u},\"traffic\":{\"down\":%llu,\"up\":%llu}}",
        !router_sta_has_ip() ? "true" : "false",
        (unsigned)(esp_timer_get_time() / 1000000ULL),
        (unsigned)router_get_sta_uptime_s(),
        (unsigned)system_metrics_free_heap(),
        (unsigned)system_metrics_min_heap(),
        (unsigned)system_metrics_cpu_load(),
        (unsigned)system_metrics_cpu0_load(),
        (unsigned)system_metrics_cpu1_load(),
        (unsigned)system_metrics_free_psram(),
        (unsigned)router_get_performance(),
        system_metrics_temperature(),
        router_napt_enabled() ? "true" : "false",
        router_get_last_disconnect_reason(),
        (unsigned)router_ap_client_count(),
        ip, gw, dns, ssid_json, rssi, (unsigned)signal,
        router_sta_distance_m(), (unsigned)channel,
        (unsigned long long)traffic_down(),
        (unsigned long long)traffic_up());
    json_reply(req, j, NULL);
    return ESP_OK;
}

static esp_err_t config_get(httpd_req_t *req)
{
    if (!authenticated(req)) return (require_auth(req), ESP_OK);
    char as[33], ap[65], ss[33], ae[100], se[100];
    uint8_t ch;
    router_get_ap_config(as, sizeof(as), ap, sizeof(ap), &ch);
    router_get_sta_config(ss, sizeof(ss), NULL, 0);
    json_escape(as, ae, sizeof(ae));
    json_escape(ss, se, sizeof(se));
    char j[320];
    snprintf(j, sizeof(j), "{\"ap\":{\"ssid\":\"%s\",\"channel\":%u},\"sta\":{\"ssid\":\"%s\"}}", ae, (unsigned)ch, se);
    json_reply(req, j, NULL);
    return ESP_OK;
}
static esp_err_t ap_post(httpd_req_t *req)
{
    if (!authenticated(req)) {
        require_auth(req);
        return ESP_OK;
    }

    char b[512], s[40], p[80];
    if (read_body(req, b, sizeof(b)) < 0 ||
        !json_str(b, "ssid", s, sizeof(s)) ||
        !json_str(b, "password", p, sizeof(p))) {
        return json_reply(req, "{\"ok\":false,\"message\":\"Invalid AP settings\"}",
                          "400 Bad Request");
    }

    int ch = json_int(b, "channel", 1);
    if (router_set_ap_config(s, p, (uint8_t)ch) != ESP_OK) {
        return json_reply(req, "{\"ok\":false,\"message\":\"Invalid AP settings\"}",
                          "400 Bad Request");
    }

    return json_reply(req, "{\"ok\":true,\"message\":\"AP settings applied\"}", NULL);
}

static esp_err_t sta_post(httpd_req_t *req)
{
    if (!authenticated(req)) {
        require_auth(req);
        return ESP_OK;
    }

    char b[512], s[40], p[80];
    if (read_body(req, b, sizeof(b)) < 0 ||
        !json_str(b, "ssid", s, sizeof(s)) ||
        !json_str(b, "password", p, sizeof(p))) {
        return json_reply(req, "{\"ok\":false,\"message\":\"Invalid STA settings\"}",
                          "400 Bad Request");
    }

    esp_err_t e = router_set_sta_config(s, p);
    if (e != ESP_OK) {
        return json_reply(req, "{\"ok\":false,\"message\":\"Could not apply STA settings\"}",
                          "400 Bad Request");
    }

    return json_reply(req, "{\"ok\":true,\"message\":\"STA settings applied\"}", NULL);
}

static esp_err_t change_pass(httpd_req_t *req)
{
    if (!authenticated(req)) {
        require_auth(req);
        return ESP_OK;
    }

    char b[512], o[80], n[80], c[80];
    if (read_body(req, b, sizeof(b)) < 0 ||
        !json_str(b, "old", o, sizeof(o)) ||
        !json_str(b, "new", n, sizeof(n)) ||
        !json_str(b, "confirm", c, sizeof(c))) {
        return json_reply(req, "{\"ok\":false,\"message\":\"Invalid request\"}",
                          "400 Bad Request");
    }

    if (strcmp(n, c) != 0 ||
        !verify_password(KEY_AUTH_HASH, KEY_AUTH_SALT, o) ||
        !set_password(KEY_AUTH_HASH, KEY_AUTH_SALT, n)) {
        return json_reply(req, "{\"ok\":false,\"message\":\"Password change failed\"}",
                          "400 Bad Request");
    }

    return json_reply(req, "{\"ok\":true,\"message\":\"Admin password changed\"}", NULL);
}

static esp_err_t change_ota_pass(httpd_req_t *req)
{
    if (!authenticated(req)) {
        require_auth(req);
        return ESP_OK;
    }

    char b[512], o[80], n[80], c[80];
    if (read_body(req, b, sizeof(b)) < 0 ||
        !json_str(b, "old", o, sizeof(o)) ||
        !json_str(b, "new", n, sizeof(n)) ||
        !json_str(b, "confirm", c, sizeof(c))) {
        return json_reply(req, "{\"ok\":false,\"message\":\"Invalid request\"}",
                          "400 Bad Request");
    }

    if (strcmp(n, c) != 0 ||
        !verify_password(KEY_OTA_HASH, KEY_OTA_SALT, o) ||
        !set_password(KEY_OTA_HASH, KEY_OTA_SALT, n)) {
        return json_reply(req, "{\"ok\":false,\"message\":\"OTA password change failed\"}",
                          "400 Bad Request");
    }

    return json_reply(req, "{\"ok\":true,\"message\":\"OTA password changed\"}", NULL);
}

static esp_err_t ota_post(httpd_req_t *req)
{
    if (!authenticated(req)) {
        require_auth(req);
        return ESP_OK;
    }

    char pass[80];
    if (httpd_req_get_hdr_value_str(req, "X-OTA-Password", pass, sizeof(pass)) != ESP_OK ||
        !verify_password(KEY_OTA_HASH, KEY_OTA_SALT, pass)) {
        return json_reply(req, "{\"ok\":false,\"message\":\"Invalid OTA password\"}",
                          "403 Forbidden");
    }

    if (req->content_len <= 0) {
        return json_reply(req, "{\"ok\":false,\"message\":\"Empty firmware\"}",
                          "400 Bad Request");
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || req->content_len > (int)part->size) {
        return json_reply(req, "{\"ok\":false,\"message\":\"Firmware is too large\"}",
                          "400 Bad Request");
    }

    esp_ota_handle_t h = 0;
    esp_err_t e = esp_ota_begin(part, req->content_len, &h);
    if (e != ESP_OK) {
        return json_reply(req, "{\"ok\":false,\"message\":\"OTA begin failed\"}",
                          "500 Internal Server Error");
    }

    uint8_t *buf = malloc(8192);
    if (!buf) {
        esp_ota_abort(h);
        return json_reply(req, "{\"ok\":false,\"message\":\"Not enough memory\"}",
                          "500 Internal Server Error");
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        int want = remaining > 8192 ? 8192 : remaining;
        int n = httpd_req_recv(req, (char *)buf, want);
        if (n <= 0) {
            e = ESP_FAIL;
            break;
        }

        e = esp_ota_write(h, buf, n);
        if (e != ESP_OK) {
            break;
        }

        remaining -= n;
    }

    free(buf);

    if (e == ESP_OK && remaining == 0) {
        e = esp_ota_end(h);
    } else {
        esp_ota_abort(h);
        if (e == ESP_OK) {
            e = ESP_FAIL;
        }
    }

    if (e == ESP_OK) {
        e = esp_ota_set_boot_partition(part);
    }

    if (e != ESP_OK) {
        return json_reply(req, "{\"ok\":false,\"message\":\"OTA verification failed\"}",
                          "500 Internal Server Error");
    }

    json_reply(req, "{\"ok\":true,\"message\":\"Firmware accepted. Restarting...\"}", NULL);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}
static esp_err_t restart_post(httpd_req_t *req){if(!authenticated(req))return(require_auth(req),ESP_OK);json_reply(req,"{\"ok\":true}",NULL);vTaskDelay(pdMS_TO_TICKS(250));esp_restart();return ESP_OK;}
static esp_err_t performance_post(httpd_req_t *req){
    if(!authenticated(req)) return(require_auth(req),ESP_OK);
    char b[128]; if(read_body(req,b,sizeof(b))<0) return json_reply(req,"{\"ok\":false,\"message\":\"Invalid request\"}","400 Bad Request");
    int p=json_int(b,"percent",100); if(p<10||p>100) return json_reply(req,"{\"ok\":false,\"message\":\"Percent must be 10-100\"}","400 Bad Request");
    if(router_set_performance((uint8_t)p)!=ESP_OK) return json_reply(req,"{\"ok\":false,\"message\":\"Performance change failed\"}","500 Internal Server Error");
    router_log_write("INFO","Performance profile changed"); return json_reply(req,"{\"ok\":true,\"message\":\"Performance profile applied\"}",NULL);
}
static esp_err_t logs_get(httpd_req_t *req){
    if(!authenticated(req)) return(require_auth(req),ESP_OK);
    char *buf=malloc(131072); if(!buf) return json_reply(req,"{\"ok\":false,\"message\":\"Not enough memory\"}","500 Internal Server Error");
    size_t n=router_log_read(buf,131072); char *esc=malloc(n*2+1); if(!esc){free(buf);return json_reply(req,"{\"ok\":false}","500 Internal Server Error");}
    json_escape(buf,esc,n*2+1); free(buf); char *out=malloc(n*2+32); if(!out){free(esc);return json_reply(req,"{\"ok\":false}","500 Internal Server Error");}
    snprintf(out,n*2+32,"{\"ok\":true,\"logs\":\"%s\"}",esc); free(esc); json_reply(req,out,NULL); free(out); return ESP_OK;
}
static esp_err_t logs_clear_post(httpd_req_t *req){if(!authenticated(req))return(require_auth(req),ESP_OK);router_log_clear();router_log_write("INFO","Logs cleared");return json_reply(req,"{\"ok\":true,\"message\":\"Logs cleared\"}",NULL);}

static esp_err_t reset_post(httpd_req_t *req){if(!authenticated(req))return(require_auth(req),ESP_OK);json_reply(req,"{\"ok\":true}",NULL);vTaskDelay(pdMS_TO_TICKS(250));router_factory_reset();return ESP_OK;}

static int dns_upstream_query(const uint8_t *query, int qlen, uint8_t *reply, int cap)
{
    esp_netif_dns_info_t di = {0};
    if (esp_netif_get_dns_info(router_get_sta_netif(), ESP_NETIF_DNS_MAIN, &di) != ESP_OK) return -1;
    if (di.ip.type != ESP_IPADDR_TYPE_V4) return -1;
    int us = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (us < 0) return -1;
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(us, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET; dst.sin_port = htons(53); dst.sin_addr.s_addr = di.ip.u_addr.ip4.addr;
    int n = sendto(us, query, qlen, 0, (struct sockaddr *)&dst, sizeof(dst));
    if (n != qlen) { close(us); return -1; }
    n = recvfrom(us, reply, cap, 0, NULL, NULL);
    close(us);
    return n;
}

static int dns_make_captive_response(const uint8_t *query, int qlen, uint8_t *resp, int cap)
{
    if (qlen < 12 || qlen > cap) return -1;
    int qend = 12;
    while (qend < qlen && query[qend] != 0) {
        int jump = query[qend];
        if (jump == 0 || jump > 63 || qend + jump + 1 >= qlen) return -1;
        qend += jump + 1;
    }
    if (qend + 5 > qlen) return -1;
    memcpy(resp, query, qlen);
    resp[2] |= 0x80; resp[3] = (uint8_t)((resp[3] & 0x70) | 0x80);
    resp[6] = 0; resp[7] = 1;
    int p = qlen;
    if (p + 16 > cap) return -1;
    resp[p++] = 0xC0; resp[p++] = 0x0C;
    resp[p++] = 0; resp[p++] = 1;
    resp[p++] = 0; resp[p++] = 1;
    resp[p++] = 0; resp[p++] = 0; resp[p++] = 0; resp[p++] = 30;
    resp[p++] = 0; resp[p++] = 4;
    resp[p++] = 192; resp[p++] = 168; resp[p++] = 4; resp[p++] = 1;
    return p;
}

static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { vTaskDelete(NULL); return; }
    struct sockaddr_in bind_addr = {0};
    bind_addr.sin_family = AF_INET; bind_addr.sin_port = htons(DNS_PORT); bind_addr.sin_addr.s_addr = inet_addr("192.168.4.1");
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) { close(sock); vTaskDelete(NULL); return; }
    uint8_t query[DNS_BUF], reply[DNS_BUF];
    for (;;) {
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        int n = recvfrom(sock, query, sizeof(query), 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;
        int out_len = -1;
        if (!router_sta_has_ip()) {
            out_len = dns_make_captive_response(query, n, reply, sizeof(reply));
        } else {
            out_len = dns_upstream_query(query, n, reply, sizeof(reply));
        }
        if (out_len > 0) sendto(sock, reply, out_len, 0, (struct sockaddr *)&from, fl);
    }
}

static void metrics_task(void *arg){(void)arg;for(;;){int r=router_sta_rssi();if(r<=-80){gpio_set_level(LED_RED,1);gpio_set_level(LED_YELLOW,0);gpio_set_level(LED_GREEN,0);}else if(r< -60){gpio_set_level(LED_RED,0);gpio_set_level(LED_YELLOW,1);gpio_set_level(LED_GREEN,0);}else{gpio_set_level(LED_RED,0);gpio_set_level(LED_YELLOW,0);gpio_set_level(LED_GREEN,1);}vTaskDelay(pdMS_TO_TICKS(1000));}}

static esp_err_t captive_probe(httpd_req_t *req){if(router_sta_has_ip()){return index_get(req);}httpd_resp_set_status(req,"302 Found");httpd_resp_set_hdr(req,"Location","/");httpd_resp_sendstr(req,"portal");return ESP_OK;}

static void register_uri(httpd_handle_t s,const char *u,httpd_method_t m,esp_err_t(*h)(httpd_req_t*)){httpd_uri_t x={.uri=u,.method=m,.handler=h,.user_ctx=NULL};ESP_ERROR_CHECK(httpd_register_uri_handler(s,&x));}

void web_server_start(void)
{
    gpio_config_t gc={.pin_bit_mask=(1ULL<<LED_RED)|(1ULL<<LED_YELLOW)|(1ULL<<LED_GREEN),.mode=GPIO_MODE_OUTPUT};gpio_config(&gc);gpio_set_level(LED_RED,0);gpio_set_level(LED_YELLOW,0);gpio_set_level(LED_GREEN,0);
    httpd_config_t c=HTTPD_DEFAULT_CONFIG();
    c.server_port=80;
    c.max_uri_handlers=32;
    c.max_open_sockets=8;
    c.stack_size=8192;
    c.core_id=1;
    c.task_priority=5;
    c.lru_purge_enable=true;
    c.uri_match_fn = httpd_uri_match_wildcard;
    c.recv_wait_timeout=5;
    c.send_wait_timeout=5;
    static struct ifreq ap_ifr;
    static char ap_ifname[6];
    memset(&ap_ifr, 0, sizeof(ap_ifr));
    memset(ap_ifname, 0, sizeof(ap_ifname));
    if (esp_netif_get_netif_impl_name(router_get_ap_netif(), ap_ifname) == ESP_OK) {
        strlcpy(ap_ifr.ifr_name, ap_ifname, sizeof(ap_ifr.ifr_name));
        c.if_name = &ap_ifr;
    }
    ESP_ERROR_CHECK(httpd_start(&s_http,&c));
    httpd_uri_t wild={0}; wild.uri="/*"; wild.method=HTTP_GET; wild.handler=captive_probe; ESP_ERROR_CHECK(httpd_register_uri_handler(s_http,&wild)); register_uri(s_http,"/",HTTP_GET,index_get);register_uri(s_http,"/api/auth",HTTP_GET,auth_get);register_uri(s_http,"/api/status",HTTP_GET,status_get);register_uri(s_http,"/api/config",HTTP_GET,config_get);register_uri(s_http,"/api/setup-password",HTTP_POST,setup_post);register_uri(s_http,"/api/login",HTTP_POST,login_post);register_uri(s_http,"/api/logout",HTTP_POST,logout_post);register_uri(s_http,"/api/config/ap",HTTP_POST,ap_post);register_uri(s_http,"/api/config/sta",HTTP_POST,sta_post);register_uri(s_http,"/api/change-password",HTTP_POST,change_pass);register_uri(s_http,"/api/change-ota-password",HTTP_POST,change_ota_pass);register_uri(s_http,"/api/ota",HTTP_POST,ota_post);register_uri(s_http,"/api/restart",HTTP_POST,restart_post);register_uri(s_http,"/api/reset",HTTP_POST,reset_post);register_uri(s_http,"/api/performance",HTTP_POST,performance_post);register_uri(s_http,"/api/logs",HTTP_GET,logs_get);register_uri(s_http,"/api/logs/clear",HTTP_POST,logs_clear_post);
    xTaskCreatePinnedToCore(dns_task,"dns",4096,NULL,4,NULL,1);xTaskCreatePinnedToCore(metrics_task,"metrics",4096,NULL,2,&s_metrics_task,1);
}
