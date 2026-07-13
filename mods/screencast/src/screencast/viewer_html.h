/* The self-contained browser viewer served at GET /. An <img> streams the MJPEG
 * endpoint; pointer events on it map to device pixels (272x480) and POST /tap as
 * down/move/up so a browser drag drives a real touch drag. Single-quoted
 * attributes and single-quoted JS strings keep this a clean double-quoted C
 * literal with no escaped double-quotes. */
#ifndef SCREENCAST_VIEWER_HTML_H
#define SCREENCAST_VIEWER_HTML_H

static const char SC_VIEWER_HTML[] =
"<!doctype html><html><head>"
"<meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1,user-scalable=no'>"
"<title>Zune Screen</title>"
"<style>"
"html,body{margin:0;height:100%;background:#111}"
"body{display:flex;align-items:center;justify-content:center}"
"#s{max-width:100vw;max-height:100vh;touch-action:none;image-rendering:pixelated;-webkit-user-select:none;user-select:none}"
"</style></head><body>"
"<img id='s' src='/stream' draggable='false'>"
"<script>"
"var s=document.getElementById('s'),down=false,last=0;"
"function P(e){var r=s.getBoundingClientRect();"
"var x=Math.round((e.clientX-r.left)/r.width*272),y=Math.round((e.clientY-r.top)/r.height*480);"
"x=x<0?0:x>271?271:x;y=y<0?0:y>479?479:y;return x+' '+y;}"
"function T(a,e){var q=new XMLHttpRequest();q.open('POST','/tap');"
"q.setRequestHeader('Content-Type','text/plain');q.send('a='+a+'&p='+P(e));}"
"s.addEventListener('pointerdown',function(e){e.preventDefault();down=true;s.setPointerCapture(e.pointerId);T(1,e);});"
"s.addEventListener('pointermove',function(e){if(!down)return;var t=Date.now();if(t-last<60)return;last=t;T(2,e);});"
"s.addEventListener('pointerup',function(e){if(!down)return;down=false;T(3,e);});"
"s.addEventListener('pointercancel',function(e){down=false;});"
"</script></body></html>";

#endif /* SCREENCAST_VIEWER_HTML_H */
