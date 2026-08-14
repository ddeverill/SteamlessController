#include "RemapWindow.h"
#include "KeyInput.h"
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
.mode-row{padding-top:14px;padding-bottom:14px;}
.mode-select{background:#101925;border:1px solid rgba(255,255,255,.12);border-radius:6px;color:#fff;font-size:13px;font-family:'Barlow',system-ui,sans-serif;padding:7px 10px;font-weight:600;cursor:pointer;flex:none;min-width:215px;}
.mode-select:hover{border-color:#66c0f4;}
.mode-select:focus{outline:none;border-color:#66c0f4;}
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
.btn-save{padding:8px 18px;border-radius:4px;border:1px solid #4c7a1d;background:linear-gradient(to bottom,#94c63d,#5d8a1f);color:#13260a;font-weight:700;font-size:13px;}
.btn-save:hover{opacity:.9;}
</style>
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
  <div class="group">
    <div class="group-label">APPLY TO</div>
    <div class="combo-wrap">
      <button class="combo-toggle" id="combo-toggle">
        <span id="combo-current">Default (all other games)</span>
        <span class="chev">&#9660;</span>
      </button>
      <div class="combo-panel" id="combo-panel" style="display:none;">
        <input class="combo-search" id="combo-search" type="text" placeholder="Search for a game..." autocomplete="off">
        <div id="combo-results"></div>
      </div>
    </div>
  </div>
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
    <div id="row-LPAD" class="row"></div>
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
    <div id="row-RPAD" class="row"></div>
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
    <button class="btn-reset" id="btn-reset">Reset this profile to default</button>
    <button class="btn-apply" id="btn-apply">Apply</button>
  </div>
</div>

<div id="modal-backdrop" style="display:none;">
  <div class="modal">
    <h3>Unsaved changes</h3>
    <p id="modal-text">You have unsaved changes to this profile.</p>
    <div class="modal-btns">
      <button class="btn-modal-cancel" id="btn-modal-cancel">Cancel</button>
      <button class="btn-discard" id="btn-modal-discard">Discard</button>
      <button class="btn-save" id="btn-modal-save">Save</button>
    </div>
  </div>
</div>
)HTML"
// Split here only to stay under MSVC's ~16KB limit on a single string literal
// (C2026); the pieces are concatenated back into one document. Markup and
// styles above, script below — keep any new content on whichever side is
// smaller rather than rejoining these.
R"HTML(
<script>
'use strict';
// ---- Defaults ----
// Upper paddles left-click, lower paddles unbound; the right pad points and
// clicks while the left pad scrolls. Keep in sync with ControllerProfile in
// TrackpadConfig.h — these two are what "default" means, and a fresh install
// and this window's reset button both have to land on the same thing.
var DEFAULTS = {L4:'leftMouse',L5:'none',R4:'leftMouse',R5:'none',LPAD:'none',RPAD:'leftMouse'};
var DEFAULT_MODES = {LPAD:'scroll',RPAD:'pointer'};
var DEFAULT_DIRS  = {LPAD:'natural',RPAD:'natural'};
var DEFAULT_PLATFORM = 'xbox';

// ---- State ----
var bindings = {L4:'leftMouse',L5:'none',R4:'leftMouse',R5:'none',LPAD:'none',RPAD:'none'};
// Trackpad movement modes, keyed by the same row ids their click rows use.
var modes = {LPAD:'none',RPAD:'none'};
// Scroll direction per pad. Only meaningful in scroll mode, but kept for
// every pad so switching modes back and forth does not lose the choice.
var dirs = {LPAD:'natural',RPAD:'natural'};
var platform = 'xbox';
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
  ];
}
var DIR_OPTIONS = [
  {id:'natural',  label:'Natural'},
  {id:'reversed', label:'Reversed'},
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
// Per-game picker. Game ids are small indices ("0","1",...) into GAMES, not
// the raw exe path — keeps the hand-rolled JSON channel back to C++ free of
// Unicode and backslash escaping. "" is the always-present default profile.
var GAMES = [];
// Each profile is a flat object: one entry per bindable row id, plus
// "<pad>mode" for each trackpad's movement mode. Flat because the JSON
// reader on the C++ side matches "key":"value" pairs without nesting.
var PROFILES = {'':{platform:'xbox',L4:'leftMouse',L5:'none',R4:'leftMouse',R5:'none',LPAD:'none',RPAD:'leftMouse',LPADmode:'scroll',RPADmode:'pointer',LPADdir:'natural',RPADdir:'natural'}};
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
  ['none'],
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
];
// The pads, and the dropdown id each one's Movement Mode lives in.
var PADS = ['LPAD','RPAD'];

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
      currentGame='';
      loadProfileInto(PROFILES['']||{});
      closeCombo();
      renderComboLabel();
      renderModeSelects();
      renderAll();
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
  // The rows below the dropdown depend on the mode: Scroll Direction only
  // applies to scrolling, and a DS4 touchpad's click is the touchpad press
  // rather than anything rebindable.
  if(listening===padId&&modeId==='ds4') cancelListening();
  renderPadRows();
}
function setDir(padId,dirId){
  dirs[padId]=dirId;
}
function resetDefaults(){
  cancelListening();
  platform=DEFAULT_PLATFORM;
  bindings={};
  ROWS.forEach(function(r){bindings[r.id]=DEFAULTS[r.id];});
  modes={}; dirs={};
  PADS.forEach(function(p){modes[p]=DEFAULT_MODES[p];dirs[p]=DEFAULT_DIRS[p];});
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
  var p={platform:platform};
  ROWS.forEach(function(r){p[r.id]=bindings[r.id];});
  PADS.forEach(function(x){p[x+'mode']=modes[x];p[x+'dir']=dirs[x];});
  return p;
}
// Inverse of currentProfile: adopt a stored profile as the live state, filling
// in defaults for anything it does not carry. A profile saved before trackpad
// settings existed has no pad entries, and reads as the unclaimed default.
function loadProfileInto(p){
  platform=p.hasOwnProperty('platform')?p.platform:DEFAULT_PLATFORM;
  bindings={};
  ROWS.forEach(function(r){
    bindings[r.id]=p.hasOwnProperty(r.id)?p[r.id]:DEFAULTS[r.id];
  });
  modes={}; dirs={};
  PADS.forEach(function(x){
    modes[x]=p.hasOwnProperty(x+'mode')?p[x+'mode']:DEFAULT_MODES[x];
    dirs[x] =p.hasOwnProperty(x+'dir') ?p[x+'dir'] :DEFAULT_DIRS[x];
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
  renderCombo();  // a newly saved game becomes a pinned entry
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
function guard(action){
  if(!isDirty()){action();return;}
  pendingAction=action;
  var name=currentGame===''?'the default profile':(gameName(currentGame)||'this game');
  document.getElementById('modal-text').textContent=
    'You have unsaved changes to '+name+'. Save them before continuing?';
  document.getElementById('modal-backdrop').style.display='flex';
}
function resolveModal(choice){
  document.getElementById('modal-backdrop').style.display='none';
  var action=pendingAction;
  pendingAction=null;
  if(choice==='cancel') return;
  if(choice==='save') saveCurrent();
  if(action) action();
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

  var q=comboQuery.trim().toLowerCase();
  var html='';

  var pinned=pinnedGames();
  if(q) pinned=pinned.filter(function(g){return g.name.toLowerCase().indexOf(q)>=0;});
  var showDefault=!q||'default (all other games)'.indexOf(q)>=0;

  if(showDefault||pinned.length){
    html+='<div class="combo-section-label">PROFILES</div><div class="combo-list">';
    if(showDefault)
      html+=itemHTML('','Default (all other games)');
    pinned.forEach(function(g){html+=itemHTML(g.id,g.name);});
    html+='</div>';
  }

  if(q){
    var pinnedIds={};
    pinned.forEach(function(g){pinnedIds[g.id]=true;});
    var matches=GAMES.filter(function(g){
      return !pinnedIds[g.id]&&g.name.toLowerCase().indexOf(q)>=0;
    }).slice(0,50);  // a two-letter query can match hundreds; cap the DOM work
    if(matches.length){
      html+='<div class="combo-section-label">INSTALLED GAMES</div><div class="combo-list">';
      matches.forEach(function(g){html+=itemHTML(g.id,g.name);});
      html+='</div>';
    } else if(!pinned.length&&!showDefault){
      html+='<div class="combo-empty">No games match "'+escapeHTML(comboQuery)+'"</div>';
    }
  }

  results.innerHTML=html;
}
function itemHTML(id,name){
  var cls='combo-item'+(id===currentGame?' active':'');
  return '<div class="'+cls+'" onclick="pickGame(\''+id+'\')">'+escapeHTML(name)+'</div>';
}
function escapeHTML(s){
  return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;')
                  .replace(/>/g,'&gt;').replace(/"/g,'&quot;').replace(/'/g,'&#39;');
}
function openCombo(){
  comboOpen=true;
  comboQuery='';
  document.getElementById('combo-search').value='';
  document.getElementById('combo-panel').style.display='block';
  renderCombo();
  document.getElementById('combo-search').focus();
}
function closeCombo(){
  comboOpen=false;
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
function renderModeSelects(){
  fillSelect('platform',PLATFORM_OPTIONS,platform);
  var opts=modeOptions();
  PADS.forEach(function(padId){
    fillSelect('mode-'+padId,opts,modes[padId]);
    fillSelect('dir-'+padId,DIR_OPTIONS,dirs[padId]);
  });
  renderPadRows();
}
// Show only the rows the current mode actually has settings for.
function renderPadRows(){
  PADS.forEach(function(padId){
    var dirRow=document.getElementById('dir-row-'+padId);
    if(dirRow) dirRow.style.display=(modes[padId]==='scroll')?'':'none';
    // A pad feeding the DS4 touchpad has no rebindable click: the press is
    // the touchpad press, and offering to remap it would promise behaviour
    // the pad cannot deliver.
    var clickRow=document.getElementById('row-'+padId);
    if(clickRow) clickRow.style.display=(modes[padId]==='ds4')?'none':'';
  });
}
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

document.getElementById('platform').addEventListener('change',function(e){
  setPlatform(e.target.value);
});
PADS.forEach(function(padId){
  var modeSel=document.getElementById('mode-'+padId);
  if(modeSel) modeSel.addEventListener('change',function(e){setMode(padId,e.target.value);});
  var dirSel=document.getElementById('dir-'+padId);
  if(dirSel) dirSel.addEventListener('change',function(e){setDir(padId,e.target.value);});
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
                       const ControllerProfile& cfg,
                       std::vector<InstalledGame> games,
                       std::map<std::wstring, ControllerProfile> gameProfiles,
                       std::function<void(const std::wstring&, const ControllerProfile&)> applyCallback)
{
    // If already open, just bring it to front.
    if (m_hwnd) {
        if (IsWindowVisible(m_hwnd)) { BringToFront(); return; }
        // Was hidden. Refresh config and show.
        m_config        = cfg;
        m_games         = std::move(games);
        m_gameProfiles  = std::move(gameProfiles);
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
    m_games        = std::move(games);
    m_gameProfiles = std::move(gameProfiles);
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
        cfg.platform = JsonStr(msg, "platform") == "ps" ? ControllerPlatform::PlayStation
                                                        : ControllerPlatform::Xbox;
        cfg.back.l4 = BackButtonBinding::FromId(JsonStr(msg, "L4"));
        cfg.back.l5 = BackButtonBinding::FromId(JsonStr(msg, "L5"));
        cfg.back.r4 = BackButtonBinding::FromId(JsonStr(msg, "R4"));
        cfg.back.r5 = BackButtonBinding::FromId(JsonStr(msg, "R5"));
        cfg.leftPad.click  = BackButtonBinding::FromId(JsonStr(msg, "LPAD"));
        cfg.rightPad.click = BackButtonBinding::FromId(JsonStr(msg, "RPAD"));
        cfg.leftPad.mode       = TrackpadModeFromId(JsonStr(msg, "LPADmode"));
        cfg.rightPad.mode      = TrackpadModeFromId(JsonStr(msg, "RPADmode"));
        cfg.leftPad.scrollDir  = ScrollDirectionFromId(JsonStr(msg, "LPADdir"));
        cfg.rightPad.scrollDir = ScrollDirectionFromId(JsonStr(msg, "RPADdir"));

        // "game" is an index into m_games (ASCII decimal), never the raw
        // game id — that can contain non-ASCII characters this hand-rolled
        // channel would mangle, and backslashes this parser does not unescape.
        const std::string gameIdx = JsonStr(msg, "game");
        if (gameIdx.empty()) {
            m_config = cfg;
            if (m_applyCallback) m_applyCallback(L"", cfg);
        } else {
            size_t idx = 0;
            bool valid = true;
            for (char c : gameIdx) {
                if (c < '0' || c > '9') { valid = false; break; }
                idx = idx * 10 + static_cast<size_t>(c - '0');
            }
            if (valid && idx < m_games.size()) {
                const std::wstring& gameId = m_games[idx].id;
                // Captured here because this is the only place both halves
                // are in hand: the picker knows the friendly name, and
                // nothing downstream re-enumerates the installed list.
                cfg.displayName = m_games[idx].name;
                m_gameProfiles[gameId] = cfg;
                if (m_applyCallback) m_applyCallback(gameId, cfg);
            }
        }
    }
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

void RemapWindow::SendInitState() {
    if (!m_webview) return;

    // Convert ASCII action ID string to wstring without char→wchar_t narrowing warnings.
    auto wid = [](const BackButtonBinding& b) -> std::wstring {
        const std::string s = b.Id();
        return std::wstring(s.begin(), s.end());
    };
    auto narrow = [](const char* s) {
        const std::string v = s;
        return std::wstring(v.begin(), v.end());
    };
    // One flat object per profile — the page reads it back the same way, and
    // the narrow JSON reader on this side cannot descend into nesting.
    auto profileJson = [&](const ControllerProfile& p) {
        return L"{\"platform\":\""
               + std::wstring(p.platform == ControllerPlatform::PlayStation ? L"ps" : L"xbox")
               + L"\","
               L"\"L4\":\"" + wid(p.back.l4) + L"\","
               L"\"L5\":\"" + wid(p.back.l5) + L"\","
               L"\"R4\":\"" + wid(p.back.r4) + L"\","
               L"\"R5\":\"" + wid(p.back.r5) + L"\","
               L"\"LPAD\":\"" + wid(p.leftPad.click) + L"\","
               L"\"RPAD\":\"" + wid(p.rightPad.click) + L"\","
               L"\"LPADmode\":\"" + narrow(TrackpadModeId(p.leftPad.mode)) + L"\","
               L"\"RPADmode\":\"" + narrow(TrackpadModeId(p.rightPad.mode)) + L"\","
               L"\"LPADdir\":\"" + narrow(ScrollDirectionId(p.leftPad.scrollDir)) + L"\","
               L"\"RPADdir\":\"" + narrow(ScrollDirectionId(p.rightPad.scrollDir)) + L"\"}";
    };

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
        for (const auto* b : { &p.back.l4, &p.back.l5, &p.back.r4, &p.back.r5,
                               &p.leftPad.click, &p.rightPad.click })
            addLabel(*b);
    };
    addProfileLabels(m_config);
    for (const auto& [id, cfg] : m_gameProfiles)
        addProfileLabels(cfg);

    // Games list: index-based picker ids so the raw game id — possibly
    // non-ASCII, sometimes backslash-laden — never has to cross the
    // hand-rolled JSON channel; see OnWebMessage's "apply" handling for the
    // other direction.
    std::wstring gamesJson;
    for (size_t i = 0; i < m_games.size(); ++i) {
        if (!gamesJson.empty()) gamesJson += L",";
        gamesJson += L"{\"id\":\"" + std::to_wstring(i) + L"\",\"name\":\""
                   + JsonEscape(m_games[i].name) + L"\"}";
    }

    // Profiles: the default plus every game that already has a saved
    // override. A listed game with no entry here simply has none yet — the
    // page falls back to the default profile's bindings when first selected.
    std::wstring profilesJson = L"\"\":" + profileJson(m_config);
    for (size_t i = 0; i < m_games.size(); ++i) {
        auto it = m_gameProfiles.find(m_games[i].id);
        if (it == m_gameProfiles.end()) continue;
        profilesJson += L",\"" + std::to_wstring(i) + L"\":" + profileJson(it->second);
    }

    std::wstring json =
        L"{\"type\":\"init\""
        L",\"labels\":{" + labels + L"}"
        L",\"games\":[" + gamesJson + L"]"
        L",\"profiles\":{" + profilesJson + L"}}";
    PostToWebView(json);
}
