#include "RemapWindow.h"
#include "KeyInput.h"
#include "ControllerManager.h"
#include "EventLog.h"
#include "ProcessIdentity.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <cstring>
#include <cstdlib>

RemapWindow* RemapWindow::s_instance = nullptr;

// Defined further down, alongside the rest of the page-message plumbing;
// declared here because the picker's "add an application" paths sit above it.
static std::wstring JsonEscape(const std::wstring& s);

// ---------------------------------------------------------------------------
// Embedded UI — self-contained HTML/CSS/JS matching the design handoff.
// Google Fonts are loaded at runtime; if offline, system-ui fallback applies.
// ---------------------------------------------------------------------------

static const std::wstring& GetHtml() {
    // Narrow literal: a wide one would hit MSVC's C2026 "string too big" far
    // sooner. Each piece still has to stay under ~16 K on its own (65 535 is
    // only the limit on the concatenated result), hence the splits below.
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
/* Everything below the APPLY TO picker lives in #settings so it can be dimmed
   as one block. That made it a plain div between #body's flex children, so its
   groups stacked with no gap at all while the groups above it got #body's —
   which is why each section heading sat jammed against the box above it. Same
   axis, same gap, so the two halves of the page space identically. */
#settings{display:flex;flex-direction:column;gap:22px;}
.group{display:flex;flex-direction:column;gap:11px;}
/* An explicit line box: at 11px with this letter-spacing the uppercase
   headings were riding the top of their line and losing a pixel off the caps. */
.group-label{font-family:'JetBrains Mono',monospace;font-size:11px;letter-spacing:1.5px;color:#5c6b78;font-weight:600;line-height:1.5;}
.row{display:flex;flex-direction:column;background:rgba(255,255,255,.025);border:1px solid rgba(255,255,255,.06);border-radius:8px;padding:12px 16px;transition:border-color .2s,box-shadow .2s,background .2s;}
.row.listening{background:rgba(255,255,255,.05);border-color:#66c0f4;box-shadow:0 0 0 1px rgba(102,192,244,.4),0 10px 26px rgba(0,0,0,.32);}
.row.flash{border-color:#5ba32b;box-shadow:0 0 0 1px rgba(91,163,43,.4);}
.row-top{display:flex;align-items:center;gap:16px;}
.badge{width:46px;height:46px;flex:none;border-radius:10px;background:#1b2a3a;border:1px solid rgba(255,255,255,.08);display:flex;flex-direction:column;align-items:center;justify-content:center;}
.badge-id{font-size:15px;font-weight:800;color:#fff;line-height:1;}
.badge-pos{font-family:'JetBrains Mono',monospace;font-size:8px;color:#5c6b78;letter-spacing:.5px;margin-top:2px;}
.pos-label{font-size:13.5px;color:#aeb9c2;font-weight:500;flex:none;}
.inherit-row{display:flex;align-items:center;gap:10px;padding:14px 0 0 0;}
.inherit-row input{width:16px;height:16px;accent-color:#4a9eff;cursor:pointer;flex:none;margin:0;}
.inherit-row label{font-size:13.5px;color:#aeb9c2;font-weight:500;cursor:pointer;}
.inherit-note{font-size:12px;color:#7d8b96;padding:8px 0 14px 26px;line-height:1.45;}
.combo-badge{margin-left:8px;font-size:10px;font-weight:700;letter-spacing:.6px;color:#c9a86a;border:1px solid rgba(201,168,106,.35);border-radius:3px;padding:1px 5px;}
.missing-note{margin-top:11px;font-size:12px;color:#c9a86a;line-height:1.45;}
/* Says how a directional pad splits its surface, which is the one thing about
   these rows that cannot be worked out from their labels. */
.pad-note{font-size:12px;color:#7d8b96;padding:2px 0 10px;line-height:1.45;}
/* Following the default leaves nothing below worth reading as editable. */
#settings.inherited{opacity:.38;pointer-events:none;}
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
.combo-wrap{position:relative;}
.combo-toggle{width:100%;background:#101925;border:1px solid rgba(255,255,255,.12);border-radius:6px;color:#fff;font-size:13.5px;font-family:'Barlow',system-ui,sans-serif;padding:9px 12px;font-weight:600;cursor:pointer;display:flex;align-items:center;justify-content:space-between;gap:8px;}
.combo-toggle:hover{border-color:#66c0f4;}
.combo-toggle .chev{color:#5c6b78;font-size:11px;flex:none;}
.combo-panel{position:absolute;top:calc(100% + 6px);left:0;right:0;background:#101925;border:1px solid rgba(102,192,244,.3);border-radius:8px;box-shadow:0 12px 28px rgba(0,0,0,.45);padding:10px;z-index:20;}
.combo-search{width:100%;box-sizing:border-box;background:#0b131c;border:1px solid rgba(255,255,255,.12);border-radius:6px;color:#fff;font-size:13px;font-family:'Barlow',system-ui,sans-serif;padding:8px 10px;}
.combo-search:focus{outline:none;border-color:#66c0f4;}
.combo-section-label{font-family:'JetBrains Mono',monospace;font-size:10px;letter-spacing:1px;color:#5c6b78;font-weight:600;margin:10px 2px 6px;}
.combo-list{max-height:220px;overflow-y:auto;display:flex;flex-direction:column;gap:2px;}
.combo-item{padding:8px 10px;border-radius:6px;cursor:pointer;font-size:13px;color:#c7d5e0;}
.combo-item:hover{background:rgba(102,192,244,.12);color:#fff;}
.combo-item.active{background:rgba(102,192,244,.18);color:#fff;}
.combo-empty{padding:10px;font-size:12px;color:#5c6b78;text-align:center;}
.combo-add{margin-top:10px;padding-top:8px;border-top:1px solid rgba(255,255,255,.08);display:flex;flex-direction:column;gap:2px;}
.combo-add-item{padding:8px 10px;border-radius:6px;cursor:pointer;font-size:12.5px;color:#66c0f4;font-weight:600;}
.combo-add-item:hover{background:rgba(102,192,244,.12);}
.combo-note{padding:8px 10px 2px;font-size:11.5px;color:#5c6b78;text-align:center;}
.combo-back{padding:6px 10px 8px;font-size:11.5px;color:#8f98a0;cursor:pointer;}
.combo-back:hover{color:#66c0f4;}
.combo-item .exe{display:block;font-size:11px;color:#5c6b78;margin-top:2px;}
.mode-row{padding-top:14px;padding-bottom:14px;}
.mode-select{background:#101925;border:1px solid rgba(255,255,255,.12);border-radius:6px;color:#fff;font-size:13px;font-family:'Barlow',system-ui,sans-serif;padding:7px 10px;font-weight:600;cursor:pointer;flex:none;min-width:215px;}
.mode-select:hover{border-color:#66c0f4;}
.mode-select:focus{outline:none;border-color:#66c0f4;}
.speed-wrap{display:flex;align-items:center;gap:10px;flex:none;min-width:215px;}
.speed-slider{-webkit-appearance:none;appearance:none;flex:1;height:4px;border-radius:2px;background:rgba(255,255,255,.16);outline:none;cursor:pointer;}
.speed-slider::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;width:15px;height:15px;border-radius:50%;background:#66c0f4;border:none;cursor:pointer;}
.speed-slider:hover::-webkit-slider-thumb{background:#8fd3ff;}
.speed-slider:focus{outline:none;}
.speed-val{font-family:'JetBrains Mono',monospace;font-size:12px;color:#8f98a0;font-weight:600;min-width:44px;text-align:right;}
.offbar{display:none;margin:0 0 14px;padding:11px 13px;border-radius:8px;background:rgba(214,146,42,.13);border:1px solid rgba(214,146,42,.42);align-items:center;gap:12px;}
.offbar.on{display:flex;}
.offbar-text{flex:1;font-size:12.5px;color:#e8c07a;line-height:1.45;}
.offbar-text b{color:#ffd699;font-weight:700;}
.offbar-btn{padding:7px 14px;border-radius:3px;border:1px solid #4c7a1d;background:linear-gradient(to bottom,#94c63d,#5d8a1f);color:#13260a;font-weight:700;font-size:12.5px;white-space:nowrap;}
.offbar-btn:hover{opacity:.9;}
.picker{margin-top:12px;background:#101925;border:1px solid rgba(102,192,244,.2);border-radius:8px;padding:12px 13px;}
.picker-label{font-family:'JetBrains Mono',monospace;font-size:10.5px;letter-spacing:1px;color:#5c6b78;font-weight:600;}
.picker-rows{display:flex;flex-direction:column;gap:7px;margin-top:10px;}
.picker-row{display:flex;flex-wrap:wrap;gap:7px;}
.picker-pill{display:flex;align-items:center;gap:8px;padding:5px 11px 5px 5px;border-radius:7px;border:1px solid rgba(255,255,255,.08);background:rgba(255,255,255,.03);cursor:pointer;transition:border-color .15s,background .15s;}
.picker-pill:hover{border-color:#66c0f4;background:rgba(102,192,244,.1);}
.chip-sm{width:28px;height:28px;border-radius:6px;display:flex;align-items:center;justify-content:center;font-weight:800;font-size:11px;flex:none;}
.picker-name{font-size:12px;color:#c7d5e0;font-weight:600;}
.mod-pill{padding:6px 14px;border-radius:7px;border:1px solid rgba(255,255,255,.12);background:rgba(255,255,255,.03);color:#aeb9c2;font-size:12px;font-weight:700;letter-spacing:.3px;transition:border-color .15s,color .15s,background .15s;}
.mod-pill:hover{border-color:#66c0f4;color:#66c0f4;}
.mod-pill.on{border-color:#66c0f4;background:rgba(102,192,244,.18);color:#fff;}
.mod-note{font-size:11.5px;color:#5c6b78;line-height:1.45;margin-top:9px;}
.mod-note b{color:#8f98a0;font-weight:700;}
#footer{height:62px;flex:none;display:flex;align-items:center;justify-content:space-between;padding:0 20px;background:#15202c;border-top:1px solid rgba(0,0,0,.45);}
.footer-right{display:flex;gap:10px;align-items:center;}
.btn-reset{padding:9px 16px;border-radius:3px;border:1px solid rgba(255,255,255,.12);background:rgba(255,255,255,.04);color:#aeb9c2;font-weight:600;font-size:13.5px;transition:color .15s;}
.btn-reset:hover{color:#fff;}
.btn-apply{padding:9px 22px;border-radius:3px;border:1px solid #4c7a1d;background:linear-gradient(to bottom,#94c63d,#5d8a1f);color:#13260a;font-weight:700;font-size:13.5px;box-shadow:0 1px 0 rgba(255,255,255,.25) inset;min-width:96px;text-align:center;transition:opacity .15s;}
.btn-apply:hover{opacity:.9;}
.btn-apply:disabled{opacity:.7;cursor:default;}
#modal-backdrop{position:fixed;inset:0;background:rgba(0,0,0,.6);display:flex;align-items:center;justify-content:center;z-index:50;}
.modal{background:#1b2838;border:1px solid rgba(102,192,244,.3);border-radius:10px;box-shadow:0 20px 50px rgba(0,0,0,.6);padding:22px 24px;max-width:420px;}
.modal h3{margin:0 0 8px;font-size:16px;font-weight:800;color:#fff;}
.modal p{margin:0 0 18px;font-size:13.5px;color:#aeb9c2;line-height:1.5;}
.modal-btns{display:flex;gap:9px;justify-content:flex-end;}
.btn-discard{padding:8px 15px;border-radius:4px;border:1px solid rgba(255,255,255,.12);background:rgba(255,255,255,.04);color:#aeb9c2;font-weight:600;font-size:13px;}
.btn-discard:hover{border-color:#c0392b;color:#fff;}
.btn-modal-cancel{padding:8px 15px;border-radius:4px;border:1px solid rgba(255,255,255,.12);background:rgba(255,255,255,.04);color:#aeb9c2;font-weight:600;font-size:13px;}
.btn-modal-cancel:hover{color:#fff;}
.btn-modal-delete{padding:8px 15px;border-radius:4px;border:none;background:#a33a3a;color:#fff;font-weight:700;font-size:13px;}
.btn-modal-delete:hover{opacity:.9;}
/* Same treatment as Apply — padding, radius, inset highlight — so it reads as
   a peer of it rather than as a warning, but coloured for what it does. The
   footer is space-between, so it sits at the far end from Apply: separated by
   the whole width of the window rather than by a gap somebody can misjudge. */
.btn-delete{padding:9px 16px;border-radius:3px;border:1px solid #7a2626;background:linear-gradient(to bottom,#b34141,#8a2727);color:#fff;font-weight:700;font-size:13.5px;box-shadow:0 1px 0 rgba(255,255,255,.18) inset;transition:opacity .15s;}
.btn-delete:hover{opacity:.9;}
.btn-save{padding:8px 18px;border-radius:4px;border:1px solid #4c7a1d;background:linear-gradient(to bottom,#94c63d,#5d8a1f);color:#13260a;font-weight:700;font-size:13px;}
.btn-save:hover{opacity:.9;}
</style>
)HTML"
// Styles above, markup below.
R"HTML(
</head>
<body>

<div id="titlebar">
  <div class="left">
    <div class="glyph"><div class="glyph-dot"></div></div>
    <span class="tb-title">SteamlessController</span>
    <span class="tb-sub">&#8212; Customize Controls</span>
  </div>
  <div class="winctls">
    <button class="winctl" id="btn-min" title="Minimize">&#8211;</button>
    <button class="winctl close" id="btn-close" title="Close">&#10005;</button>
  </div>
</div>

<div id="body">
  <div>
    <h2>Customize Controls</h2>
    <p class="instr">Click <b>Rebind</b> on any button, then press any gamepad button on your controller &#8212; or any key on your keyboard, or your mouse's middle or thumb buttons. The new binding shows up here instantly. Pick <b>Off</b> to stop a button doing anything at all.</p>
  </div>
  <div class="offbar" id="offbar">
    <div class="offbar-text" id="offbar-text"></div>
    <button class="offbar-btn" id="offbar-btn">Enable</button>
  </div>
  <div class="group">
    <div class="group-label">APPLY TO</div>
    <div class="combo-wrap">
      <button class="combo-toggle" id="combo-toggle">
        <span id="combo-current">Default (all other games)</span>
        <span class="chev">&#9660;</span>
      </button>
      <div class="combo-panel" id="combo-panel" style="display:none;">
        <input class="combo-search" id="combo-search" type="text" placeholder="Search for a game or app..." autocomplete="off">
        <div id="combo-results"></div>
        <div class="combo-add" id="combo-add">
          <div class="combo-add-item" onclick="askRunningApps()">&#43;&nbsp; Add an app that&#39;s running now&#8230;</div>
          <div class="combo-add-item" onclick="askBrowseExe()">&#43;&nbsp; Browse for a program&#8230;</div>
        </div>
      </div>
    </div>
    <div class="missing-note" id="missing-note" style="display:none;">This game wasn't found on this PC. Its profile is kept, and will start working again if the game comes back. Delete this if the game won't be coming back.</div>
  </div>
  <div class="group" id="inherit-group" style="display:none;">
    <div class="group-label">MAPPINGS</div>
    <div class="inherit-row">
      <input type="checkbox" id="use-default">
      <label for="use-default">Use my default mappings</label>
    </div>
    <div class="inherit-note">This game turns the controller on and uses the controls from your default profile, including any later changes to it. Uncheck to give this game controls of its own.</div>
  </div>
  <div id="settings">
  <div class="group">
    <div class="group-label">CONTROLLER PLATFORM</div>
    <div class="row mode-row">
      <div class="row-top">
        <span class="pos-label">Appear to games as</span>
        <div class="connector"></div>
        <select id="platform" class="mode-select"></select>
      </div>
    </div>
  </div>
  <div class="group">
    <div class="group-label">LEFT TRACKPAD</div>
    <div class="row mode-row">
      <div class="row-top">
        <span class="pos-label">Movement Mode</span>
        <div class="connector"></div>
        <select id="mode-LPAD" class="mode-select"></select>
      </div>
    </div>
    <div class="row mode-row" id="dir-row-LPAD">
      <div class="row-top">
        <span class="pos-label">Scroll Direction</span>
        <div class="connector"></div>
        <select id="dir-LPAD" class="mode-select"></select>
      </div>
    </div>
    <div class="row mode-row" id="speed-row-LPAD">
      <div class="row-top">
        <span class="pos-label">Scroll Speed</span>
        <div class="connector"></div>
        <div class="speed-wrap">
          <input type="range" id="speed-LPAD" class="speed-slider">
          <span class="speed-val" id="speed-val-LPAD">1x</span>
        </div>
      </div>
    </div>
    <div class="row mode-row" id="diag-row-LPAD">
      <div class="row-top">
        <span class="pos-label">Diagonals</span>
        <div class="connector"></div>
        <select id="diag-LPAD" class="mode-select"></select>
      </div>
    </div>
    <div class="pad-note" id="note-LPAD">Press the outer part of the pad for a direction, or the middle for the trackpad click.</div>
    <div id="row-LPADup" class="row"></div>
    <div id="row-LPADdown" class="row"></div>
    <div id="row-LPADleft" class="row"></div>
    <div id="row-LPADright" class="row"></div>
    <div id="row-LPAD" class="row"></div>
    <div id="row-LPADtouch" class="row"></div>
  </div>
  <div class="group">
    <div class="group-label">RIGHT TRACKPAD</div>
    <div class="row mode-row">
      <div class="row-top">
        <span class="pos-label">Movement Mode</span>
        <div class="connector"></div>
        <select id="mode-RPAD" class="mode-select"></select>
      </div>
    </div>
    <div class="row mode-row" id="dir-row-RPAD">
      <div class="row-top">
        <span class="pos-label">Scroll Direction</span>
        <div class="connector"></div>
        <select id="dir-RPAD" class="mode-select"></select>
      </div>
    </div>
    <div class="row mode-row" id="speed-row-RPAD">
      <div class="row-top">
        <span class="pos-label">Scroll Speed</span>
        <div class="connector"></div>
        <div class="speed-wrap">
          <input type="range" id="speed-RPAD" class="speed-slider">
          <span class="speed-val" id="speed-val-RPAD">1x</span>
        </div>
      </div>
    </div>
    <div class="row mode-row" id="diag-row-RPAD">
      <div class="row-top">
        <span class="pos-label">Diagonals</span>
        <div class="connector"></div>
        <select id="diag-RPAD" class="mode-select"></select>
      </div>
    </div>
    <div class="pad-note" id="note-RPAD">Press the outer part of the pad for a direction, or the middle for the trackpad click.</div>
    <div id="row-RPADup" class="row"></div>
    <div id="row-RPADdown" class="row"></div>
    <div id="row-RPADleft" class="row"></div>
    <div id="row-RPADright" class="row"></div>
    <div id="row-RPAD" class="row"></div>
    <div id="row-RPADtouch" class="row"></div>
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
</div>

<div id="footer">
  <div class="footer-right">
    <button class="btn-reset" id="btn-reset">Reset this profile to default</button>
    <button class="btn-apply" id="btn-apply">Apply</button>
  </div>
  <button class="btn-delete" id="btn-delete" style="display:none;">Delete Profile</button>
</div>

<div id="modal-backdrop" style="display:none;">
  <div class="modal">
    <h3 id="modal-title">Unsaved changes</h3>
    <p id="modal-text">You have unsaved changes to this profile.</p>
    <div class="modal-btns">
      <button class="btn-modal-cancel" id="btn-modal-cancel">Cancel</button>
      <button class="btn-discard" id="btn-modal-discard">Discard</button>
      <button class="btn-save" id="btn-modal-save">Save</button>
      <button class="btn-modal-delete" id="btn-modal-delete" style="display:none;">Delete</button>
    </div>
  </div>
</div>
)HTML"
// The page is split across several adjacent string literals only because MSVC
// caps one at 16380 bytes (C2026); the compiler concatenates them straight
// back into a single document, so a split can go anywhere two complete lines
// meet and means nothing at runtime.
//
// Keep every piece under about 12KB. An earlier arrangement aimed at "roughly
// 16KB" and drifted to 17.5KB apiece, which built for a while and then stopped
// compiling outright when the toolchain moved to VS 2026 — the limit is exact,
// and there is no warning on the way to hitting it. When a piece outgrows its
// budget, add another split rather than rebalancing the existing ones.
//
// Markup above, script below.
R"HTML(
<script>
'use strict';
// ---- Defaults ----
// Upper paddles left-click, lower paddles unbound; the right pad points and
// clicks while the left pad scrolls. Keep in sync with ControllerProfile in
// TrackpadConfig.h — these two are what "default" means, and a fresh install
// and this window's reset button both have to land on the same thing.
var DEFAULTS = {L4:'leftMouse',L5:'none',R4:'leftMouse',R5:'none',LPAD:'none',RPAD:'leftMouse',
  // Directions default to the gamepad d-pad so picking the mode does the
  // obvious thing before anything is rebound; touch starts unbound. Keep in
  // step with TrackpadSettings in TrackpadConfig.h.
  LPADup:'Up',LPADdown:'Down',LPADleft:'Left',LPADright:'Right',LPADtouch:'none',
  RPADup:'Up',RPADdown:'Down',RPADleft:'Left',RPADright:'Right',RPADtouch:'none'};
var DEFAULT_MODES = {LPAD:'scroll',RPAD:'pointer'};
var DEFAULT_DIRS  = {LPAD:'natural',RPAD:'natural'};
var DEFAULT_DIAGS = {LPAD:'eight',RPAD:'eight'};
// Percent of the calibrated scroll scale. Keep in step with
// kScrollSpeedDefault in TrackpadConfig.h.
var DEFAULT_SPEEDS = {LPAD:100,RPAD:100};
// The stops the scroll speed slider offers, as percentages of the calibrated
// feel. Stored and sent as those percentages, so the C++ side keeps taking any
// value in kScrollSpeedMin..Max and nothing about persistence changes — these
// are where the slider is willing to stop, not what the setting can hold.
//
// Discrete rather than continuous for two reasons. The spacing is roughly a
// constant ratio (about 1.4x a step), which a linear slider cannot be: over
// 10..400 linear, three quarters of the track sits above 1x where nobody has
// asked to go, and the region people actually use is squeezed into the first
// quarter. And a named stop can be said out loud — "set it to 0.5x" ends a
// support round trip that "set it to 47%" starts another one of.
//
// 100 is here because it is the default, and 25 because that is where the #95
// reporter settled; a list that rounded either of them away would move a
// setting somebody had already chosen.
var SPEED_STOPS = [10,15,25,35,50,75,100,150,200,300,400];
var DEFAULT_PLATFORM = 'xbox';

// ---- State ----
var bindings = {L4:'leftMouse',L5:'none',R4:'leftMouse',R5:'none',LPAD:'none',RPAD:'none'};
// Trackpad movement modes, keyed by the same row ids their click rows use.
var modes = {LPAD:'none',RPAD:'none'};
// Scroll direction per pad. Only meaningful in scroll mode, but kept for
// every pad so switching modes back and forth does not lose the choice.
var dirs = {LPAD:'natural',RPAD:'natural'};
// Whether a directional pad's diagonals press two directions or round to one.
// Kept for every pad for the same reason as the scroll direction.
var diags = {LPAD:'eight',RPAD:'eight'};
// Scroll speed per pad, kept for every pad for the same reason as the two
// above: switching modes back and forth must not lose the choice.
var speeds = {LPAD:100,RPAD:100};
var platform = 'xbox';
// This game follows the default profile's controls instead of carrying its
// own. Only ever true for a game — the default has nothing to follow.
var useDefault = false;
// The DS4 touchpad only exists on a virtual PlayStation pad — an X360 report
// has nothing to carry it. The option stays selectable either way (a profile
// can legitimately be edited before its platform is), but it says so, and
// re-labels the moment the platform dropdown above it changes.
function modeOptions(){
  return [
    {id:'none',    label:'None'},
    {id:'pointer', label:'Mouse Pointer'},
    {id:'scroll',  label:'Scroll Wheel'},
    {id:'ds4',     label:platform==='ps'?'DS4 Touchpad'
                                        :'DS4 Touchpad (PlayStation only)'},
    {id:'dpad',    label:'Directional Pad'},
    {id:'button',  label:'Single Button'},
  ];
}
var DIR_OPTIONS = [
  {id:'natural',  label:'Natural'},
  {id:'reversed', label:'Reversed'},
];
var DIAG_OPTIONS = [
  {id:'eight', label:'8-way (press two at once)'},
  {id:'four',  label:'4-way (one at a time)'},
];
var PLATFORM_OPTIONS = [
  {id:'xbox', label:'Xbox Controller'},
  {id:'ps',   label:'PlayStation Controller'},
];
var listening = null;
// Modifier bits, matching BackButtonBinding::Modifier.
var MOD = {CTRL:1, ALT:2, SHIFT:4, WIN:8};
var MODIFIER_LIST = [
  {bit:MOD.CTRL,  label:'Ctrl'},
  {bit:MOD.ALT,   label:'Alt'},
  {bit:MOD.SHIFT, label:'Shift'},
  // Cannot be captured by pressing it — Windows opens the Start menu and
  // takes focus before this page is told anything. The toggle is the only
  // way in, which is why modifiers are chosen rather than typed.
  {bit:MOD.WIN,   label:'Win'},
];
var MODIFIER_CODES = {
  ControlLeft:MOD.CTRL, ControlRight:MOD.CTRL,
  AltLeft:MOD.ALT,      AltRight:MOD.ALT,
  ShiftLeft:MOD.SHIFT,  ShiftRight:MOD.SHIFT,
  MetaLeft:MOD.WIN,     MetaRight:MOD.WIN,
};
// Modifiers armed for the binding currently being captured, from ticking a
// toggle or from pressing the key itself. Cleared whenever listening starts.
var pendingMods = 0;
// Which of those arrived by pressing the physical key rather than ticking a
// toggle — only those can be released, and releasing is what binds them alone.
var modsPressed = 0;
// A capture is in flight. The binding comes back from C++ asynchronously, and
// the user's fingers leave the keys well before it lands; without this the
// keyup of a modifier they were holding would send a second, different one.
var captureSent = false;
var sasWarning = false;
function sendCapture(code, mods){
  captureSent=true;
  postMsg({type:'keyCaptured',code:code,mods:String(mods)});
}
// What the keyboard reported alongside this keypress. A fallback, not the
// advertised flow: pressing a whole shortcut at once often never reaches this
// page at all, because Windows claims combinations like Alt+Tab first. It
// still earns its place for keys that need a modifier to type — Shift and the
// punctuation key that produces "?" arrive together whether or not the toggle
// was ticked.
function liveMods(e){
  return (e.ctrlKey?MOD.CTRL:0)|(e.altKey?MOD.ALT:0)
       |(e.shiftKey?MOD.SHIFT:0)|(e.metaKey?MOD.WIN:0);
}
// Display names for key bindings, keyed by binding id. Supplied by C++ from the
// active keyboard layout — the catalog below can't cover them.
var keyLabels = {};
var flash = null;
var flashTimer = null;
var applyTimer = null;
// Per-game picker. Game ids are opaque decimal tokens issued by C++, not the
// raw exe path — that keeps the hand-rolled JSON channel free of Unicode and
// backslash escaping. A token is never reused or renumbered, so one held here
// stays valid when GAMES is replaced. "" is the always-present default profile.
var GAMES = [];
// True between the window opening and the installed list arriving. The list
// is built on a background thread because it takes a second or more.
var gamesPending = true;
// The "add an app that's running now" list, and whether it is showing in
// place of the normal picker contents. Same token discipline as GAMES.
var RUNNING = [];
var runningOpen = false;
// Each profile is a flat object: one entry per bindable row id, plus
// "<pad>mode" for each trackpad's movement mode. Flat because the JSON
// reader on the C++ side matches "key":"value" pairs without nesting.
var PROFILES = {'':{platform:'xbox',L4:'leftMouse',L5:'none',R4:'leftMouse',R5:'none',LPAD:'none',RPAD:'leftMouse',LPADmode:'scroll',RPADmode:'pointer',LPADdir:'natural',RPADdir:'natural',LPADspeed:'100',RPADspeed:'100',LPADdiag:'eight',RPADdiag:'eight',LPADtouch:'none',RPADtouch:'none',LPADup:'Up',LPADdown:'Down',LPADleft:'Left',LPADright:'Right',RPADup:'Up',RPADdown:'Down',RPADleft:'Left',RPADright:'Right'}};
var currentGame = '';
var comboOpen = false;
var comboQuery = '';
// What the current selection was when it was last loaded or saved — the
// baseline "unsaved changes" is measured against.
var savedProfile = null;
// What to run once the unsaved-changes prompt resolves. Set when the prompt
// opens, because the action that triggered it has to wait for the answer.
var pendingAction = null;

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
  {id:'mouse:middle',glyph:'MMB',   label:'Middle Click',bg:'#3a2a5a',fg:'#c9b8f0'},
  {id:'mouse:x1',  glyph:'M4',      label:'Mouse 4',     bg:'#3a2a5a',fg:'#c9b8f0'},
  {id:'mouse:x2',  glyph:'M5',      label:'Mouse 5',     bg:'#3a2a5a',fg:'#c9b8f0'},
  {id:'menu',      glyph:'MNU',     label:'Menu',        bg:'#2a3a2a',fg:'#9ac89a'},
  {id:'view',      glyph:'VEW',     label:'View',        bg:'#2a3a2a',fg:'#9ac89a'},
  {id:'L3',        glyph:'L3',      label:'L3 Stick',    bg:'#37485a',fg:'#cdd9e3'},
  {id:'R3',        glyph:'R3',      label:'R3 Stick',    bg:'#37485a',fg:'#cdd9e3'},
  {id:'touchKeyboard',glyph:'KBD',  label:'Touch Keyboard',bg:'#2a3a2a',fg:'#9ac89a'},
  {id:'none',      glyph:'OFF',     label:'Off',         bg:'#1e2d3d',fg:'#5a7a9a'},
];
var byId = {};
INPUTS.forEach(function(t){byId[t.id]=t;});

// Picker layout: each inner array is one row in the manual-pick panel.
var PICKER_ROWS = [
  ['A','B','X','Y'],
  ['LB','LT','RB','RT'],
  ['Up','Down','Left','Right'],
  ['L3','R3','menu','view'],
  ['leftMouse','rightMouse','mouse:middle','mouse:x1','mouse:x2'],
  ['touchKeyboard','none'],
];

// Every rebindable row, trackpad clicks included — they are the same kind of
// thing as a paddle and go through the identical rebind/capture flow.
var ROWS = [
  {id:'L4',posTag:'UPPER',posLabel:'Upper grip'},
  {id:'L5',posTag:'LOWER',posLabel:'Lower grip'},
  {id:'R4',posTag:'UPPER',posLabel:'Upper grip'},
  {id:'R5',posTag:'LOWER',posLabel:'Lower grip'},
  {id:'LPAD',badge:'L',posTag:'PAD',posLabel:'Trackpad Click'},
  {id:'RPAD',badge:'R',posTag:'PAD',posLabel:'Trackpad Click'},
  {id:'LPADtouch',badge:'L',posTag:'TAP',posLabel:'Trackpad Tap'},
  {id:'RPADtouch',badge:'R',posTag:'TAP',posLabel:'Trackpad Tap'},
  {id:'LPADup',   badge:'L',posTag:'UP',   posLabel:'Up'},
  {id:'LPADdown', badge:'L',posTag:'DOWN', posLabel:'Down'},
  {id:'LPADleft', badge:'L',posTag:'LEFT', posLabel:'Left'},
  {id:'LPADright',badge:'L',posTag:'RIGHT',posLabel:'Right'},
  {id:'RPADup',   badge:'R',posTag:'UP',   posLabel:'Up'},
  {id:'RPADdown', badge:'R',posTag:'DOWN', posLabel:'Down'},
  {id:'RPADleft', badge:'R',posTag:'LEFT', posLabel:'Left'},
  {id:'RPADright',badge:'R',posTag:'RIGHT',posLabel:'Right'},
];
// The pads, and the dropdown id each one's Movement Mode lives in.
var PADS = ['LPAD','RPAD'];
// Every row a pad can have, as the suffix appended to its id. The empty one is
// the click, whose row id is the bare pad id — it was the only pad row when
// these were named, and renaming it would strand every profile already saved.
var PAD_SUFFIXES = ['','touch','up','down','left','right'];
// Which of a pad's rows each mode shows. The click and the four directions are
// the same physical press told apart by where it lands, so a mode either
// offers directions or it does not; touch is a separate event and rides along
// with anything that is using the pad at all.
var PAD_ROWS_BY_MODE = {
  none:    [],
  pointer: ['','touch'],
  scroll:  ['','touch'],
  // A DS4 touchpad's press and contact are the touchpad's own, so neither is
  // rebindable — offering either would promise what the pad cannot deliver.
  ds4:     [],
  dpad:    ['up','down','left','right','','touch'],
  button:  ['','touch'],
};

// ---- WebView2 bridge ----
function postMsg(obj){
  if(window.chrome&&window.chrome.webview)
    window.chrome.webview.postMessage(JSON.stringify(obj));
}
if(window.chrome&&window.chrome.webview){
  window.chrome.webview.addEventListener('message',function(e){
    var msg=JSON.parse(e.data);
    if(msg.type==='init'){
      keyLabels=msg.labels||{};
      GAMES=msg.games||[];
      PROFILES=msg.profiles||PROFILES;
      // Set by C++, not assumed: when the enumeration beat WebView2's startup
      // the list is already in this message and no 'games' message follows.
      gamesPending=(msg.pending==='1');
      currentGame='';
      loadProfileInto(PROFILES['']||{});
      closeCombo();
      renderComboLabel();
      renderModeSelects();
      renderAll();
    } else if(msg.type==='controlState'){
      controlOn     = msg.enabled==='1';
      controlManual = msg.manual==='1';
      renderOffbar();
    } else if(msg.type==='games'){
      // Finding every installed app takes a second or more, so it lands after
      // the window is already up — and possibly after the user has started
      // editing. Deliberately touches nothing but the list: currentGame and
      // the bindings in flight are theirs, not ours to reset.
      GAMES=msg.games||[];
      PROFILES=msg.profiles||PROFILES;
      gamesPending=false;
      renderComboLabel();
      if(comboOpen) renderCombo();
    } else if(msg.type==='runningApps'){
      RUNNING=msg.apps||[];
      runningOpen=true;
      renderCombo();
    } else if(msg.type==='gameAdded'){
      runningOpen=false;
      pickGame(msg.id);
    } else if(msg.type==='closeRequested'){
      // C++ asks before hiding the window so an unsaved profile isn't lost.
      guard(function(){postMsg({type:'close'});});
    } else if(msg.type==='buttonCaptured'){
      if(!listening) return;
      if(msg.label) keyLabels[msg.button]=msg.label;
      doBind(listening,msg.button);
    }
  });
}

)HTML"
// Bridge and catalog above, the actions they drive below.
R"HTML(
// ---- Actions ----
function startListening(rowId){
  if(listening&&listening!==rowId) postMsg({type:'stopListening'});
  listening=rowId;
  pendingMods=0;
  modsPressed=0;
  captureSent=false;
  sasWarning=false;
  renderAll();
  postMsg({type:'startListening',row:rowId});
}
function toggleMod(bit){
  pendingMods^=bit;
  sasWarning=false;
  renderAll();
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
function setMode(padId,modeId){
  modes[padId]=modeId;
  // The rows below the dropdown depend on the mode. Anything listening that
  // the new mode hides has to stop: left running it would bind a row nobody
  // can see, and the captured press would land somewhere invisible.
  if(listening&&listening.indexOf(padId)===0){
    var shown=PAD_ROWS_BY_MODE[modeId]||[];
    if(shown.indexOf(listening.slice(padId.length))<0) cancelListening();
  }
  renderPadRows();
}
function setDir(padId,dirId){
  dirs[padId]=dirId;
}
// The one place a scroll speed is bounded, so a value arriving from storage
// and one arriving from the slider cannot disagree. Anything unparseable or
// zero is "unset", which is what the registry writes for a profile saved
// before this setting existed — see ScrollSpeedFromDword.
//
// Snaps to the nearest stop, so the slider position and the readout always
// describe the same number. A value between stops can still be stored — an
// older build wrote continuous ones, and C++ accepts anything in range — it
// simply shows as the stop nearest it, and only becomes that value if the user
// applies.
function clampSpeed(padId,value){
  var v=parseInt(value,10);
  if(isNaN(v)||v===0) return DEFAULT_SPEEDS[padId];
  var best=SPEED_STOPS[0];
  for(var i=0;i<SPEED_STOPS.length;i++){
    if(Math.abs(SPEED_STOPS[i]-v)<Math.abs(best-v)) best=SPEED_STOPS[i];
  }
  return best;
}
// How a stop is written for a person: a multiplier, because that is what it is
// and because "0.5x" survives being read aloud into a bug report.
function speedLabel(value){ return String(value/100)+'x'; }
// Live while dragging, so the readout tracks the thumb. Takes a percentage, not
// a slider position — the listener converts. Stored as a number;
// currentProfile stringifies it on the way out, matching every other value on
// the wire.
function setSpeed(padId,value){
  speeds[padId]=clampSpeed(padId,value);
  var out=document.getElementById('speed-val-'+padId);
  if(out) out.textContent=speedLabel(speeds[padId]);
}
function setDiag(padId,diagId){
  diags[padId]=diagId;
}
function resetDefaults(){
  cancelListening();
  platform=DEFAULT_PLATFORM;
  bindings={};
  ROWS.forEach(function(r){bindings[r.id]=DEFAULTS[r.id];});
  modes={}; dirs={}; diags={}; speeds={};
  PADS.forEach(function(p){
    modes[p]=DEFAULT_MODES[p];
    dirs[p]=DEFAULT_DIRS[p];
    diags[p]=DEFAULT_DIAGS[p];
    speeds[p]=DEFAULT_SPEEDS[p];
  });
  flash=null;
  clearTimeout(flashTimer);
  renderModeSelects();
  renderAll();
}
function applyBindings(){
  clearTimeout(applyTimer);
  var btn=document.getElementById('btn-apply');
  btn.textContent='Applied \u2713';
  btn.disabled=true;
  applyTimer=setTimeout(function(){btn.textContent='Apply';btn.disabled=false;},1500);
  saveCurrent();
}
// Flatten the live editor state into the stored/wire profile shape.
function currentProfile(){
  var p={useDefault:useDefault?'1':'0',platform:platform};
  ROWS.forEach(function(r){p[r.id]=bindings[r.id];});
  PADS.forEach(function(x){
    p[x+'mode']=modes[x];
    p[x+'dir']=dirs[x];
    p[x+'diag']=diags[x];
    p[x+'speed']=String(speeds[x]);
  });
  return p;
}
// Inverse of currentProfile: adopt a stored profile as the live state, filling
// in defaults for anything it does not carry. A profile saved before trackpad
// settings existed has no pad entries, and reads as the unclaimed default.
function loadProfileInto(p){
  platform=p.hasOwnProperty('platform')?p.platform:DEFAULT_PLATFORM;
  // A game we have never saved starts out following the default rather than
  // forking a copy of it: under "off unless a game profile is running" most
  // profiles exist only to turn the controller on, and a fork made for that
  // reason stops tracking the default the moment it is created. A game that
  // has been saved keeps whatever it was saved as, and the default profile
  // itself can never follow anything.
  useDefault = currentGame!=='' && (PROFILES.hasOwnProperty(currentGame)
                                      ? p.useDefault==='1'
                                      : true);
  bindings={};
  ROWS.forEach(function(r){
    bindings[r.id]=p.hasOwnProperty(r.id)?p[r.id]:DEFAULTS[r.id];
  });
  modes={}; dirs={}; diags={}; speeds={};
  PADS.forEach(function(x){
    modes[x]=p.hasOwnProperty(x+'mode')?p[x+'mode']:DEFAULT_MODES[x];
    dirs[x] =p.hasOwnProperty(x+'dir') ?p[x+'dir'] :DEFAULT_DIRS[x];
    diags[x]=p.hasOwnProperty(x+'diag')?p[x+'diag']:DEFAULT_DIAGS[x];
    // clampSpeed turns a missing key into the default, so this needs no
    // hasOwnProperty guard of its own.
    speeds[x]=clampSpeed(x,p[x+'speed']);
  });
  savedProfile=currentProfile();
}
// Commit the current state to the selected profile. Split out from the
// Apply button because the unsaved-changes prompt saves without the button's
// transient "Applied" feedback.
function saveCurrent(){
  var snapshot=currentProfile();
  var p={}; for(var k in PROFILES) p[k]=PROFILES[k];
  p[currentGame]=snapshot;
  PROFILES=p;
  savedProfile=snapshot;
  var msg=currentProfile();
  msg.type='apply';
  msg.game=currentGame;
  postMsg(msg);
  renderCombo();   // a newly saved game becomes a pinned entry
  renderInherit(); // and gains a profile that can now be removed
}

// ---- Unsaved-changes guard ----
function isDirty(){
  if(!savedProfile) return false;
  var cur=currentProfile();
  for(var k in cur) if(cur[k]!==savedProfile[k]) return true;
  return false;
}
// Run `action`, but if the current profile has unsaved edits, ask first.
// Every path that would abandon those edits \u2014 switching selection, closing
// the window \u2014 goes through here rather than duplicating the prompt.
// One modal serves both prompts rather than two sharing the screen: Escape,
// the backdrop and the keydown handler all have to know about exactly one
// thing that can sit on top of everything else, and a second backdrop would
// have to be taught to every one of them.
function showModal(mode,title,text){
  var del=(mode==='delete');
  document.getElementById('modal-title').textContent=title;
  document.getElementById('modal-text').textContent=text;
  document.getElementById('btn-modal-discard').style.display=del?'none':'';
  document.getElementById('btn-modal-save').style.display=del?'none':'';
  document.getElementById('btn-modal-delete').style.display=del?'':'none';
  document.getElementById('modal-backdrop').style.display='flex';
}
function guard(action){
  if(!isDirty()){action();return;}
  pendingAction=action;
  var name=currentGame===''?'the default profile':(gameName(currentGame)||'this game');
  showModal('unsaved','Unsaved changes',
            'You have unsaved changes to '+name+'. Save them before continuing?');
}
function confirmRemove(){
  if(currentGame==='') return;  // the default profile is not removable
  showModal('delete','Delete this profile?',
            'The profile for '+(gameName(currentGame)||'this game')+
            ' will be deleted. It will go back to default behavior.');
}
function resolveModal(choice){
  document.getElementById('modal-backdrop').style.display='none';
  var action=pendingAction;
  pendingAction=null;
  if(choice==='cancel') return;
  // Delete opens on its own rather than in front of a pending action, so there
  // is never one waiting behind it to run afterwards.
  if(choice==='delete'){removeCurrent();return;}
  if(choice==='save') saveCurrent();
  if(action) action();
}
// Drop the selected game's profile and fall back to the default. Edits in
// flight go with it — the user just said to throw the profile away, so asking
// again about unsaved changes to it would be asking about nothing.
function removeCurrent(){
  if(currentGame==='') return;
  var gone=currentGame;
  var p={}; for(var k in PROFILES) if(k!==gone) p[k]=PROFILES[k];
  PROFILES=p;
  // An entry that existed only because a profile pointed at it goes with the
  // profile. Left behind it stops being pinned, and the only section it could
  // then appear in is the one labelled INSTALLED GAMES, which it never was.
  if(gameMissing(gone))
    GAMES=GAMES.filter(function(g){return g.id!==gone;});
  postMsg({type:'delete',game:gone});
  selectGame('');
  renderCombo();  // it stops being a pinned entry
}
)HTML"
// Second split, same C2026 limit as above — the script outgrew one literal
// once per-game profiles landed. The picker and everything below it live
// here; keep new script on whichever of the two halves is smaller.
R"HTML(
// ---- Per-game picker (searchable combo box) ----
function gameName(gameId){
  for(var i=0;i<GAMES.length;i++) if(GAMES[i].id===gameId) return GAMES[i].name;
  return '';
}
function currentLabel(){
  return currentGame===''?'Default (all other games)':(gameName(currentGame)||'Unknown game');
}
function renderComboLabel(){
  document.getElementById('combo-current').textContent=currentLabel();
}
// Games with a saved profile, plus Default, are pinned: they show with no
// search text, because they are the ones the user has already committed to
// and will come back to. Everything else is reachable only by searching \u2014
// the full list runs to hundreds of entries and scrolling it is not a real
// way to find anything.
function pinnedGames(){
  return GAMES.filter(function(g){return PROFILES.hasOwnProperty(g.id);});
}
function renderCombo(){
  renderComboLabel();
  var results=document.getElementById('combo-results');
  if(!results) return;

  var add=document.getElementById('combo-add');
  // The two "add" actions are the way out of the running-app list, not
  // something to offer inside it.
  if(add) add.style.display=runningOpen?'none':'';

  if(runningOpen){ results.innerHTML=runningHTML(); return; }

  var q=comboQuery.trim().toLowerCase();
  var html='';

  var pinned=pinnedGames();
  if(q) pinned=pinned.filter(function(g){return g.name.toLowerCase().indexOf(q)>=0;});
  var showDefault=!q||'default (all other games)'.indexOf(q)>=0;

  if(showDefault||pinned.length){
    html+='<div class="combo-section-label">PROFILES</div><div class="combo-list">';
    if(showDefault)
      html+=itemHTML('','Default (all other games)');
    pinned.forEach(function(g){html+=itemHTML(g.id,g.name,g.missing==='1');});
    html+='</div>';
  }

  if(q){
    var pinnedIds={};
    pinned.forEach(function(g){pinnedIds[g.id]=true;});
    var matches=GAMES.filter(function(g){
      return !pinnedIds[g.id]&&g.name.toLowerCase().indexOf(q)>=0;
    }).slice(0,50);  // a two-letter query can match hundreds; cap the DOM work
    if(matches.length){
      html+='<div class="combo-section-label">FOUND ON THIS PC</div><div class="combo-list">';
      matches.forEach(function(g){html+=itemHTML(g.id,g.name,g.missing==='1');});
      html+='</div>';
    } else if(!pinned.length&&!showDefault){
      // The moment the user finds out their game is missing. The two actions
      // below the results are what answers it, so say so rather than leaving
      // them at a dead end.
      html+='<div class="combo-empty">'+(gamesPending
        ?'Still looking for your games&#8230;'
        :'Nothing matches "'+escapeHTML(comboQuery)+'".<br>Add it yourself below.')+'</div>';
    }
  } else if(gamesPending){
    html+='<div class="combo-note">Looking for your games&#8230;</div>';
  }

  results.innerHTML=html;
}
// The applications with a window open right now. Offered because the game a
// user wants a profile for is very often the one they just alt-tabbed out of,
// and this asks them to know nothing about where it was installed.
function runningHTML(){
  var html='<div class="combo-back" onclick="closeRunning()">&#8592; Back</div>'
          +'<div class="combo-section-label">RUNNING NOW</div>';
  if(!RUNNING.length)
    return html+'<div class="combo-empty">Nothing else is running that we can see.</div>';
  html+='<div class="combo-list">';
  RUNNING.forEach(function(a){
    // The executable name is shown as well as the friendly one: two windows
    // can describe themselves identically, and this is what tells them apart.
    html+='<div class="combo-item" onclick="pickRunning(\''+a.i+'\')">'
        +escapeHTML(a.name)+'<span class="exe">'+escapeHTML(a.exe)+'</span></div>';
  });
  return html+'</div>';
}
function itemHTML(id,name,missing){
  var cls='combo-item'+(id===currentGame?' active':'');
  var badge=missing?'<span class="combo-badge">NOT INSTALLED</span>':'';
  return '<div class="'+cls+'" onclick="pickGame(\''+id+'\')">'+escapeHTML(name)+badge+'</div>';
}
function askRunningApps(){ postMsg({type:'listRunning'}); }
function askBrowseExe(){
  // The file dialog is modal to this window, so the panel would otherwise sit
  // open behind it and still be open when the dialog closes.
  closeCombo();
  postMsg({type:'browseExe'});
}
function pickRunning(token){ postMsg({type:'pickRunning',app:token}); }
function closeRunning(){ runningOpen=false; renderCombo(); }
// True for a picker entry that exists only because a profile refers to it.
function gameMissing(gameId){
  for(var i=0;i<GAMES.length;i++) if(GAMES[i].id===gameId) return GAMES[i].missing==='1';
  return false;
}
function escapeHTML(s){
  return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;')
                  .replace(/>/g,'&gt;').replace(/"/g,'&quot;').replace(/'/g,'&#39;');
}
function openCombo(){
  comboOpen=true;
  comboQuery='';
  runningOpen=false;
  document.getElementById('combo-search').value='';
  document.getElementById('combo-panel').style.display='block';
  renderCombo();
  document.getElementById('combo-search').focus();
}
function closeCombo(){
  comboOpen=false;
  runningOpen=false;
  var panel=document.getElementById('combo-panel');
  if(panel) panel.style.display='none';
}
function toggleCombo(){
  if(comboOpen) closeCombo(); else openCombo();
}
function pickGame(gameId){
  closeCombo();
  if(gameId===currentGame) return;
  guard(function(){selectGame(gameId);});
}
function selectGame(gameId){
  currentGame=gameId;
  // A game with no profile of its own starts from the default's settings \u2014
  // loadProfileInto copies, so editing here cannot mutate the cached profile.
  loadProfileInto(PROFILES[currentGame]||PROFILES['']||{});
  cancelListening();
  renderComboLabel();
  renderModeSelects();
  renderAll();
}
// ---- Movement Mode / Scroll Direction dropdowns ----
function fillSelect(id,options,value){
  var sel=document.getElementById(id);
  if(!sel) return;
  sel.innerHTML='';
  options.forEach(function(o){
    var opt=document.createElement('option');
    opt.value=o.id;
    opt.textContent=o.label;
    sel.appendChild(opt);
  });
  sel.value=value;
}
function setPlatform(value){
  platform=value;
  // The DS4 Touchpad option's label depends on this, so the pad dropdowns
  // have to be rebuilt rather than left as they are.
  renderModeSelects();
}
// The checkbox belongs to a game, never to the default profile, and while it
// is ticked everything below it describes controls this game is not using —
// so the settings are dimmed and inert, and "reset this profile" with them.
// Whether the controller is actually being driven, and whether this window is
// allowed to change that. Both arrive from the app; assume off until told, so
// a page that somehow never hears warns rather than staying quiet about it.
var controlOn = false, controlManual = true;
// Every setting in this window does nothing while Steamless mode is off. The
// event log has said so for a while, which turned out to be no use at all —
// people are looking at this window, not at a log, and three separate test
// sessions were spent changing settings that were never connected to anything.
function renderOffbar(){
  var bar=document.getElementById('offbar');
  if(!bar) return;
  bar.className = controlOn ? 'offbar' : 'offbar on';
  if(controlOn) return;
  var text=document.getElementById('offbar-text');
  var btn=document.getElementById('offbar-btn');
  // Deliberately says nothing about whether the settings are saved. An earlier
  // draft ended "they are saved either way", which reads as though the page
  // saves on its own — and it does not; Apply does. A banner about one thing
  // being off is the wrong place to imply something else is automatic.
  if(controlManual){
    text.innerHTML='<b>Steamless mode is off.</b> Nothing on this page is '+
                   'driving your controller yet - the trackpads, paddles '+
                   'and buttons below take effect once it is on.';
    if(btn) btn.style.display='';
  }else{
    // Not ours to switch on: an auto mode owns the decision, and a button here
    // would either lie or fight it a moment later.
    text.innerHTML='<b>Steamless mode is off right now.</b> Control Mode is set '+
                   'to one of the automatic options, so it turns on by itself '+
                   'when the conditions are met. Settings below take effect then.';
    if(btn) btn.style.display='none';
  }
}
function renderInherit(){
  var group=document.getElementById('inherit-group');
  if(group) group.style.display=(currentGame==='')?'none':'';
  var box=document.getElementById('use-default');
  if(box) box.checked=useDefault;
  var settings=document.getElementById('settings');
  if(settings) settings.className=(currentGame!==''&&useDefault)?'inherited':'';
  var reset=document.getElementById('btn-reset');
  if(reset) reset.disabled=(currentGame!==''&&useDefault);
  // Only offered for a game that actually has something saved — a game merely
  // being looked at has no profile to delete. Hidden rather than disabled so
  // the footer holds nothing red at all for the default profile.
  var del=document.getElementById('btn-delete');
  if(del) del.style.display=
    (currentGame!==''&&PROFILES.hasOwnProperty(currentGame))?'':'none';
  // Says why this game is reachable at all when it is not on the PC, and that
  // its profile is being kept rather than quietly ignored.
  var miss=document.getElementById('missing-note');
  if(miss) miss.style.display=(currentGame!==''&&gameMissing(currentGame))?'':'none';
}
function renderModeSelects(){
  renderInherit();
  fillSelect('platform',PLATFORM_OPTIONS,platform);
  var opts=modeOptions();
  PADS.forEach(function(padId){
    fillSelect('mode-'+padId,opts,modes[padId]);
    fillSelect('dir-'+padId,DIR_OPTIONS,dirs[padId]);
    fillSelect('diag-'+padId,DIAG_OPTIONS,diags[padId]);
    var speedSel=document.getElementById('speed-'+padId);
    if(speedSel){
      // The slider travels over stop INDEXES, which is what makes the spacing
      // ratio-even rather than linear in the percentage.
      speedSel.min=0; speedSel.max=SPEED_STOPS.length-1; speedSel.step=1;
      speedSel.value=SPEED_STOPS.indexOf(clampSpeed(padId,speeds[padId]));
    }
    // Goes through setSpeed so the readout beside the slider is written by the
    // one function that owns it, rather than by two that can drift apart.
    setSpeed(padId,speeds[padId]);
  });
  renderPadRows();
}
// Show only the rows the current mode actually has settings for.
function renderPadRows(){
  PADS.forEach(function(padId){
    var mode=modes[padId];
    var shown=PAD_ROWS_BY_MODE[mode]||[];
    var dirRow=document.getElementById('dir-row-'+padId);
    if(dirRow) dirRow.style.display=(mode==='scroll')?'':'none';
    var speedRow=document.getElementById('speed-row-'+padId);
    if(speedRow) speedRow.style.display=(mode==='scroll')?'':'none';
    var diagRow=document.getElementById('diag-row-'+padId);
    if(diagRow) diagRow.style.display=(mode==='dpad')?'':'none';
    // Only a directional pad splits its surface, so only it needs explaining.
    var note=document.getElementById('note-'+padId);
    if(note) note.style.display=(mode==='dpad')?'':'none';
    PAD_SUFFIXES.forEach(function(suffix){
      var row=document.getElementById('row-'+padId+suffix);
      if(row) row.style.display=(shown.indexOf(suffix)>=0)?'':'none';
    });
  });
}
)HTML"
// Picker and dropdowns above, row rendering and static wiring below.
R"HTML(
// ---- Render helpers ----
// Resolves a binding id to something renderable. Keys aren't in the static
// catalog, so they're built on the fly from the label C++ sent.
function inputInfo(id){
  if(byId[id]) return byId[id];
  if(id&&id.indexOf('key:')===0){
    var lbl=keyLabels[id]||'Key';
    return {id:id,glyph:(lbl.length<=3?lbl:'KEY'),label:lbl,bg:'#4a3524',fg:'#f2d5ab'};
  }
  return byId['none'];
}
function chipHTML(t,cls){
  return '<div class="'+cls+'" style="background:'+t.bg+';color:'+t.fg+';">'+t.glyph+'</div>';
}
function renderRow(def){
  var el=document.getElementById('row-'+def.id);
  if(!el) return;
  var isL=listening===def.id, isF=flash===def.id;
  el.className='row'+(isL?' listening':isF?' flash':'');
  var curBind=bindings[def.id];
  var tgt=inputInfo(curBind);
  var rightHTML;
  if(isL){
    rightHTML='<div class="listen-right">'+
      '<span class="listen-pill"><span class="pulse-dot"></span>Press a button, key, or mouse button...</span>'+
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
    var modPills=MODIFIER_LIST.map(function(m){
      var on=(pendingMods&m.bit)!==0;
      return '<button class="mod-pill'+(on?' on':'')+'" '+
        'onclick="toggleMod('+m.bit+')">'+m.label+'</button>';
    }).join('');
    pickerHTML='<div class="picker">'+
      '<div class="picker-label">HOLD WITH THE KEY(S)</div>'+
      '<div class="picker-row" style="margin-top:9px;">'+modPills+'</div>'+
      (sasWarning
        ? '<div class="mod-note">Windows reserves Ctrl + Alt + Delete for itself &#8212; '
          + 'no application can send it, so it cannot be bound here.</div>'
        : '<div class="mod-note">Pick modifiers, then press the key.</div>')+
      '<div class="picker-label" style="margin-top:14px;">OR PICK MANUALLY</div>'+
      '<div class="picker-rows">'+pickerRows+'</div></div>';
  }
  el.innerHTML='<div class="row-top">'+
    '<div class="badge">'+
      '<span class="badge-id">'+(def.badge||def.id)+'</span>'+
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
document.getElementById('btn-close').onclick=function(){guard(function(){postMsg({type:'close'});});};
document.getElementById('btn-reset').onclick=resetDefaults;
document.getElementById('btn-apply').onclick=applyBindings;
var offBtn=document.getElementById('offbar-btn');
if(offBtn) offBtn.onclick=function(){
  // No optimistic hiding: enabling can fail (no ViGEm, a device another
  // process will not release) and a banner that vanished on click would claim
  // a success nothing verified. The app pushes the real state when it knows.
  postMsg({type:'requestEnable'});
};

document.getElementById('use-default').addEventListener('change',function(e){
  useDefault=e.target.checked;
  // Unticking must stop any capture already running underneath the dimmed
  // rows, and reveals whatever controls this game had stored all along.
  cancelListening();
  renderInherit();
});
document.getElementById('platform').addEventListener('change',function(e){
  setPlatform(e.target.value);
});
PADS.forEach(function(padId){
  var modeSel=document.getElementById('mode-'+padId);
  if(modeSel) modeSel.addEventListener('change',function(e){setMode(padId,e.target.value);});
  var dirSel=document.getElementById('dir-'+padId);
  if(dirSel) dirSel.addEventListener('change',function(e){setDir(padId,e.target.value);});
  var diagSel=document.getElementById('diag-'+padId);
  if(diagSel) diagSel.addEventListener('change',function(e){setDiag(padId,e.target.value);});
  var speedSel=document.getElementById('speed-'+padId);
  // 'input' rather than 'change': the readout has to follow the thumb while it
  // is being dragged, not only when it is let go.
  // The slider's value is a stop index; setSpeed wants the percentage.
  if(speedSel) speedSel.addEventListener('input',function(e){
    setSpeed(padId,SPEED_STOPS[parseInt(e.target.value,10)]);
  });
});

// mousedown as well as click: the document-level handler below closes the
// panel on mousedown, which lands before this button's click — without this
// the toggle would close and immediately reopen, never appearing to shut.
document.getElementById('combo-toggle').addEventListener('mousedown',function(e){
  e.stopPropagation();
});
document.getElementById('combo-toggle').onclick=function(e){
  e.stopPropagation();
  toggleCombo();
};
document.getElementById('combo-search').addEventListener('input',function(e){
  comboQuery=e.target.value;
  // Typing is a search of the installed list, so it leaves the running-app
  // view rather than filtering something the box does not describe.
  runningOpen=false;
  renderCombo();
});
// Clicks inside the panel must not reach the document handler below, which
// closes it — that would fire before an item's own onclick could run.
document.getElementById('combo-panel').addEventListener('mousedown',function(e){
  e.stopPropagation();
});
document.addEventListener('mousedown',function(){
  if(comboOpen) closeCombo();
});

document.getElementById('btn-modal-cancel').onclick=function(){resolveModal('cancel');};
document.getElementById('btn-modal-discard').onclick=function(){resolveModal('discard');};
document.getElementById('btn-modal-save').onclick=function(){resolveModal('save');};
document.getElementById('btn-modal-delete').onclick=function(){resolveModal('delete');};
document.getElementById('btn-delete').onclick=confirmRemove;

// Title bar drag: mousedown on titlebar (but not on buttons) tells C++ to start a window move.
document.getElementById('titlebar').addEventListener('mousedown',function(e){
  if(e.target.closest('.winctls')) return;
  if(e.button!==0) return;
  postMsg({type:'startDrag'});
});

// Middle and the two thumb buttons can be captured by pressing them, because
// none of them is needed to operate this window. Left and right deliberately
// cannot — the user has to keep clicking Rebind and Cancel — so they stay
// available from the picker below instead.
var MOUSE_CAPTURE = {1:'mouse:middle', 3:'mouse:x1', 4:'mouse:x2'};
document.addEventListener('mousedown',function(e){
  if(!listening||comboOpen) return;
  var id=MOUSE_CAPTURE[e.button];
  if(!id) return;
  // Suppress the browser defaults these carry: autoscroll on middle, and
  // history navigation on the thumb buttons.
  e.preventDefault();
  e.stopPropagation();
  postMsg({type:'mouseCaptured',button:id});
});
document.addEventListener('auxclick',function(e){
  if(listening&&MOUSE_CAPTURE[e.button]) e.preventDefault();
});

document.addEventListener('keydown',function(e){
  // Escape stays the cancel affordance, so it is deliberately not bindable.
  // Innermost dismissable thing first: the modal is the only one that can sit
  // on top of the others.
  if(e.key==='Escape'){
    if(pendingAction!==null||document.getElementById('modal-backdrop').style.display==='flex'){
      resolveModal('cancel');
    } else if(comboOpen){
      closeCombo();
    } else {
      cancelListening();
    }
    return;
  }
  // The search box is a real text field — typing in it must not be swallowed
  // by the binding-capture handler below.
  if(comboOpen) return;
  if(!listening||e.repeat) return;
  // Swallow the key so it can't also drive the page (Tab moving focus, Space
  // clicking the focused button) while a row is listening.
  e.preventDefault();
  e.stopPropagation();

  // A modifier on its own is the start of a combination far more often than
  // it is the whole binding, so pressing one arms its toggle and waits to see
  // what follows. Releasing it without pressing anything else binds the bare
  // modifier — see the keyup handler below, which is what keeps a paddle
  // bound to plain Ctrl possible.
  if(MODIFIER_CODES[e.code]){
    pendingMods|=MODIFIER_CODES[e.code];
    modsPressed|=MODIFIER_CODES[e.code];
    renderAll();
    return;
  }

  // Ctrl+Alt+Delete is the Secure Attention Sequence. Windows will not let any
  // ordinary process synthesize it, so binding it would produce a paddle that
  // silently does nothing.
  if(e.code==='Delete'&&(pendingMods&MOD.CTRL)&&(pendingMods&MOD.ALT)){
    sasWarning=true;
    renderAll();
    return;
  }

  sendCapture(e.code, pendingMods|liveMods(e));
});

// A modifier released without anything pressed while it was down is the whole
// binding, not the start of one. Any other modifiers still armed ride along,
// so holding Ctrl and tapping Shift binds Ctrl+Shift.
document.addEventListener('keyup',function(e){
  if(!listening||captureSent) return;
  var bit=MODIFIER_CODES[e.code];
  if(!bit||!(modsPressed&bit)) return;
  e.preventDefault();
  e.stopPropagation();
  sendCapture(e.code, pendingMods&~bit);
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

    case WM_NCCALCSIZE: {
        // WS_THICKFRAME reserves a sizing frame on all four sides — 7 logical
        // pixels of it. Windows draws the left, right and bottom ones as
        // nothing at all, but paints the top, which is the pale band that sat
        // above our own title bar and belonged to no part of this design.
        //
        // Giving the top back to the client area is what removes it: the page
        // then reaches the very top of the window and there is no frame left
        // there to paint. The other three sides are deliberately left alone —
        // they cost nothing visually and they are what Windows snaps, sizes
        // and casts the drop shadow from.
        if (!wp) break;  // the RECT-only form; nothing to reclaim
        // A maximized window is handed a rect that already overhangs the
        // monitor by the frame on every side, and Windows trims it back to
        // the work area. Reclaiming the top there would push the title bar
        // off-screen — or under a taskbar docked at the top — so the frame
        // is left exactly as Windows computed it. Reachable by snapping:
        // this window has no maximize button, but Win+Up does not need one.
        if (IsZoomed(hwnd)) break;
        auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
        const LONG frameTop = params->rgrc[0].top;
        const LRESULT result = DefWindowProcW(hwnd, msg, wp, lp);
        if (result != 0) return result;  // Windows wants the client moved; defer
        params->rgrc[0].top = frameTop;
        return 0;
    }

    case WM_NCHITTEST: {
        // Let Windows handle non-client areas (resize border, etc.) first.
        LRESULT hit = DefWindowProcW(hwnd, msg, wp, lp);

        const POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        const UINT  dpi = GetDpiForWindow(hwnd);

        // The top frame is client area now (see WM_NCCALCSIZE), so Windows no
        // longer reports a resize target there and would let the title bar
        // below claim it as somewhere to drag from. Restoring it by hand is
        // the price of reclaiming those pixels, and it has to come before the
        // caption test for the same reason.
        RECT window{};
        GetWindowRect(hwnd, &window);
        const int grip = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi)
                       + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        // Worked out from the window rect rather than from what
        // DefWindowProc said: with the top no longer part of the frame it
        // stops reporting HTLEFT/HTRIGHT in the top corners, so asking it
        // would cost the two diagonal resize grips.
        if (pt.y < window.top + grip) {
            if (pt.x <  window.left  + grip) return HTTOPLEFT;
            if (pt.x >= window.right - grip) return HTTOPRIGHT;
            return HTTOP;
        }

        if (hit == HTCLIENT) {
            POINT local = pt;
            ScreenToClient(hwnd, &local);
            RECT rc;
            GetClientRect(hwnd, &rc);
            // The right ~96px of the title bar hosts our close/min buttons —
            // leave those as HTCLIENT so WebView2 can receive the clicks.
            int tbPx  = MulDiv(46, dpi, 96);   // CSS 46px → physical pixels
            int ctlPx = MulDiv(96, dpi, 96);
            if (local.y < tbPx && local.x < rc.right - ctlPx)
                return HTCAPTION;
        }
        return hit;
    }

    case WM_BUTTON_CAPTURED:
        // Marshalled from the read thread via PostMessage, packed into WPARAM.
        PostCapturedBinding(BackButtonBinding::Unpack(static_cast<uint32_t>(wp)));
        return 0;

    case WM_CLOSE:
        // Ask the page first: it owns the in-progress bindings and knows
        // whether they differ from what was saved. It answers with a "close"
        // message once the user has resolved that, which arrives here as
        // WM_CLOSE_CONFIRMED. Without a live webview there is nothing holding
        // unsaved state, so hide immediately.
        if (m_webview) {
            PostToWebView(L"{\"type\":\"closeRequested\"}");
            return 0;
        }
        ShowWindow(hwnd, SW_HIDE);
        if (m_onClose) m_onClose();
        return 0;

    case WM_CLOSE_CONFIRMED:
        ShowWindow(hwnd, SW_HIDE);
        if (m_onClose) m_onClose();
        return 0;

    case WM_GAMES_READY: {
        // Ownership arrives with the message; see StartEnumeration.
        std::unique_ptr<std::vector<InstalledGame>> found(
            reinterpret_cast<std::vector<InstalledGame>*>(lp));
        // A second enumeration can land after the user has already added an
        // application by hand — they can do that before the list arrives, and
        // reopening the window starts another pass. Their entries are kept
        // and re-appended, with their tokens, so a selection made against one
        // still means the same thing afterwards.
        std::vector<PickerEntry> manual;
        for (auto& entry : m_games)
            if (entry.game.source == GameSource::Manual
                && m_gameProfiles.find(entry.game.id) == m_gameProfiles.end())
                manual.push_back(std::move(entry));

        m_games.clear();
        for (auto& game : *found) {
            const size_t token = TokenFor(game.id);
            m_games.push_back({ std::move(game), token });
        }
        AppendOrphanProfiles();
        // Anything the user added that has a profile is already back, put
        // there by AppendOrphanProfiles; only the not-yet-applied ones need
        // carrying over by hand.
        for (auto& entry : manual)
            m_games.push_back(std::move(entry));

        SendGameList();
        return 0;
    }

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

// True for a profile id that names something we can still see on disk. An
// application the user picked by hand is never in the enumerated list, so
// without this every one of them would come back wearing the "not installed"
// badge and an invitation to delete a profile that works. Also rescues an
// ordinary game whose Start Menu shortcut was removed while the game stayed.
static bool IdStillOnDisk(const std::wstring& id) {
    // Only the two path-shaped ids can be checked this way. A "steam://" or
    // "aumid:" id says nothing about the filesystem, and its game being
    // absent from the enumerated list really does mean it is gone.
    const bool isDir = id.rfind(L"dir:", 0) == 0;
    if (!isDir && (id.rfind(L"steam://", 0) == 0 || id.rfind(L"aumid:", 0) == 0))
        return false;

    const std::wstring path = isDir ? id.substr(4) : id;
    if (path.empty()) return false;
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    return isDir == ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0);
}

void RemapWindow::AppendOrphanProfiles() {
    // An enumeration that found nothing at all looks exactly like every game
    // having been uninstalled, and is by far the likelier of the two — a COM
    // failure reading the package list, or a Start Menu walk that came back
    // empty. Flagging every profile as missing on that evidence would invite
    // the user to delete the lot, so say nothing rather than guess.
    if (m_games.empty()) return;

    const size_t enumerated = m_games.size();
    for (const auto& [id, profile] : m_gameProfiles) {
        bool found = false;
        for (size_t i = 0; i < enumerated; ++i) {
            if (_wcsicmp(m_games[i].game.id.c_str(), id.c_str()) == 0) {
                found = true;
                break;
            }
        }
        if (found) continue;

        InstalledGame orphan;
        orphan.id = id;
        // What displayName was stored for: naming a profile at a moment the
        // installed list cannot. Profiles written before it existed fall back
        // to the raw id — poor prose, but it still identifies the game.
        orphan.name   = profile.displayName.empty() ? id : profile.displayName;
        orphan.source = IdStillOnDisk(id) ? GameSource::Manual : GameSource::Missing;
        const size_t token = TokenFor(id);
        m_games.push_back({ std::move(orphan), token });
    }
}

// ---------------------------------------------------------------------------
// Naming an application the installed list does not have
// ---------------------------------------------------------------------------

// "C:\Games\Celeste\Celeste.exe" -> "Celeste.exe".
static std::wstring LeafOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

void RemapWindow::SendRunningApps() {
    if (!m_webview) return;

    // Tokens for these come from the same sequence as the picker's, so a
    // stale "pickRunning" cannot collide with a game's token.
    m_runningApps.clear();
    std::wstring json;
    for (auto& app : GameLibrary::EnumerateRunning()) {
        const size_t token = m_nextToken++;
        if (!json.empty()) json += L",";
        // The executable name is shown alongside the friendly one because two
        // applications can describe themselves identically, and it is what
        // tells them apart.
        json += L"{\"i\":\"" + std::to_wstring(token) + L"\",\"name\":\""
              + JsonEscape(app.name) + L"\",\"exe\":\""
              + JsonEscape(LeafOf(app.id)) + L"\"}";
        m_runningApps.push_back({ std::move(app), token });
    }

    PostToWebView(L"{\"type\":\"runningApps\",\"apps\":[" + json + L"]}");
}

void RemapWindow::BrowseForExe() {
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog))))
        return;

    COMDLG_FILTERSPEC filter[] = { { L"Programs", L"*.exe" } };
    dialog->SetFileTypes(ARRAYSIZE(filter), filter);
    dialog->SetTitle(L"Pick the program to make a profile for");

    // Where games are, far more often than the last folder the user happened
    // to open something from.
    Microsoft::WRL::ComPtr<IShellItem> programFiles;
    if (SUCCEEDED(SHCreateItemInKnownFolder(FOLDERID_ProgramFiles, 0, nullptr,
                                            IID_PPV_ARGS(&programFiles))))
        dialog->SetDefaultFolder(programFiles.Get());

    // Modal to our own window, so it cannot end up behind it.
    if (FAILED(dialog->Show(m_hwnd))) return;  // includes the user cancelling

    Microsoft::WRL::ComPtr<IShellItem> result;
    if (FAILED(dialog->GetResult(&result))) return;

    PWSTR path = nullptr;
    if (FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) {
        if (path) CoTaskMemFree(path);
        return;
    }
    const std::wstring exePath(path);
    CoTaskMemFree(path);

    // The same rule the shortcut walk and the running-application list apply:
    // a profile for part of Windows would fire at moments that have nothing
    // to do with playing a game.
    if (GameLibrary::IsSystemProgram(exePath)) {
        MessageBoxW(m_hwnd,
            L"That program is part of Windows itself.\n\n"
            L"Pick a game or application you installed instead.",
            L"Not a game", MB_OK | MB_ICONINFORMATION);
        return;
    }

    InstalledGame game;
    game.id     = exePath;
    game.source = GameSource::Manual;
    game.name   = GameLibrary::NameForExecutable(exePath);
    AddManualGame(std::move(game));
}

void RemapWindow::AddManualGame(InstalledGame game) {
    if (game.id.empty() || !m_webview) return;

    const size_t token = TokenFor(game.id);

    // Picking something already in the list selects it rather than adding a
    // second copy of it — which is the sensible reading of the action, and
    // stops a browsed exe that turned out to be a listed game from producing
    // two entries that both claim it.
    for (const auto& entry : m_games) {
        if (entry.token != token) continue;
        PostToWebView(L"{\"type\":\"gameAdded\",\"id\":\"" + std::to_wstring(token)
                      + L"\"}");
        return;
    }

    m_games.push_back({ std::move(game), token });
    // Only the list changed — the page keeps whatever the user was editing,
    // and routes selecting the new entry through its own unsaved-changes
    // prompt, exactly as if they had picked it from the list themselves.
    SendGameList();
    PostToWebView(L"{\"type\":\"gameAdded\",\"id\":\"" + std::to_wstring(token) + L"\"}");
}

size_t RemapWindow::TokenFor(const std::wstring& id) {
    auto [it, inserted] = m_tokens.try_emplace(id, m_nextToken);
    if (inserted) ++m_nextToken;
    return it->second;
}

PickerEntry* RemapWindow::EntryForToken(const std::string& value) {
    if (value.empty()) return nullptr;
    size_t token = 0;
    for (char c : value) {
        if (c < '0' || c > '9') return nullptr;
        token = token * 10 + static_cast<size_t>(c - '0');
    }
    for (auto& entry : m_games)
        if (entry.token == token) return &entry;
    return nullptr;
}

void RemapWindow::StartEnumeration() {
    // Detached rather than joined. The only thing it touches afterwards is the
    // window handle, by value, so it cannot outlive anything it refers to; and
    // joining would mean blocking the UI thread on close for exactly as long
    // as running this on the UI thread would have cost in the first place.
    HWND hwnd = m_hwnd;
    std::thread([hwnd] {
        auto games = std::make_unique<std::vector<InstalledGame>>(
            GameLibrary::EnumerateInstalled());

        // Counted per source because "my game isn't in the list" is what this
        // feature gets reported for, and one source coming back empty is
        // nearly always the reason. GameLibraryProbe prints the same summary.
        // Source names are ASCII literals, so narrowing them is exact; done a
        // character at a time to say so explicitly rather than leaning on an
        // implicit conversion the compiler is right to warn about.
        std::map<std::string, size_t> counts;
        for (const auto& g : *games) {
            std::string name;
            for (const wchar_t* c = GameSourceName(g.source); *c; ++c)
                name += static_cast<char>(*c);
            ++counts[name];
        }
        std::string summary;
        for (const auto& [source, count] : counts)
            summary += " " + source + "=" + std::to_string(count);
        EventLog::Write("PROFILE: game picker found %zu app(s):%s",
                        games->size(), summary.empty() ? " none" : summary.c_str());

        // Fails when the window has already gone, which is the whole reason
        // ownership only transfers on success.
        if (PostMessageW(hwnd, WM_GAMES_READY, 0,
                         reinterpret_cast<LPARAM>(games.get())))
            games.release();
    }).detach();
}

void RemapWindow::Open(HINSTANCE hInst, ControllerManager* mgr,
                       const ControllerProfile& cfg,
                       std::map<std::wstring, ControllerProfile> gameProfiles,
                       std::function<void(const std::wstring&, const ControllerProfile&)> applyCallback,
                       std::function<void(const std::wstring&)> deleteCallback)
{
    // If already open, just bring it to front.
    if (m_hwnd) {
        if (IsWindowVisible(m_hwnd)) { BringToFront(); return; }
        // Was hidden. Refresh config and show. The list is rebuilt from
        // scratch rather than kept, because the likeliest thing to have
        // happened since it was last up is that the user installed a game.
        m_config        = cfg;
        m_games.clear();
        m_gameProfiles  = std::move(gameProfiles);
        m_applyCallback = std::move(applyCallback);
        m_deleteCallback = std::move(deleteCallback);
        ShowWindow(m_hwnd, SW_SHOW);
        BringToFront();
        SendInitState();
        StartEnumeration();
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
    m_games.clear();
    m_gameProfiles = std::move(gameProfiles);
    m_applyCallback = std::move(applyCallback);
    m_deleteCallback = std::move(deleteCallback);
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
        0, CLASS_NAME, L"Customize Controls",
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

    // Started after the window exists, since that is what the result is
    // posted back to. Finding every installed application takes well over a
    // second on an ordinary machine — long enough that doing it before the
    // window went up read as the app having hung.
    StartEnumeration();
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
// Key names come from the keyboard layout, so they can contain characters that
// would otherwise terminate or corrupt the JSON string we build by hand — the
// backslash key is the obvious one.
static std::wstring JsonEscape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 8);
    for (wchar_t c : s) {
        switch (c) {
        case L'"':  out += L"\\\""; break;
        case L'\\': out += L"\\\\"; break;
        default:
            if (c >= 0x20) out += c;  // drop control characters
            break;
        }
    }
    return out;
}

// Display label for a binding, or empty when the page's static catalog already
// has a name for it. Only keys need one — their names come from the layout.
static std::wstring BindingLabel(const BackButtonBinding& binding) {
    return binding.kind == BackButtonBinding::Kind::Key
         ? KeyComboDisplayName(binding)
         : std::wstring();
}

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
        // The page has resolved any unsaved-changes prompt — this is the
        // confirmed close, not a fresh request, so it must not route back
        // through WM_CLOSE and ask again.
        PostMessageW(m_hwnd, WM_CLOSE_CONFIRMED, 0, 0);

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

    } else if (type == "mouseCaptured") {
        const BackButtonBinding binding = BackButtonBinding::FromId(JsonStr(msg, "button"));
        if (binding.kind == BackButtonBinding::Kind::MouseButton) {
            if (m_mgr) m_mgr->StopButtonCapture();
            PostCapturedBinding(binding);
        }

    } else if (type == "keyCaptured") {
        // Arrives from the page's keydown handler, already on the UI thread.
        // Unmappable keys are ignored so the row keeps listening rather than
        // binding to nothing.
        const uint16_t vk = VkFromJsCode(JsonStr(msg, "code"));
        if (vk != 0) {
            // The page sends the modifier set it observed, plus whatever the
            // user ticked — including Windows, which cannot arrive any other
            // way because pressing it opens the Start menu before the page
            // ever sees the key.
            uint8_t mods = 0;
            const std::string modStr = JsonStr(msg, "mods");
            for (char c : modStr) {
                if (c < '0' || c > '9') { mods = 0; break; }
                mods = static_cast<uint8_t>(mods * 10 + (c - '0'));
            }
            if (m_mgr) m_mgr->StopButtonCapture();
            PostCapturedBinding(BackButtonBinding::FromKey(vk, mods));
        }

    } else if (type == "stopListening") {
        if (m_mgr) m_mgr->StopButtonCapture();

    } else if (type == "apply") {
        // The page sends one flat object: a binding per row id, plus a mode
        // per pad.
        ControllerProfile cfg;
        // The page sends "0" for the default profile, which cannot follow
        // anything — so this needs no special case for it here.
        cfg.useDefaultMappings = JsonStr(msg, "useDefault") == "1";
        cfg.platform = JsonStr(msg, "platform") == "ps" ? ControllerPlatform::PlayStation
                                                        : ControllerPlatform::Xbox;
        cfg.back.l4 = BackButtonBinding::FromId(JsonStr(msg, "L4"));
        cfg.back.l5 = BackButtonBinding::FromId(JsonStr(msg, "L5"));
        cfg.back.r4 = BackButtonBinding::FromId(JsonStr(msg, "R4"));
        cfg.back.r5 = BackButtonBinding::FromId(JsonStr(msg, "R5"));
        // The reader matches on "key":" including the colon, so the bare pad
        // id cannot be found inside a longer one — "LPAD" does not match
        // "LPADup", and "LPADdir" does not match "LPADdiag".
        auto readPad = [&](const std::string& id, TrackpadSettings& s) {
            s.click     = BackButtonBinding::FromId(JsonStr(msg, id));
            s.mode      = TrackpadModeFromId(JsonStr(msg, id + "mode"));
            s.scrollDir = ScrollDirectionFromId(JsonStr(msg, id + "dir"));
            // strtoul yields 0 for anything unparseable, which is the same
            // "unset" the registry uses and lands on the default.
            s.scrollSpeed = ScrollSpeedFromDword(static_cast<uint32_t>(
                std::strtoul(JsonStr(msg, id + "speed").c_str(), nullptr, 10)));
            s.diagonals = DiagonalModeFromId(JsonStr(msg, id + "diag"));
            s.touch     = BackButtonBinding::FromId(JsonStr(msg, id + "touch"));
            s.up        = BackButtonBinding::FromId(JsonStr(msg, id + "up"));
            s.down      = BackButtonBinding::FromId(JsonStr(msg, id + "down"));
            s.left      = BackButtonBinding::FromId(JsonStr(msg, id + "left"));
            s.right     = BackButtonBinding::FromId(JsonStr(msg, id + "right"));
        };
        readPad("LPAD", cfg.leftPad);
        readPad("RPAD", cfg.rightPad);

        const std::string token = JsonStr(msg, "game");
        if (PickerEntry* entry = EntryForToken(token)) {
            const std::wstring gameId = entry->game.id;
            // Captured here because this is the only place both halves
            // are in hand: the picker knows the friendly name, and
            // nothing downstream re-enumerates the installed list.
            cfg.displayName = entry->game.name;
            m_gameProfiles[gameId] = cfg;
            if (m_applyCallback) m_applyCallback(gameId, cfg);
        } else if (token.empty()) {
            // No token at all is the default profile; an unknown one is a
            // message we did not send and will not act on.
            m_config = cfg;
            if (m_applyCallback) m_applyCallback(L"", cfg);
        }

    } else if (type == "delete") {
        // Never carries an empty token: the page does not offer removal for
        // the default profile, which has to exist for anything else to fall
        // back to.
        if (PickerEntry* entry = EntryForToken(JsonStr(msg, "game"))) {
            const std::wstring gameId = entry->game.id;
            m_gameProfiles.erase(gameId);
            if (m_deleteCallback) m_deleteCallback(gameId);
        }

    } else if (type == "listRunning") {
        SendRunningApps();

    } else if (type == "pickRunning") {
        // The list was captured when it was sent; an application that has
        // closed since simply is not in it any more, which reads to the user
        // as the click doing nothing — better than adding a profile for a
        // window that is gone.
        for (const auto& app : m_runningApps) {
            if (std::to_string(app.token) != JsonStr(msg, "app")) continue;
            AddManualGame(app.game);
            break;
        }

    } else if (type == "requestEnable") {
        // Only ever offered in manual mode, but checked here too: the mode can
        // change while the window is up, and a stale button must not reach
        // past the tray's own rule about who decides.
        if (m_controlManual && m_onRequestEnable) m_onRequestEnable();

    } else if (type == "browseExe") {
        BrowseForExe();
    }
}


// Pushed whenever the tray's view of the world changes, and again when the
// page comes up — the window is usually opened while the state is already
// settled, so waiting for the next change would leave the banner wrong until
// something unrelated happened.
void RemapWindow::SetControlState(bool enabled, bool manual) {
    m_controlEnabled = enabled;
    m_controlManual  = manual;
    if (!m_webview) return;
    PostToWebView(std::wstring(L"{\"type\":\"controlState\",\"enabled\":\"")
                  + (enabled ? L"1" : L"0")
                  + L"\",\"manual\":\"" + (manual ? L"1" : L"0") + L"\"}");
}
void RemapWindow::PostToWebView(const std::wstring& jsonStr) {
    if (m_webview) m_webview->PostWebMessageAsString(jsonStr.c_str());
}

void RemapWindow::PostCapturedBinding(const BackButtonBinding& binding) {
    const std::string id = binding.Id();
    std::wstring json = L"{\"type\":\"buttonCaptured\",\"button\":\"";
    json += std::wstring(id.begin(), id.end());
    json += L"\",\"label\":\"" + JsonEscape(BindingLabel(binding)) + L"\"}";
    PostToWebView(json);
}

// Convert ASCII action ID string to wstring without char→wchar_t narrowing warnings.
static std::wstring Wid(const BackButtonBinding& b) {
    const std::string s = b.Id();
    return std::wstring(s.begin(), s.end());
}

static std::wstring Narrow(const char* s) {
    const std::string v = s;
    return std::wstring(v.begin(), v.end());
}

// One flat object per profile — the page reads it back the same way, and the
// narrow JSON reader on this side cannot descend into nesting.
std::wstring RemapWindow::ProfileJson(const ControllerProfile& p) {
    const auto wid    = &Wid;
    const auto narrow = &Narrow;
    // One pad's rows, under the prefix the page keys them by. The click keeps
    // the bare pad id it has always had, so profiles saved before the other
    // rows existed still read back.
    auto pad = [&](const wchar_t* id, const TrackpadSettings& s) {
        const std::wstring k = id;
        return L"\"" + k + L"\":\"" + wid(s.click) + L"\","
               L"\"" + k + L"mode\":\"" + narrow(TrackpadModeId(s.mode)) + L"\","
               L"\"" + k + L"dir\":\"" + narrow(ScrollDirectionId(s.scrollDir)) + L"\","
               L"\"" + k + L"speed\":\"" + std::to_wstring(s.scrollSpeed) + L"\","
               L"\"" + k + L"diag\":\"" + narrow(DiagonalModeId(s.diagonals)) + L"\","
               L"\"" + k + L"touch\":\"" + wid(s.touch) + L"\","
               L"\"" + k + L"up\":\"" + wid(s.up) + L"\","
               L"\"" + k + L"down\":\"" + wid(s.down) + L"\","
               L"\"" + k + L"left\":\"" + wid(s.left) + L"\","
               L"\"" + k + L"right\":\"" + wid(s.right) + L"\"";
    };
    return L"{\"useDefault\":\""
               + std::wstring(p.useDefaultMappings ? L"1" : L"0")
               + L"\","
               L"\"platform\":\""
               + std::wstring(p.platform == ControllerPlatform::PlayStation ? L"ps" : L"xbox")
               + L"\","
               L"\"L4\":\"" + wid(p.back.l4) + L"\","
               L"\"L5\":\"" + wid(p.back.l5) + L"\","
               L"\"R4\":\"" + wid(p.back.r4) + L"\","
               L"\"R5\":\"" + wid(p.back.r5) + L"\","
             + pad(L"LPAD", p.leftPad) + L","
             + pad(L"RPAD", p.rightPad) + L"}";
}

void RemapWindow::SendInitState() {
    if (!m_webview) return;

    auto wid = &Wid;

    // Key bindings need a label the page cannot derive on its own — their names
    // come from the keyboard layout. Sent as an id→name map so a paddle pair
    // bound to the same key contributes one entry. Covers every profile, not
    // just the default's, so a key bound only in a per-game override still
    // gets a name when that game is selected.
    std::wstring labels;
    auto addLabel = [&](const BackButtonBinding& b) {
        const std::wstring label = BindingLabel(b);
        if (label.empty()) return;
        const std::wstring entry = L"\"" + wid(b) + L"\":\"" + JsonEscape(label) + L"\"";
        if (labels.find(entry) != std::wstring::npos) return;
        if (!labels.empty()) labels += L",";
        labels += entry;
    };
    auto addProfileLabels = [&](const ControllerProfile& p) {
        // Every binding a profile holds, not just the ones a given mode is
        // using: the page names a key by looking it up here, and a direction
        // stored under a pad that is currently a mouse pointer still has to
        // have a name ready for when the mode is switched back.
        for (const auto* pad : { &p.leftPad, &p.rightPad })
            for (const auto* b : { &pad->click, &pad->touch, &pad->up,
                                   &pad->down, &pad->left, &pad->right })
                addLabel(*b);
        for (const auto* b : { &p.back.l4, &p.back.l5, &p.back.r4, &p.back.r5 })
            addLabel(*b);
    };
    addProfileLabels(m_config);
    for (const auto& [id, cfg] : m_gameProfiles)
        addProfileLabels(cfg);

    // Which of the enumeration and WebView2's own startup finishes first is
    // not ours to decide, and both orders happen. If the list got here first
    // it is already in this message and nothing further is coming; saying so
    // is what stops the picker sitting on "Looking for your games" forever
    // waiting for a "games" message that was sent before anything could
    // receive it.
    std::wstring json =
        L"{\"type\":\"init\""
        L",\"pending\":\"" + std::wstring(m_games.empty() ? L"1" : L"0") + L"\""
        L",\"labels\":{" + labels + L"}"
        L",\"games\":[" + GamesJson() + L"]"
        L",\"profiles\":{" + ProfilesJson() + L"}}";
    PostToWebView(json);

    // Sent separately rather than folded into the init message: the same state
    // arrives later whenever it changes, so the page needs one handler for it
    // either way, and having two ways to learn it is how they drift.
    SetControlState(m_controlEnabled, m_controlManual);
}

// Token-based picker ids so the raw game id — possibly non-ASCII, sometimes
// backslash-laden — never has to cross the hand-rolled JSON channel; see
// OnWebMessage's "apply" handling for the other direction.
std::wstring RemapWindow::GamesJson() const {
    std::wstring json;
    for (const auto& entry : m_games) {
        if (!json.empty()) json += L",";
        json += L"{\"id\":\"" + std::to_wstring(entry.token) + L"\",\"name\":\""
              + JsonEscape(entry.game.name) + L"\""
              + (entry.game.source == GameSource::Missing ? L",\"missing\":\"1\"" : L"")
              + L"}";
    }
    return json;
}

// The default plus every game that already has a saved override. A listed
// game with no entry here simply has none yet — the page falls back to the
// default profile's bindings when it is first selected.
std::wstring RemapWindow::ProfilesJson() const {
    std::wstring json = L"\"\":" + ProfileJson(m_config);
    for (const auto& entry : m_games) {
        auto it = m_gameProfiles.find(entry.game.id);
        if (it == m_gameProfiles.end()) continue;
        json += L",\"" + std::to_wstring(entry.token) + L"\":" + ProfileJson(it->second);
    }
    return json;
}

// Everything init carries except which profile is selected, because this is
// sent while the user may already be part-way through editing one.
void RemapWindow::SendGameList() {
    if (!m_webview) return;
    PostToWebView(L"{\"type\":\"games\""
                  L",\"games\":[" + GamesJson() + L"]"
                  L",\"profiles\":{" + ProfilesJson() + L"}}");
}
