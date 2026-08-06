#include "RemapWindow.h"
#include "ControllerManager.h"
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <string>
#include <cstring>

RemapWindow* RemapWindow::s_instance = nullptr;

// ---------------------------------------------------------------------------
// Embedded UI — self-contained HTML/CSS/JS matching the design handoff.
// Google Fonts are loaded at runtime; if offline, system-ui fallback applies.
// ---------------------------------------------------------------------------

static const std::wstring& GetHtml() {
    // Narrow literal (65 535 byte limit — much more than needed) avoids MSVC
    // C2026 "string too big" that fires at ~16 K for wide literals.
    // HTML is ASCII-only (JS glyphs use \uXXXX escapes); each byte widens 1:1.
    static const std::wstring html = []() -> std::wstring {
        static const char kHtml[] = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Barlow:wght@400;500;600;700;800&family=JetBrains+Mono:wght@600&display=swap" rel="stylesheet">
<style>
*,*::before,*::after{box-sizing:border-box;}
html,body{margin:0;padding:0;height:100%;overflow:hidden;}
body{font-family:'Barlow',system-ui,sans-serif;background:linear-gradient(180deg,#1b2838 0%,#15202c 100%);color:#fff;display:flex;flex-direction:column;height:100%;}
button{font-family:'Barlow',system-ui,sans-serif;cursor:pointer;border:none;background:none;}
@keyframes scPulse{0%,100%{opacity:.4;transform:scale(.85);}50%{opacity:1;transform:scale(1.2);}}
#titlebar{height:46px;flex:none;display:flex;align-items:center;justify-content:space-between;padding:0 6px 0 14px;background:#171a21;border-bottom:1px solid rgba(0,0,0,.5);user-select:none;}
#titlebar .left{display:flex;align-items:center;gap:10px;}
.glyph{width:20px;height:20px;border-radius:5px;background:linear-gradient(135deg,#1a9fff,#0a4a78);display:flex;align-items:center;justify-content:center;box-shadow:0 0 8px rgba(26,159,255,.4);flex:none;}
.glyph-dot{width:7px;height:7px;border-radius:50%;background:#bfe3ff;}
.tb-title{font-size:13px;font-weight:600;color:#c7d5e0;letter-spacing:.3px;}
.tb-sub{font-size:12px;color:#5c6b78;}
.winctls{display:flex;align-items:center;gap:2px;}
.winctl{width:30px;height:30px;display:flex;align-items:center;justify-content:center;border-radius:4px;color:#8f98a0;font-size:13px;cursor:pointer;transition:background .15s,color .15s;}
.winctl:hover{background:rgba(255,255,255,.08);}
.winctl.close:hover{background:#c0392b;color:#fff;}
#body{flex:1;display:flex;flex-direction:column;padding:24px 26px;gap:22px;overflow-y:auto;min-height:0;}
#body h2{font-size:21px;font-weight:800;color:#fff;letter-spacing:-.2px;}
.instr{font-size:13.5px;color:#8f98a0;line-height:1.5;margin-top:4px;}
.instr b{color:#66c0f4;font-weight:700;}
.group{display:flex;flex-direction:column;gap:11px;}
.group-label{font-family:'JetBrains Mono',monospace;font-size:11px;letter-spacing:1.5px;color:#5c6b78;font-weight:600;}
.row{display:flex;flex-direction:column;background:rgba(255,255,255,.025);border:1px solid rgba(255,255,255,.06);border-radius:8px;padding:12px 16px;transition:border-color .2s,box-shadow .2s,background .2s;}
.row.listening{background:rgba(255,255,255,.05);border-color:#66c0f4;box-shadow:0 0 0 1px rgba(102,192,244,.4),0 10px 26px rgba(0,0,0,.32);}
.row.flash{border-color:#5ba32b;box-shadow:0 0 0 1px rgba(91,163,43,.4);}
.row-top{display:flex;align-items:center;gap:16px;}
.badge{width:46px;height:46px;flex:none;border-radius:10px;background:#1b2a3a;border:1px solid rgba(255,255,255,.08);display:flex;flex-direction:column;align-items:center;justify-content:center;}
.badge-id{font-size:15px;font-weight:800;color:#fff;line-height:1;}
.badge-pos{font-family:'JetBrains Mono',monospace;font-size:8px;color:#5c6b78;letter-spacing:.5px;margin-top:2px;}
.pos-label{font-size:13.5px;color:#aeb9c2;font-weight:500;flex:none;}
.connector{flex:1;height:1.5px;border-top:1.5px dashed rgba(255,255,255,.1);}
.row-right{display:flex;align-items:center;gap:11px;flex:none;}
.chip-lg{width:32px;height:32px;border-radius:8px;display:flex;align-items:center;justify-content:center;font-weight:800;font-size:13px;flex:none;}
.input-label{font-size:13.5px;color:#fff;font-weight:600;min-width:100px;}
.btn-rebind{padding:7px 14px;border-radius:4px;border:1px solid rgba(255,255,255,.12);background:rgba(255,255,255,.04);color:#aeb9c2;font-size:12px;font-weight:600;transition:border-color .15s,color .15s;}
.btn-rebind:hover{border-color:#66c0f4;color:#66c0f4;}
.listen-right{display:flex;align-items:center;gap:8px;flex:none;}
.listen-pill{display:flex;align-items:center;gap:9px;background:rgba(102,192,244,.12);border:1px solid rgba(102,192,244,.45);color:#cfe7fa;border-radius:6px;padding:7px 12px;font-weight:600;font-size:12.5px;}
.pulse-dot{width:9px;height:9px;border-radius:50%;background:#66c0f4;animation:scPulse 1.2s ease-in-out infinite;flex:none;}
.btn-cancel{width:32px;height:32px;border-radius:5px;border:1px solid rgba(255,255,255,.1);background:rgba(255,255,255,.03);color:#8f98a0;font-size:12px;display:flex;align-items:center;justify-content:center;transition:border-color .15s,color .15s,background .15s;}
.btn-cancel:hover{border-color:#c0392b;color:#fff;background:rgba(192,57,43,.08);}
.btn-row-reset{padding:6px 12px;border-radius:5px;border:1px solid rgba(255,255,255,.1);background:rgba(255,255,255,.03);color:#8f98a0;font-size:12px;font-weight:600;transition:border-color .15s,color .15s,background .15s;}
.btn-row-reset:hover{border-color:#66c0f4;color:#66c0f4;background:rgba(102,192,244,.06);}
.picker{margin-top:12px;background:#101925;border:1px solid rgba(102,192,244,.2);border-radius:8px;padding:12px 13px;}
.picker-label{font-family:'JetBrains Mono',monospace;font-size:10.5px;letter-spacing:1px;color:#5c6b78;font-weight:600;}
.picker-rows{display:flex;flex-direction:column;gap:7px;margin-top:10px;}
.picker-row{display:flex;flex-wrap:wrap;gap:7px;}
.picker-pill{display:flex;align-items:center;gap:8px;padding:5px 11px 5px 5px;border-radius:7px;border:1px solid rgba(255,255,255,.08);background:rgba(255,255,255,.03);cursor:pointer;transition:border-color .15s,background .15s;}
.picker-pill:hover{border-color:#66c0f4;background:rgba(102,192,244,.1);}
.chip-sm{width:28px;height:28px;border-radius:6px;display:flex;align-items:center;justify-content:center;font-weight:800;font-size:11px;flex:none;}
.picker-name{font-size:12px;color:#c7d5e0;font-weight:600;}
#footer{height:62px;flex:none;display:flex;align-items:center;justify-content:space-between;padding:0 20px;background:#15202c;border-top:1px solid rgba(0,0,0,.45);}
.footer-right{display:flex;gap:10px;align-items:center;}
.btn-reset{padding:9px 16px;border-radius:3px;border:1px solid rgba(255,255,255,.12);background:rgba(255,255,255,.04);color:#aeb9c2;font-weight:600;font-size:13.5px;transition:color .15s;}
.btn-reset:hover{color:#fff;}
.btn-apply{padding:9px 22px;border-radius:3px;border:1px solid #4c7a1d;background:linear-gradient(to bottom,#94c63d,#5d8a1f);color:#13260a;font-weight:700;font-size:13.5px;box-shadow:0 1px 0 rgba(255,255,255,.25) inset;min-width:96px;text-align:center;transition:opacity .15s;}
.btn-apply:hover{opacity:.9;}
.btn-apply:disabled{opacity:.7;cursor:default;}
</style>
</head>
<body>

<div id="titlebar">
  <div class="left">
    <div class="glyph"><div class="glyph-dot"></div></div>
    <span class="tb-title">SteamlessController</span>
    <span class="tb-sub">&#8212; Back Button Mapping</span>
  </div>
  <div class="winctls">
    <button class="winctl" id="btn-min" title="Minimize">&#8211;</button>
    <button class="winctl close" id="btn-close" title="Close">&#10005;</button>
  </div>
</div>

<div id="body">
  <div>
    <h2>Back Button Mapping</h2>
    <p class="instr">Click <b>Rebind</b> on a button, then press any gamepad button on your controller. The new binding shows up here instantly. Pick <b>Off</b> to stop a paddle doing anything at all.</p>
  </div>
  <div class="group">
    <div class="group-label">LEFT GRIP</div>
    <div id="row-L4" class="row"></div>
    <div id="row-L5" class="row"></div>
  </div>
  <div class="group">
    <div class="group-label">RIGHT GRIP</div>
    <div id="row-R4" class="row"></div>
    <div id="row-R5" class="row"></div>
  </div>
</div>

<div id="footer">
  <div class="footer-right">
    <button class="btn-reset" id="btn-reset">Reset all to default</button>
    <button class="btn-apply" id="btn-apply">Apply</button>
  </div>
</div>

<script>
'use strict';
// ---- Defaults (upper paddles left-click, lower paddles unbound) ----
// Keep in sync with BackButtonConfig in BackButtonConfig.h.
var DEFAULTS = {L4:'leftMouse',L5:'none',R4:'leftMouse',R5:'none'};

// ---- State ----
var bindings = {L4:'leftMouse',L5:'none',R4:'leftMouse',R5:'none'};
var listening = null;
var flash = null;
var flashTimer = null;
var applyTimer = null;

// ---- Input catalog (Steam Controller nomenclature) ----
var INPUTS = [
  {id:'A',         glyph:'A',      label:'A Button',    bg:'#5ba32b',fg:'#fff'},
  {id:'B',         glyph:'B',      label:'B Button',    bg:'#c0392b',fg:'#fff'},
  {id:'X',         glyph:'X',      label:'X Button',    bg:'#2b7fc0',fg:'#fff'},
  {id:'Y',         glyph:'Y',      label:'Y Button',    bg:'#c9a227',fg:'#211a04'},
  {id:'LB',        glyph:'L1',     label:'L1 Bumper',   bg:'#37485a',fg:'#cdd9e3'},
  {id:'LT',        glyph:'L2',     label:'L2 Trigger',  bg:'#37485a',fg:'#cdd9e3'},
  {id:'RB',        glyph:'R1',     label:'R1 Bumper',   bg:'#37485a',fg:'#cdd9e3'},
  {id:'RT',        glyph:'R2',     label:'R2 Trigger',  bg:'#37485a',fg:'#cdd9e3'},
  {id:'Up',        glyph:'&#x2191;', label:'D-Pad Up',    bg:'#2a3f57',fg:'#cdd9e3'},
  {id:'Down',      glyph:'&#x2193;', label:'D-Pad Down',  bg:'#2a3f57',fg:'#cdd9e3'},
  {id:'Left',      glyph:'&#x2190;', label:'D-Pad Left',  bg:'#2a3f57',fg:'#cdd9e3'},
  {id:'Right',     glyph:'&#x2192;', label:'D-Pad Right', bg:'#2a3f57',fg:'#cdd9e3'},
  {id:'leftMouse', glyph:'LMB',     label:'Left Click',  bg:'#3a2a5a',fg:'#c9b8f0'},
  {id:'rightMouse',glyph:'RMB',     label:'Right Click', bg:'#3a2a5a',fg:'#c9b8f0'},
  {id:'menu',      glyph:'MNU',     label:'Menu',        bg:'#2a3a2a',fg:'#9ac89a'},
  {id:'view',      glyph:'VEW',     label:'View',        bg:'#2a3a2a',fg:'#9ac89a'},
  {id:'L3',        glyph:'L3',      label:'L3 Stick',    bg:'#37485a',fg:'#cdd9e3'},
  {id:'R3',        glyph:'R3',      label:'R3 Stick',    bg:'#37485a',fg:'#cdd9e3'},
  {id:'none',      glyph:'OFF',     label:'Off',         bg:'#1e2d3d',fg:'#5a7a9a'},
];
var byId = {};
INPUTS.forEach(function(t){byId[t.id]=t;});

// Picker layout: each inner array is one row in the manual-pick panel.
var PICKER_ROWS = [
  ['A','B','X','Y'],
  ['LB','LT','RB','RT'],
  ['Up','Down','Left','Right'],
  ['L3','R3','menu','view','leftMouse','rightMouse'],
  ['none'],
];

var ROWS = [
  {id:'L4',posTag:'UPPER',posLabel:'Upper grip'},
  {id:'L5',posTag:'LOWER',posLabel:'Lower grip'},
  {id:'R4',posTag:'UPPER',posLabel:'Upper grip'},
  {id:'R5',posTag:'LOWER',posLabel:'Lower grip'},
];

// ---- WebView2 bridge ----
function postMsg(obj){
  if(window.chrome&&window.chrome.webview)
    window.chrome.webview.postMessage(JSON.stringify(obj));
}
if(window.chrome&&window.chrome.webview){
  window.chrome.webview.addEventListener('message',function(e){
    var msg=JSON.parse(e.data);
    if(msg.type==='init'){
      bindings=msg.bindings;
      renderAll();
    } else if(msg.type==='buttonCaptured'){
      if(listening&&byId[msg.button]) doBind(listening,msg.button);
    }
  });
}

// ---- Actions ----
function startListening(rowId){
  if(listening&&listening!==rowId) postMsg({type:'stopListening'});
  listening=rowId;
  renderAll();
  postMsg({type:'startListening',row:rowId});
}
function cancelListening(){
  if(!listening) return;
  listening=null;
  renderAll();
  postMsg({type:'stopListening'});
}
function doBind(rowId,inputId){
  var b={};
  for(var k in bindings) b[k]=bindings[k];
  b[rowId]=inputId;
  bindings=b;
  listening=null;
  flash=rowId;
  clearTimeout(flashTimer);
  flashTimer=setTimeout(function(){flash=null;renderAll();},1300);
  renderAll();
  postMsg({type:'stopListening'});
}
function resetRow(rowId){
  doBind(rowId, DEFAULTS[rowId]);
}
function resetDefaults(){
  cancelListening();
  bindings={L4:DEFAULTS.L4,L5:DEFAULTS.L5,R4:DEFAULTS.R4,R5:DEFAULTS.R5};
  flash=null;
  clearTimeout(flashTimer);
  renderAll();
}
function applyBindings(){
  clearTimeout(applyTimer);
  var btn=document.getElementById('btn-apply');
  btn.textContent='Applied \u2713';
  btn.disabled=true;
  applyTimer=setTimeout(function(){btn.textContent='Apply';btn.disabled=false;},1500);
  postMsg({type:'apply',bindings:bindings});
}
// ---- Render helpers ----
function chipHTML(t,cls){
  return '<div class="'+cls+'" style="background:'+t.bg+';color:'+t.fg+';">'+t.glyph+'</div>';
}
function renderRow(def){
  var el=document.getElementById('row-'+def.id);
  if(!el) return;
  var isL=listening===def.id, isF=flash===def.id;
  el.className='row'+(isL?' listening':isF?' flash':'');
  var curBind=bindings[def.id];
  var tgt=byId[curBind]||byId['none'];
  var rightHTML;
  if(isL){
    rightHTML='<div class="listen-right">'+
      '<span class="listen-pill"><span class="pulse-dot"></span>Press a button...</span>'+
      '<button class="btn-row-reset" onclick="resetRow(\''+def.id+'\')">Reset</button>'+
      '<button class="btn-cancel" onclick="cancelListening()">&#10005;</button>'+
      '</div>';
  } else {
    rightHTML='<div class="row-right">'+
      chipHTML(tgt,'chip-lg')+
      '<span class="input-label">'+tgt.label+'</span>'+
      '<button class="btn-rebind" onclick="startListening(\''+def.id+'\')">Rebind</button>'+
      '</div>';
  }
  var pickerHTML='';
  if(isL){
    var pickerRows=PICKER_ROWS.map(function(row){
      var pills=row.map(function(inputId){
        var t=byId[inputId];
        return '<button class="picker-pill" onclick="doBind(\''+def.id+'\',\''+inputId+'\')">'+
          chipHTML(t,'chip-sm')+
          '<span class="picker-name">'+t.label+'</span></button>';
      }).join('');
      return '<div class="picker-row">'+pills+'</div>';
    }).join('');
    pickerHTML='<div class="picker">'+
      '<div class="picker-label">OR PICK MANUALLY</div>'+
      '<div class="picker-rows">'+pickerRows+'</div></div>';
  }
  el.innerHTML='<div class="row-top">'+
    '<div class="badge">'+
      '<span class="badge-id">'+def.id+'</span>'+
      '<span class="badge-pos">'+def.posTag+'</span>'+
    '</div>'+
    '<span class="pos-label">'+def.posLabel+'</span>'+
    '<div class="connector"></div>'+
    rightHTML+
    '</div>'+pickerHTML;
}
function renderAll(){
  ROWS.forEach(function(r){renderRow(r);});
}

// ---- Wire static controls ----
document.getElementById('btn-min').onclick=function(){postMsg({type:'minimize'});};
document.getElementById('btn-close').onclick=function(){postMsg({type:'close'});};
document.getElementById('btn-reset').onclick=resetDefaults;
document.getElementById('btn-apply').onclick=applyBindings;

// Title bar drag: mousedown on titlebar (but not on buttons) tells C++ to start a window move.
document.getElementById('titlebar').addEventListener('mousedown',function(e){
  if(e.target.closest('.winctls')) return;
  if(e.button!==0) return;
  postMsg({type:'startDrag'});
});

document.addEventListener('keydown',function(e){
  if(e.key==='Escape') cancelListening();
});

renderAll();
</script>
</body>
</html>
)HTML";
        std::wstring w;
        w.reserve(sizeof(kHtml));
        for (const char* p = kHtml; *p; ++p)
            w += static_cast<wchar_t>(static_cast<unsigned char>(*p));
        return w;
    }();
    return html;
}

// ---------------------------------------------------------------------------
// DPI sizing
// ---------------------------------------------------------------------------

// Resize to the design size scaled for this window's monitor and centre it
// there. Nothing scales these numbers for us — the process is
// PER_MONITOR_AWARE_V2 — so the raw design size renders a postage-stamp
// window on a high-DPI display while WebView2 renders its CSS at full scale
// inside it. Clamped to the work area because 760x668 at 200% is taller than
// a 1080p screen.
static void SizeAndCentreForDpi(HWND hwnd, int baseW, int baseH) {
    UINT dpi = GetDpiForWindow(hwnd);
    if (dpi == 0) dpi = USER_DEFAULT_SCREEN_DPI;

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
        return;

    const int workW = mi.rcWork.right - mi.rcWork.left;
    const int workH = mi.rcWork.bottom - mi.rcWork.top;
    int w = MulDiv(baseW, dpi, USER_DEFAULT_SCREEN_DPI);
    int h = MulDiv(baseH, dpi, USER_DEFAULT_SCREEN_DPI);
    if (w > workW) w = workW;
    if (h > workH) h = workH;

    SetWindowPos(hwnd, nullptr,
                 mi.rcWork.left + (workW - w) / 2,
                 mi.rcWork.top  + (workH - h) / 2,
                 w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK RemapWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (s_instance) return s_instance->HandleMessage(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT RemapWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        if (m_controller) {
            RECT bounds;
            GetClientRect(hwnd, &bounds);
            m_controller->put_Bounds(bounds);
        }
        return 0;

    case WM_GETMINMAXINFO: {
        // Also scaled: a 560x440 floor in raw pixels is unusably cramped once
        // the content inside is rendering at 200%.
        UINT dpi = GetDpiForWindow(hwnd);
        if (dpi == 0) dpi = USER_DEFAULT_SCREEN_DPI;
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize.x = MulDiv(MIN_WINDOW_W, dpi, USER_DEFAULT_SCREEN_DPI);
        mmi->ptMinTrackSize.y = MulDiv(MIN_WINDOW_H, dpi, USER_DEFAULT_SCREEN_DPI);
        return 0;
    }

    case WM_DPICHANGED: {
        // Dragged onto a monitor with different scaling. Windows hands us a
        // suggested rect already converted for the new DPI; taking it keeps
        // the window the same physical size instead of jumping.
        const RECT* suggested = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr,
                     suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_NCHITTEST: {
        // Let Windows handle non-client areas (resize border, etc.) first.
        LRESULT hit = DefWindowProcW(hwnd, msg, wp, lp);
        if (hit == HTCLIENT) {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &pt);
            RECT rc;
            GetClientRect(hwnd, &rc);
            // The right ~96px of the title bar hosts our close/min buttons —
            // leave those as HTCLIENT so WebView2 can receive the clicks.
            UINT dpi   = GetDpiForWindow(hwnd);
            int  tbPx  = MulDiv(46, dpi, 96);   // CSS 46px → physical pixels
            int  ctlPx = MulDiv(96, dpi, 96);
            if (pt.y < tbPx && pt.x < rc.right - ctlPx)
                return HTCAPTION;
        }
        return hit;
    }

    case WM_BUTTON_CAPTURED: {
        // Marshalled from the read thread via PostMessage, packed into WPARAM.
        const std::string id =
            BackButtonBinding::Unpack(static_cast<uint32_t>(wp)).Id();
        if (m_webview) {
            std::wstring json = L"{\"type\":\"buttonCaptured\",\"button\":\"";
            json += std::wstring(id.begin(), id.end());
            json += L"\"}";
            m_webview->PostWebMessageAsString(json.c_str());
        }
        return 0;
    }

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        if (m_mgr) m_mgr->StopButtonCapture();
        m_webview.Reset();
        m_controller.Reset();
        m_env.Reset();
        m_hwnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

RemapWindow::~RemapWindow() {
    if (m_hwnd) DestroyWindow(m_hwnd);
    s_instance = nullptr;
}

void RemapWindow::Open(HINSTANCE hInst, ControllerManager* mgr,
                       const BackButtonConfig& cfg,
                       std::function<void(const BackButtonConfig&)> applyCallback)
{
    // If already open, just bring it to front.
    if (m_hwnd) {
        if (IsWindowVisible(m_hwnd)) { BringToFront(); return; }
        // Was hidden. Refresh config and show.
        m_config        = cfg;
        m_applyCallback = std::move(applyCallback);
        ShowWindow(m_hwnd, SW_SHOW);
        BringToFront();
        SendInitState();
        return;
    }

    // --- Check WebView2 runtime ---
    WCHAR* versionStr = nullptr;
    if (FAILED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &versionStr)) || !versionStr) {
        MessageBoxW(nullptr,
            L"Back Button Remapping requires the Microsoft Edge WebView2 Runtime.\n\n"
            L"Please update Windows or download the runtime from microsoft.com/edge/webview2.",
            L"WebView2 Runtime Not Found", MB_OK | MB_ICONINFORMATION);
        return;
    }
    CoTaskMemFree(versionStr);

    m_hInst        = hInst;
    m_mgr          = mgr;
    m_config       = cfg;
    m_applyCallback = std::move(applyCallback);
    s_instance     = this;

    // --- Register window class (once) ---
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    // Match the page background. Resizing repaints the frame before WebView2
    // catches up, and the default white flashed against the dark UI. Brush
    // lives for the process, like the class it belongs to.
    wc.hbrBackground = CreateSolidBrush(RGB(0x15, 0x20, 0x2c));
    wc.lpszClassName = CLASS_NAME;
    wc.style         = CS_DROPSHADOW;
    RegisterClassExW(&wc); // OK if already registered

    // --- Create the popup window on the monitor the user is working on ---
    // The tray menu they opened this from is under the cursor, and on a
    // mixed-DPI desktop that monitor's scaling is what matters. Create it
    // there first, then size it once the window can report the DPI it landed
    // on. WS_THICKFRAME makes it resizable; its border sits in the
    // non-client area, outside the rect WebView2 occupies, so it stays
    // grabbable even though the web content covers the whole client area.
    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi);

    m_hwnd = CreateWindowExW(
        0, CLASS_NAME, L"Back Button Mapping",
        WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX,
        mi.rcWork.left, mi.rcWork.top, WINDOW_W, WINDOW_H,
        nullptr, nullptr, hInst, nullptr);

    if (!m_hwnd) return;

    SizeAndCentreForDpi(m_hwnd, WINDOW_W, WINDOW_H);

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);

    // WebView2 initialization is async; callbacks run on the UI thread
    // via the app's existing GetMessage/DispatchMessage loop in TrayApp::Run().
    CreateWebViewAsync(m_hwnd);
}

void RemapWindow::BringToFront() const {
    if (!m_hwnd) return;
    SetForegroundWindow(m_hwnd);
    BringWindowToTop(m_hwnd);
}

// ---------------------------------------------------------------------------
// WebView2 async init chain
// ---------------------------------------------------------------------------

void RemapWindow::CreateWebViewAsync(HWND hwnd) {
    // Use %LOCALAPPDATA%\SteamlessController\WebView2 for the browser data dir.
    wchar_t localApp[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localApp);
    std::wstring dataDir = std::wstring(localApp) + L"\\SteamlessController\\WebView2";

    using namespace Microsoft::WRL;

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, dataDir.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, hwnd](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) return S_OK;
                m_env = env;

                m_env->CreateCoreWebView2Controller(hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this](HRESULT result, ICoreWebView2Controller* ctrl) -> HRESULT {
                            if (FAILED(result) || !ctrl) return S_OK;
                            OnControllerReady(ctrl);
                            return S_OK;
                        }
                    ).Get());

                return S_OK;
            }
        ).Get());

    if (FAILED(hr)) {
        // WebView2 creation failed unexpectedly after version check — bail.
        DestroyWindow(hwnd);
    }
}

void RemapWindow::OnControllerReady(ICoreWebView2Controller* ctrl) {
    m_controller = ctrl;
    m_controller->get_CoreWebView2(&m_webview);

    // Settings: disable context menu and dev tools in production.
    Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
    m_webview->get_Settings(&settings);
    if (settings) {
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
#ifndef _DEBUG
        settings->put_AreDevToolsEnabled(FALSE);
#endif
    }

    // Size the WebView2 to fill the window.
    RECT bounds;
    GetClientRect(m_hwnd, &bounds);
    m_controller->put_Bounds(bounds);
    m_controller->put_IsVisible(TRUE);

    // Register the web message handler. Tokens are not stored since the handlers
    // persist for the lifetime of the WebView2 object (destroyed in WM_DESTROY).
    EventRegistrationToken token{};
    (void)m_webview->add_WebMessageReceived(
        Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                LPWSTR raw = nullptr;
                args->TryGetWebMessageAsString(&raw);
                if (raw) {
                    OnWebMessage(std::wstring(raw));
                    CoTaskMemFree(raw);
                }
                return S_OK;
            }
        ).Get(), &token);

    // Send initial state to the page once navigation completes.
    EventRegistrationToken navToken{};
    (void)m_webview->add_NavigationCompleted(
        Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                SendInitState();
                return S_OK;
            }
        ).Get(), &navToken);

    // Navigate to the embedded HTML.
    m_webview->NavigateToString(GetHtml().c_str());
}

// ---------------------------------------------------------------------------
// Message routing: JS → C++
// ---------------------------------------------------------------------------

// Minimal JSON string extractor — finds "key":"value" in a flat JSON object.
static std::string JsonStr(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    size_t end = json.find('"', pos);
    return end != std::string::npos ? json.substr(pos, end - pos) : std::string{};
}

void RemapWindow::OnWebMessage(const std::wstring& raw) {
    // All message content is ASCII; narrow explicitly to suppress C4244.
    std::string msg;
    msg.reserve(raw.size());
    for (wchar_t c : raw) msg += static_cast<char>(c);

    std::string type = JsonStr(msg, "type");

    if (type == "startDrag") {
        // Safe to call from WndProc context (we're on the UI thread).
        ReleaseCapture();
        PostMessageW(m_hwnd, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, 0);

    } else if (type == "minimize") {
        ShowWindow(m_hwnd, SW_MINIMIZE);

    } else if (type == "close") {
        PostMessageW(m_hwnd, WM_CLOSE, 0, 0);

    } else if (type == "startListening") {
        std::string row = JsonStr(msg, "row");
        if (!row.empty() && m_mgr) {
            HWND hwnd = m_hwnd;
            m_mgr->StartButtonCapture([hwnd](const BackButtonBinding& binding) {
                // Called from the read thread — use PostMessage to marshal to UI.
                PostMessageW(hwnd, WM_BUTTON_CAPTURED,
                             static_cast<WPARAM>(binding.Pack()), 0);
            });
        }

    } else if (type == "stopListening") {
        if (m_mgr) m_mgr->StopButtonCapture();

    } else if (type == "apply") {
        // Extract the four bindings from the nested "bindings" object.
        size_t bStart = msg.find("\"bindings\":");
        if (bStart != std::string::npos) {
            std::string sub = msg.substr(bStart);
            BackButtonConfig cfg;
            cfg.l4 = BackButtonBinding::FromId(JsonStr(sub, "L4"));
            cfg.l5 = BackButtonBinding::FromId(JsonStr(sub, "L5"));
            cfg.r4 = BackButtonBinding::FromId(JsonStr(sub, "R4"));
            cfg.r5 = BackButtonBinding::FromId(JsonStr(sub, "R5"));
            m_config = cfg;
            if (m_applyCallback) m_applyCallback(cfg);
        }
    }
}

void RemapWindow::PostToWebView(const std::wstring& jsonStr) {
    if (m_webview) m_webview->PostWebMessageAsString(jsonStr.c_str());
}

void RemapWindow::SendInitState() {
    if (!m_webview) return;

    // Convert ASCII action ID string to wstring without char→wchar_t narrowing warnings.
    auto wid = [](const BackButtonBinding& b) -> std::wstring {
        const std::string s = b.Id();
        return std::wstring(s.begin(), s.end());
    };

    std::wstring json =
        L"{\"type\":\"init\",\"bindings\":{"
        L"\"L4\":\"" + wid(m_config.l4) + L"\","
        L"\"L5\":\"" + wid(m_config.l5) + L"\","
        L"\"R4\":\"" + wid(m_config.r4) + L"\","
        L"\"R5\":\"" + wid(m_config.r5) + L"\""
        L"}}";
    PostToWebView(json);
}
