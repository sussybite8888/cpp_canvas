#pragma once
// The browser backend (emscripten): every Canvas is a CanvasRenderingContext2D
// on the page, fed through a batched op stream. Included only by canvas.cpp.
//
// How a call reaches the page
// ---------------------------
// Calls do not cross into JavaScript one at a time: each is a fixed-size
// record in one buffer in wasm memory that all canvases share, holding one
// canvas's calls at a time. It is handed over whole (c2d_flush) when another
// canvas starts recording, at Window::present(), when it fills, and before
// anything reads the pixels of the canvas holding it. Style strings are
// interned once and named by index; text, path geometry and pixels ride in
// the buffer's arenas. The drawing state --
// save/restore included -- is mirrored in wasm (web_), and what the page holds
// is tracked in applied_, so a setter costs nothing until a draw reads it.
//
// Culling
// -------
// Every record that paints carries its device bounding box (already clipped)
// and, when it is opaque, the device rectangle it is known to cover
// completely. Before a buffer is handed over it is walked backwards with a
// one-bit-per-pixel occlusion map: a paint whose whole box a later opaque
// paint (or clear) covers is replaced by a no-op, along with the path it alone
// would have built. The page draws exactly what it would have drawn anyway.

#include <emscripten.h>

#include "effects.h"

#include <deque>
#include <unordered_map>
#include <unordered_set>

// ===========================================================================
// The page's side.
// ===========================================================================
EM_JS(int, c2d_create, (const char* element, int width, int height, int virtualCanvas, int desynchronized), {
  Module.cppCanvasContexts ||= [];
  Module.cppCanvasFreeIds ||= [];
  Module.cppCanvasSpare ||= [];
  Module.cppCanvasLoss ||= {seen: false, note: () => { Module.cppCanvasLoss.seen = true; }, waited: 0, checkedAt: 0, probe: null, probedAt: 0, probeBroken: false};
  let surface, options, ctx = null;
  if (virtualCanvas) {
    // A surface a destroyed canvas left behind: resizing it is several times
    // cheaper than a new surface and context, and leaves nothing for the
    // garbage collector (apps that make and drop canvases every frame).
    const spare = Module.cppCanvasSpare.pop();
    if (spare) {
      Module.cppCanvasSparePixels -= spare.surface.width * spare.surface.height;
      surface = spare.surface; ctx = spare.ctx;
    } else surface = typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(width, height) : document.createElement('canvas');
  }
  else { const id = UTF8ToString(element); surface = document.getElementById(id) || Object.assign(document.createElement('canvas'), {id}); if (!surface.parentNode) document.body.appendChild(surface); options = {alpha: false, desynchronized: !!desynchronized}; }
  // Setting the size clears the bitmap and resets the context's state, even
  // when it does not change.
  surface.width = width; surface.height = height;
  if (ctx) { if (ctx.reset) ctx.reset(); }
  else { surface.addEventListener('contextlost', Module.cppCanvasLoss.note); ctx = surface.getContext('2d', options); }
  const entry = {surface, ctx, main: !virtualCanvas, layers: []};
  if (entry.main) Module.cppCanvasMain = entry;
  // Ids are reused, so the table does not grow with every canvas ever made.
  // Nothing names a destroyed id: its pending draws were flushed before it went.
  const C = Module.cppCanvasContexts, free = Module.cppCanvasFreeIds;
  const id = free.length ? free.pop() : C.length;
  C[id] = entry;
  return id;
});
EM_JS(void, c2d_destroy, (int id), {
  const C = Module.cppCanvasContexts, entry = C && C[id];
  if (!entry) return;
  C[id] = null;
  Module.cppCanvasFreeIds.push(id);
  if (entry.main) return;
  // Kept for the next virtual canvas, up to 64 surfaces and 4M pixels in all;
  // never a lost one.
  const S = Module.cppCanvasSpare, px = entry.surface.width * entry.surface.height;
  Module.cppCanvasSparePixels ||= 0;
  const lost = entry.ctx && entry.ctx.isContextLost && entry.ctx.isContextLost();
  if (!lost && !Module.cppCanvasLoss.seen && entry.ctx && S.length < 64 && Module.cppCanvasSparePixels + px <= (4 << 20)) {
    S.push({surface: entry.surface, ctx: entry.ctx});
    Module.cppCanvasSparePixels += px;
  } else entry.surface.removeEventListener('contextlost', Module.cppCanvasLoss.note);
});
// GPU loss: 0 fine, 1 lost, 2 lost for too long, 3 ready to rebuild. See the
// README for the three ways a loss is noticed.
EM_JS(int, c2d_loss_check, (int useProbe), {
  const L = Module.cppCanvasLoss;
  if (!L) return 0;
  const now = performance.now(), main = Module.cppCanvasMain, x = main && main.ctx;
  const step = Math.min(now - L.checkedAt, 100);
  L.checkedAt = now;
  const mainLost = !!(x && x.isContextLost && x.isContextLost());
  if (mainLost) L.seen = true;
  if (!L.seen && useProbe && !L.probeBroken && now - L.probedAt >= 2000) {
    L.probedAt = now;
    try {
      if (!L.probe) {
        const side = 300;
        const surface = typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(side, side) : Object.assign(document.createElement('canvas'), {width: side, height: side});
        const p = surface.getContext('2d');
        p.fillStyle = '#ff00ff';
        p.fillRect(0, 0, side, side);
        const d = p.getImageData(0, 0, 1, 1).data;
        if (d[0] > 200 && d[1] < 55 && d[2] > 200 && d[3] > 200) L.probe = p;
        else L.probeBroken = true;
      } else if (L.probe.getImageData(0, 0, 1, 1).data[3] < 128) {
        L.seen = true;
      }
    } catch (e) {
      L.probeBroken = true;
    }
  }
  if (!L.seen) { L.waited = 0; return 0; }
  if (!mainLost) return 3;
  L.waited += step;
  return L.waited >= 4000 ? 2 : 1;
});
EM_JS(int, c2d_recover, (), {
  const L = Module.cppCanvasLoss;
  L.seen = false; L.waited = 0; L.probe = null; L.probedAt = 0;
  if (Module.cppCanvasImages) Module.cppCanvasImages.clear();
  if (Module.cppCanvasSpare) { for (const e of Module.cppCanvasSpare) e.surface.removeEventListener('contextlost', L.note); Module.cppCanvasSpare = []; Module.cppCanvasSparePixels = 0; }
  let replaced = 0;
  for (const entry of Module.cppCanvasContexts) {
    if (!entry) continue;
    const old = entry.surface, width = old.width, height = old.height;
    entry.layers = [];
    if (entry.main) { old.width = width; continue; }
    old.removeEventListener('contextlost', L.note);
    const surface = typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(width, height) : Object.assign(document.createElement('canvas'), {width, height});
    surface.addEventListener('contextlost', L.note);
    entry.surface = surface;
    entry.ctx = surface.getContext('2d');
    ++replaced;
  }
  return replaced;
});
EM_JS(void, c2d_present, (int id, const char* target), {
  const item = Module.cppCanvasContexts[id], name = UTF8ToString(target);
  let canvas = document.getElementById(name);
  if (!canvas) { canvas = document.createElement('canvas'); canvas.id = name; document.body.appendChild(canvas); }
  canvas.width = item.surface.width; canvas.height = item.surface.height; canvas.getContext('2d').drawImage(item.surface, 0, 0);
});
EM_JS(void, c2d_intern, (int id, const char* text), {
  (Module.cppCanvasStrings || (Module.cppCanvasStrings = [""]))[id] = UTF8ToString(text);
});
EM_JS(void, c2d_intern_reset, (), { Module.cppCanvasStrings = [""]; });
// A record is twelve 32-bit slots: the opcode, ten float arguments, and one
// integer reference (a string index, an arena offset, a key). A call that
// throws is skipped on its own; the calls after it still run.
EM_JS(void, c2d_flush, (int id, const int* ops, int count, const char* text, const float* paths, const unsigned char* bytes), {
  const entry = Module.cppCanvasContexts[id];
  if (!entry || !entry.ctx) return;
  let x = entry.layers.length ? entry.layers[entry.layers.length - 1].ctx : entry.ctx;
  const S = Module.cppCanvasStrings || [""], O = Module.cppCanvasObjects || (Module.cppCanvasObjects = new Map());
  const F = HEAPF32, I = HEAP32;
  // A style reference beginning with \x01 names a gradient or pattern.
  const style = (s) => s.charCodeAt(0) === 1 ? (O.get(s.slice(1)) || 'transparent') : s;
  for (let n = 0, p = ops >> 2; n < count; n++, p += 12) {
    const s = S[I[p+11]];
    try {
    switch(I[p]) {
      case 0:x.save();break; case 1:x.restore();break; case 2: x.reset ? x.reset() : (x.setTransform(1,0,0,1,0,0),x.clearRect(0,0,x.canvas.width,x.canvas.height));break;
      case 3:x.scale(F[p+1],F[p+2]);break; case 4:x.rotate(F[p+1]);break; case 5:x.translate(F[p+1],F[p+2]);break; case 6:x.transform(F[p+1],F[p+2],F[p+3],F[p+4],F[p+5],F[p+6]);break; case 7:x.setTransform(F[p+1],F[p+2],F[p+3],F[p+4],F[p+5],F[p+6]);break; case 8:x.resetTransform();break;
      case 9:x.fillStyle=style(s);break; case 10:x.strokeStyle=style(s);break; case 11:x.globalAlpha=F[p+1];break; case 12:x.globalCompositeOperation=s;break; case 13:x.filter=s;break;
      case 14:x.lineWidth=F[p+1];break; case 15:x.lineCap=s;break; case 16:x.lineJoin=s;break; case 17:x.miterLimit=F[p+1];break; case 18:x.lineDashOffset=F[p+1];break;
      case 19:x.shadowColor=s;x.shadowBlur=F[p+1];x.shadowOffsetX=F[p+2];x.shadowOffsetY=F[p+3];break; case 20:x.font=s;break; case 21:x.textAlign=s;break; case 22:x.textBaseline=s;break; case 23:x.direction=s;break; case 24:x.imageSmoothingEnabled=!!F[p+1];break; case 25:x.imageSmoothingQuality=s;break;
      case 26:{const q=(paths>>2)+I[p+11];x.setLineDash(Array.from(F.subarray(q,q+F[p+1])));break;}
      case 27:if('letterSpacing' in x)x.letterSpacing=s;break; case 28:if('wordSpacing' in x)x.wordSpacing=s;break; case 29:if('fontKerning' in x)x.fontKerning=s;break;
      case 30:x.clearRect(F[p+1],F[p+2],F[p+3],F[p+4]);break; case 31:x.fillRect(F[p+1],F[p+2],F[p+3],F[p+4]);break; case 32:x.strokeRect(F[p+1],F[p+2],F[p+3],F[p+4]);break;
      case 33:x.beginPath();break; case 34:x.closePath();break; case 35:x.moveTo(F[p+1],F[p+2]);break; case 36:x.lineTo(F[p+1],F[p+2]);break; case 37:x.quadraticCurveTo(F[p+1],F[p+2],F[p+3],F[p+4]);break; case 38:x.bezierCurveTo(F[p+1],F[p+2],F[p+3],F[p+4],F[p+5],F[p+6]);break; case 39:x.arc(F[p+1],F[p+2],F[p+3],F[p+4],F[p+5],!!F[p+6]);break; case 40:x.arcTo(F[p+1],F[p+2],F[p+3],F[p+4],F[p+5]);break; case 41:x.ellipse(F[p+1],F[p+2],F[p+3],F[p+4],F[p+5],F[p+6],F[p+7],!!F[p+8]);break; case 42:x.rect(F[p+1],F[p+2],F[p+3],F[p+4]);break; case 43:x.roundRect(F[p+1],F[p+2],F[p+3],F[p+4],F[p+5]);break;
      case 44:x.fill(s || 'nonzero');break; case 45:x.stroke();break; case 46:x.clip(s || 'nonzero');break;
      case 47:{const t=UTF8ToString(text+I[p+11]);F[p+1] < 0 ? x.fillText(t,F[p+2],F[p+3]) : x.fillText(t,F[p+2],F[p+3],F[p+1]);break;}
      case 48:{const t=UTF8ToString(text+I[p+11]);F[p+1] < 0 ? x.strokeText(t,F[p+2],F[p+3]) : x.strokeText(t,F[p+2],F[p+3],F[p+1]);break;}
      case 49:{const q=(paths>>2)+I[p+11],r=[];for(let i=0;i<F[p+5];i++)r.push({x:F[q+2*i],y:F[q+2*i+1]});x.roundRect(F[p+1],F[p+2],F[p+3],F[p+4],r);break;}
      case 50:{const P=Module.cppCanvasPaths.get(I[p+11]);if(P){const r=F[p+2]?'evenodd':'nonzero';F[p+1]===0?x.fill(P,r):F[p+1]===1?x.stroke(P):x.clip(P,r);}break;}
      case 51:{const im=Module.cppCanvasContexts[I[p+11]].surface;F[p+5]?x.drawImage(im,F[p+1],F[p+2],F[p+3],F[p+4]):x.drawImage(im,F[p+1],F[p+2]);break;}
      case 52:x.drawImage(Module.cppCanvasContexts[I[p+11]].surface,F[p+1],F[p+2],F[p+3],F[p+4],F[p+5],F[p+6],F[p+7],F[p+8]);break;
      // Geometry the page has not seen, from the path arena; a nonzero key
      // keeps it. Action 3 only keeps it (its paint was culled).
      case 53:{
        const n=F[p+4],q=(paths>>2)+F[p+3],P=new Path2D();
        for(let i=0;i<n;i++){const o=q+i*10,cc=!!F[o+1],a=o+2;
          switch(F[o]){case 0:P.moveTo(F[a],F[a+1]);break;case 1:P.lineTo(F[a],F[a+1]);break;case 2:P.quadraticCurveTo(F[a],F[a+1],F[a+2],F[a+3]);break;case 3:P.bezierCurveTo(F[a],F[a+1],F[a+2],F[a+3],F[a+4],F[a+5]);break;case 4:P.arc(F[a],F[a+1],F[a+2],F[a+3],F[a+4],cc);break;case 5:P.arcTo(F[a],F[a+1],F[a+2],F[a+3],F[a+4]);break;case 6:P.ellipse(F[a],F[a+1],F[a+2],F[a+3],F[a+4],F[a+5],F[a+6],cc);break;case 7:P.rect(F[a],F[a+1],F[a+2],F[a+3]);break;case 8:P.roundRect(F[a],F[a+1],F[a+2],F[a+3],F[a+4]);break;case 9:P.closePath();break;}}
        if(I[p+11]) (Module.cppCanvasPaths||(Module.cppCanvasPaths=new Map())).set(I[p+11],P);
        const r=F[p+2]?'evenodd':'nonzero';
        if(F[p+1]===0)x.fill(P,r);else if(F[p+1]===1)x.stroke(P);else if(F[p+1]===2)x.clip(P,r);
        break;}
      case 54:{const R=Module.cppRemoteImages,e=R&&R.get(I[p+11]);if(e){e.drawnAt=performance.now();if(e.parked)e.resume();const im=e.frame||(e.state===1?e.img:null);if(im)x.drawImage(im,F[p+1],F[p+2],F[p+3],F[p+4]);}break;}
      // An uploaded image (c2d_image_upload): source rect, destination rect,
      // extra alpha. A negative key is a one-off upload, dropped once drawn.
      case 55:{const k=I[p+11],im=Module.cppCanvasImages&&Module.cppCanvasImages.get(k);if(im){const was=x.globalAlpha;x.globalAlpha=was*F[p+9];x.drawImage(im,F[p+1],F[p+2],F[p+3],F[p+4],F[p+5],F[p+6],F[p+7],F[p+8]);x.globalAlpha=was;}if(k<0&&Module.cppCanvasImages)Module.cppCanvasImages.delete(k);break;}
      case 56:{const w=F[p+1],h=F[p+2],o=bytes+I[p+11];x.putImageData(new ImageData(new Uint8ClampedArray(HEAPU8.slice(o,o+w*h*4)),w,h),F[p+3],F[p+4]);break;}
      case 57:if(Module.cppCanvasImages)Module.cppCanvasImages.delete(I[p+11]);break;
      // clear(colour): every pixel replaced, the state left as it was.
      case 58:x.save();x.setTransform(1,0,0,1,0,0);x.globalAlpha=1;x.globalCompositeOperation='copy';x.filter='none';x.shadowColor='transparent';x.fillStyle=s;x.fillRect(0,0,x.canvas.width,x.canvas.height);x.restore();break;
      // Layers: the browser's own when it has them, an offscreen surface
      // carrying the state across when it does not.
      case 59:
        if (x.beginLayer) { x.beginLayer(); entry.layers.push({ctx: x, native: true}); break; }
        { const c = typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(x.canvas.width, x.canvas.height) : Object.assign(document.createElement('canvas'), {width: x.canvas.width, height: x.canvas.height});
          const l = c.getContext('2d');
          l.setTransform(x.getTransform());
          for (const k of ['fillStyle','strokeStyle','lineWidth','lineCap','lineJoin','miterLimit','lineDashOffset','font','textAlign','textBaseline','direction','letterSpacing','wordSpacing','fontKerning','imageSmoothingEnabled','imageSmoothingQuality']) if (k in x) l[k] = x[k];
          l.setLineDash(x.getLineDash());
          entry.layers.push({ctx: l, parent: x, native: false});
          x = l; }
        break;
      case 60: {
        const top = entry.layers.pop();
        if (!top) break;
        if (top.native) { x.endLayer(); break; }
        x = top.parent;
        x.save(); x.setTransform(1,0,0,1,0,0); x.drawImage(top.ctx.canvas, 0, 0); x.restore();
        break; }
      case 61: break;
    }
    } catch (e) {
      if (!Module.cppCanvasThrew) { Module.cppCanvasThrew = true; console.warn('[canvas] a drawing call threw and was skipped (reported once):', e); }
    }
  }
});
EM_JS(void, c2d_path_drop, (int key), { if (Module.cppCanvasPaths) Module.cppCanvasPaths.delete(key); });
EM_JS(int, c2d_blink, (), {
  if (typeof navigator === 'undefined') return 0;
  const d = navigator.userAgentData;
  if (d && d.brands) return d.brands.some(b => b.brand.includes('Chromium') || b.brand.includes('Google Chrome')) ? 1 : 0;
  const ua = navigator.userAgent;
  return ua.includes('Chrome/') || ua.includes('Chromium/') ? 1 : 0;
});
EM_JS(void, c2d_warn, (const char* text), { console.warn(UTF8ToString(text)); });
// Text is measured on a context of its own, so measuring never has to flush
// the canvas's op stream to bring its font across.
EM_JS(void, c2d_measure, (const char* font, const char* text, const char* letter, const char* word, const char* kerning, const char* direction, float* out), {
  const M = Module.cppMeasure || (Module.cppMeasure = (typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(1, 1) : document.createElement('canvas')).getContext('2d'));
  M.font = UTF8ToString(font);
  if ('letterSpacing' in M) M.letterSpacing = UTF8ToString(letter);
  if ('wordSpacing' in M) M.wordSpacing = UTF8ToString(word);
  if ('fontKerning' in M) M.fontKerning = UTF8ToString(kerning);
  M.direction = UTF8ToString(direction);
  M.textAlign = 'left'; M.textBaseline = 'alphabetic';
  const m = M.measureText(UTF8ToString(text)), o = out >> 2;
  const v = [m.width, m.actualBoundingBoxLeft, m.actualBoundingBoxRight, m.actualBoundingBoxAscent, m.actualBoundingBoxDescent,
             m.fontBoundingBoxAscent, m.fontBoundingBoxDescent, m.emHeightAscent, m.emHeightDescent, m.hangingBaseline, m.alphabeticBaseline, m.ideographicBaseline];
  for (let i = 0; i < v.length; i++) HEAPF32[o + i] = +v[i] || 0;
});
EM_JS(int, c2d_get_pixels, (int id,int x,int y,int w,int h,std::uint8_t* out), { const e=Module.cppCanvasContexts[id]; const c=e&&e.ctx; if (!c) return 0; const d=c.getImageData(x,y,w,h).data; HEAPU8.set(d,out); return d.length; });
// Uploads pixels as a drawable surface under `key` (no context is touched, so
// nothing has to flush). The C++ side decides what stays and what goes.
EM_JS(void, c2d_image_upload, (int key, const std::uint8_t* data, int iw, int ih), {
  const cache = Module.cppCanvasImages || (Module.cppCanvasImages = new Map());
  const surface = typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(iw, ih) : Object.assign(document.createElement('canvas'), {width: iw, height: ih});
  const c = surface.getContext('2d');
  if (!c) return;
  c.putImageData(new ImageData(new Uint8ClampedArray(HEAPU8.slice(data, data + iw * ih * 4)), iw, ih), 0, 0);
  cache.set(key, surface);
});
EM_JS(void, c2d_image_drop, (int key), { if (Module.cppCanvasImages) Module.cppCanvasImages.delete(key); });
// Gradients and patterns are page objects named by id. They are created
// immutable here: a stop added after use makes a new one, so draws recorded
// before it keep the gradient they were recorded with.
EM_JS(void, c2d_gradient, (int id, int kind, float a, float b, float c, float d, float e, float f, const float* stops, const char* colours, int count), {
  const O = Module.cppCanvasObjects || (Module.cppCanvasObjects = new Map());
  const H = Module.cppObjectContext || (Module.cppObjectContext = (typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(1, 1) : document.createElement('canvas')).getContext('2d'));
  let g;
  try {
    g = kind === 0 ? H.createLinearGradient(a, b, c, d) : kind === 1 ? H.createRadialGradient(a, b, c, d, e, f) : H.createConicGradient(a, b, c);
    const names = UTF8ToString(colours).split('\n');
    for (let i = 0; i < count; i++) g.addColorStop(HEAPF32[(stops >> 2) + i], names[i]);
  } catch (err) { g = 'transparent'; }
  O.set('g' + id, g);
});
EM_JS(void, c2d_pattern, (int id, int sourceId, int snapshotId, const std::uint8_t* data, int w, int h, const char* repetition, float a, float b, float c, float d, float e, float f), {
  const O = Module.cppCanvasObjects || (Module.cppCanvasObjects = new Map());
  const H = Module.cppObjectContext || (Module.cppObjectContext = (typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(1, 1) : document.createElement('canvas')).getContext('2d'));
  const snaps = Module.cppPatternSources || (Module.cppPatternSources = new Map());
  let src = snaps.get(snapshotId);
  if (!src) {
    src = typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(w, h) : Object.assign(document.createElement('canvas'), {width: w, height: h});
    const c2 = src.getContext('2d');
    if (sourceId >= 0) c2.drawImage(Module.cppCanvasContexts[sourceId].surface, 0, 0);
    else c2.putImageData(new ImageData(new Uint8ClampedArray(HEAPU8.slice(data, data + w * h * 4)), w, h), 0, 0);
    snaps.set(snapshotId, src);
  }
  let p = null;
  try { p = H.createPattern(src, UTF8ToString(repetition)); if (p) p.setTransform(new DOMMatrix([a, b, c, d, e, f])); } catch (err) {}
  O.set('p' + id, p || 'transparent');
});
EM_JS(void, c2d_object_drop, (const char* name), { if (Module.cppCanvasObjects) Module.cppCanvasObjects.delete(UTF8ToString(name)); });
EM_JS(void, c2d_pattern_source_drop, (int id), { if (Module.cppPatternSources) Module.cppPatternSources.delete(id); });
EM_JS(int, c2d_register_font, (const char* family, const std::uint8_t* data, int size, int weight, int italic), {
  if (typeof FontFace === 'undefined' || typeof document === 'undefined') return 0;
  try {
    const face = new FontFace(UTF8ToString(family), HEAPU8.slice(data, data + size), {weight: String(weight), style: italic ? 'italic' : 'normal'});
    face.load().then(() => {}, () => {});
    (document.fonts || {add() {}}).add(face);
    return 1;
  } catch (e) { return 0; }
});
// The safety net: drawing still buffered when control returns to the browser
// is handed over before the next paint.
EM_JS(void, c2d_schedule_flush, (), {
  if (Module.cppFlushScheduled) return;
  Module.cppFlushScheduled = true;
  queueMicrotask(() => { Module.cppFlushScheduled = false; if (Module._cpp_canvas_flush_all) Module._cpp_canvas_flush_all(); });
});
EM_JS(int, c2d_remote_request, (const char* url), {
  const R = Module.cppRemoteImages || (Module.cppRemoteImages = new Map());
  Module.cppRemoteImageNext = (Module.cppRemoteImageNext || 0) + 1;
  const id = Module.cppRemoteImageNext, img = new Image();
  const e = {img, state: 0, frame: null, decoder: null, timer: 0, drawnAt: 0, parked: false, dead: false, resume: () => {}};
  img.decoding = 'async';
  img.referrerPolicy = 'no-referrer';
  img.onload = () => { e.state = img.naturalWidth > 0 && img.naturalHeight > 0 ? 1 : -1; };
  img.onerror = () => { e.state = -1; };
  img.src = UTF8ToString(url);
  R.set(id, e);
  // Animated pictures are decoded a frame at a time while they are drawn.
  Module.cppRemoteAnimate ||= async (e, src) => {
    let decoder = null;
    try {
      const response = await fetch(src, {mode: 'cors', credentials: 'omit', referrerPolicy: 'no-referrer'});
      if (!response.ok || e.dead) return;
      const type = String(response.headers.get('content-type') || 'x').split(';')[0].trim().toLowerCase();
      if (!type.startsWith('image/') || !(await ImageDecoder.isTypeSupported(type))) return;
      decoder = new ImageDecoder({data: response.body, type});
      await decoder.completed;
      const track = decoder.tracks.selectedTrack;
      if (e.dead || !track || !track.animated || track.frameCount < 2) { decoder.close(); return; }
      e.decoder = decoder;
      let index = 0;
      const step = async () => {
        e.timer = 0;
        if (e.dead) return;
        if (performance.now() - e.drawnAt > 1000) { e.parked = true; return; }
        const { image } = await decoder.decode({frameIndex: index});
        if (e.dead) { image.close(); return; }
        if (e.frame) e.frame.close();
        e.frame = image;
        index = (index + 1) % track.frameCount;
        const ms = (image.duration || 0) / 1000;
        e.timer = setTimeout(() => step().catch(() => {}), ms > 10 ? ms : 100);
      };
      e.resume = () => { e.parked = false; step().catch(() => {}); };
      e.drawnAt = performance.now();
      await step();
    } catch (err) {
      if (decoder && !e.decoder) { try { decoder.close(); } catch (_) {} }
    }
  };
  if (typeof ImageDecoder !== 'undefined') Module.cppRemoteAnimate(e, img.src);
  return id;
});
EM_JS(int, c2d_remote_status, (int id, int* size), {
  const e = Module.cppRemoteImages && Module.cppRemoteImages.get(id);
  if (!e) return -1;
  if (e.state === 1) { HEAP32[size>>2] = e.img.naturalWidth; HEAP32[(size>>2)+1] = e.img.naturalHeight; }
  return e.state;
});
EM_JS(void, c2d_remote_release, (int id), {
  const e = Module.cppRemoteImages && Module.cppRemoteImages.get(id);
  if (!e) return;
  e.img.onload = e.img.onerror = null;
  e.img.removeAttribute('src');
  e.dead = true;
  if (e.timer) clearTimeout(e.timer);
  if (e.frame) e.frame.close();
  if (e.decoder) { try { e.decoder.close(); } catch (_) {} }
  Module.cppRemoteImages.delete(id);
});

// ===========================================================================
// The op buffers.
// ===========================================================================
namespace {
union OpSlot { float f; std::int32_t i; };
constexpr int kOpSlots = 12;
// A bound on how far a call can lag the page, not on how big a frame may be:
// filling the buffer flushes it and carries on.
constexpr int kOpCapacity = 16384;
constexpr std::size_t kMaxArenaFloats = 1u << 20;
constexpr int kNop = 61;

// What culling needs to know about one record.
struct OpMeta {
    // 0 state/other (kept), 1 paint (cullable), 2 page-path building.
    std::uint8_t kind = 0;
    // A paint whose page path is `pathGen` (fill/stroke of the current path).
    bool usesPagePath = false;
    // Record 53 that also keeps its geometry under a key.
    bool keepsPath = false;
    // Record 55 of a one-off upload.
    bool tempImage = false;
    std::int32_t pathGen = 0;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // paint box, device, clipped
    std::int32_t ox0 = 0, oy0 = 0, ox1 = 0, oy1 = 0;  // occluded box (empty unless opaque)
};

std::unordered_map<std::string, std::int32_t>& interned() {
    static auto* m = new std::unordered_map<std::string, std::int32_t>{{std::string(), 0}};
    return *m;
}
constexpr std::size_t kMaxInternedStrings = 4096;

CanvasFrameStats gFrameStats;
int gOpTypeCounts[kCanvasOpCodes] = {};

std::unordered_map<int, Canvas*>& liveCanvases() {
    static auto* live = new std::unordered_map<int, Canvas*>();
    return *live;
}
}  // namespace

// What a canvas keeps of its own between batches: the generation of the
// page's current path, which culling matches paints against.
struct WebGpu;  // detail/webgpu.h
struct Canvas::WebOps {
    std::int32_t pathGen = 1;
    // Set for an element canvas drawn with WebGL2 (detail/webgpu.h).
    std::shared_ptr<WebGpu> gpu;
};

namespace {
// The calls not yet handed to the page. There is ONE buffer, as in florr's
// backend, and it holds one canvas's calls at a time: a canvas that records
// while another holds it flushes that one first (claimOps), which is what
// keeps every canvas's calls reaching the page in the order they were made.
// Per-canvas buffers cost each canvas the whole capacity -- an app that bakes
// hundreds of small canvases, more with every new scroll position, ran the
// page out of memory.
struct OpBuffer {
    std::vector<OpSlot> slots;
    std::vector<OpMeta> meta;
    int count = 0;
    // Arenas for the calls in the buffer: text, path geometry and pixels.
    // Emptied by each flush; their capacity is kept.
    std::string text;
    std::vector<float> paths;
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint64_t> occlusion;  // culling scratch
    // The canvas the calls belong to, and its own state; null when no canvas
    // has recorded since the last owner went away.
    const Canvas* owner = nullptr;
    Canvas::WebOps* state = nullptr;
    OpBuffer() { slots.resize(static_cast<std::size_t>(kOpCapacity) * kOpSlots); meta.resize(kOpCapacity); }
};
// Never destroyed: a Canvas outliving it at exit still has something to leave.
OpBuffer& opBuffer() { static auto* b = new OpBuffer(); return *b; }

void flushAllBuffers();
void flushBuffer(const Canvas& c) { const_cast<Canvas&>(c).flush(); }

// The buffer, holding `c`'s calls: whoever held it is flushed first.
OpBuffer& claimOps(const Canvas& c, Canvas::WebOps& state) {
    OpBuffer& b = opBuffer();
    if (b.owner != &c) {
        if (b.owner) flushBuffer(*b.owner);
        b.owner = &c;
        b.state = &state;
    }
    return b;
}
// Drops the buffered calls unsent.
void discardOps(OpBuffer& b) {
    b.count = 0;
    b.text.clear(); b.paths.clear(); b.bytes.clear();
}
// `c` is going away or being moved: the buffer stops naming it. Anything it
// still holds was flushed by the caller, or can never be (no context).
void releaseOps(const Canvas* c) {
    OpBuffer& b = opBuffer();
    if (b.owner != c) return;
    discardOps(b);
    b.owner = nullptr;
    b.state = nullptr;
}

// Sends whatever is buffered, whichever canvas it belongs to: before a
// canvas's surface goes, since the calls may read it.
void flushOwner() {
    OpBuffer& b = opBuffer();
    if (b.owner && b.count > 0) flushBuffer(*b.owner);
}

// The interned strings by index (for a GPU canvas, which reads them back).
std::vector<std::string>& internedNames() { static auto* v = new std::vector<std::string>{std::string()}; return *v; }
// While set, interning never empties the table: the records being read name
// strings by index.
bool gInternLocked = false;
std::int32_t internStyle(const char* text) {
    if (!text || !*text) return 0;
    auto& table = interned();
    const std::string key(text);
    const auto found = table.find(key);
    if (found != table.end()) return found->second;
    if (table.size() >= kMaxInternedStrings && !gInternLocked) {
        // Buffered records name strings by index: every buffer has to go
        // before the table can be dropped.
        flushAllBuffers();
        table.clear();
        table.emplace(std::string(), 0);
        internedNames().assign(1, std::string());
        c2d_intern_reset();
    }
    const std::int32_t id = static_cast<std::int32_t>(table.size());
    table.emplace(key, id);
    internedNames().push_back(key);
    c2d_intern(id, key.c_str());
    return id;
}
}  // namespace

// The record for one call in `c`'s buffer, opcode written.
static OpSlot* opRecord(const Canvas& c, OpBuffer& b, int code);

// ===========================================================================
// Canvas internals that need the buffer.
// ===========================================================================
namespace {
// Bits of Canvas::dirty_.
enum : unsigned {
  kTransformBit = 1u << 0, kFillBit = 1u << 1, kStrokeBit = 1u << 2, kAlphaBit = 1u << 3,
  kCompositeBit = 1u << 4, kFilterBit = 1u << 5, kShadowBit = 1u << 6, kLineWidthBit = 1u << 7,
  kLineCapBit = 1u << 8, kLineJoinBit = 1u << 9, kMiterBit = 1u << 10, kDashBit = 1u << 11,
  kDashOffsetBit = 1u << 12, kFontBit = 1u << 13, kTextAlignBit = 1u << 14,
  kTextBaselineBit = 1u << 15, kDirectionBit = 1u << 16, kSmoothingBit = 1u << 17,
  kSmoothingQualityBit = 1u << 18, kLetterSpacingBit = 1u << 19, kWordSpacingBit = 1u << 20,
  kKerningBit = 1u << 21,
  kAllStateBits = (1u << 22) - 1,
};
constexpr unsigned kPaintBits = kAlphaBit | kCompositeBit | kFilterBit | kShadowBit;
constexpr unsigned kFillBits = kPaintBits | kFillBit;
constexpr unsigned kStrokeBits = kPaintBits | kStrokeBit | kLineWidthBit | kLineCapBit | kLineJoinBit | kMiterBit | kDashBit | kDashOffsetBit;
constexpr unsigned kTextBits = kFontBit | kTextAlignBit | kTextBaselineBit | kDirectionBit | kLetterSpacingBit | kWordSpacingBit | kKerningBit;
constexpr unsigned kImageBits = kPaintBits | kSmoothingBit | kSmoothingQualityBit;

enum : unsigned {
  kFitAffine = 1u << 0, kFitSimilar = 1u << 1, kFitAxis = 1u << 2, kFitPositive = 1u << 3, kFitTranslate = 1u << 4,
};
constexpr unsigned kFitUpright = kFitAffine | kFitSimilar | kFitAxis | kFitPositive;
constexpr float kTwoPiFloat = 6.2831853071795864f;

bool wholeTurn(float start, float end, bool ccw) { return ccw ? start - end >= kTwoPiFloat : end - start >= kTwoPiFloat; }
bool wholeTurn(const Path2D::Segment& g) {
  if (g.command == Path2D::Command::Arc) return wholeTurn(g.v[3], g.v[4], g.counterClockwise);
  if (g.command == Path2D::Command::Ellipse) return wholeTurn(g.v[5], g.v[6], g.counterClockwise);
  return false;
}
// A whole turn that is the path's first segment and is followed by nothing
// that runs on from its end: filled, it is the same disc from any start.
bool looseTurn(const std::vector<Path2D::Segment>& segs, std::size_t i) {
  if (i != 0 || segs.empty() || !wholeTurn(segs[0])) return false;
  const auto opens = [&](std::size_t j) {
    if (j >= segs.size()) return true;
    const Path2D::Command c = segs[j].command;
    return c == Path2D::Command::Move || c == Path2D::Command::Rect || c == Path2D::Command::RoundRect;
  };
  if (segs.size() > 1 && segs[1].command == Path2D::Command::Close) return opens(2);
  return opens(1);
}
unsigned segmentNeed(const Path2D::Segment& g) {
  switch (g.command) {
    case Path2D::Command::Arc:
    case Path2D::Command::Ellipse: return wholeTurn(g) ? kFitUpright : (kFitAffine | kFitSimilar);
    case Path2D::Command::ArcTo: return kFitAffine | kFitSimilar;
    case Path2D::Command::Rect: return kFitAffine | kFitAxis;
    case Path2D::Command::RoundRect: return kFitUpright;
    case Path2D::Command::Close: return 0;
    default: return kFitAffine;
  }
}
bool finiteMatrix(const std::array<double, 6>& m) {
  for (double v : m) if (!std::isfinite(v)) return false;
  return true;
}
CanvasPageFrame relativeFrame(const std::array<double, 6>& p, const std::array<double, 6>& m) {
  CanvasPageFrame r;
  r.fits = 0;
  if (!finiteMatrix(p) || !finiteMatrix(m)) return r;
  const double pNorm = std::max(std::max(std::abs(p[0]), std::abs(p[1])), std::max(std::abs(p[2]), std::abs(p[3])));
  const double pDet = p[0] * p[3] - p[1] * p[2];
  if (!(pNorm > 0) || !(std::abs(pDet) > 1e-9 * pNorm * pNorm)) return r;
  const double ia = p[3] / pDet, ib = -p[1] / pDet, ic = -p[2] / pDet, id = p[0] / pDet;
  const double ie = (p[2] * p[5] - p[3] * p[4]) / pDet, jf = (p[1] * p[4] - p[0] * p[5]) / pDet;
  r.a = ia * m[0] + ic * m[1]; r.b = ib * m[0] + id * m[1];
  r.c = ia * m[2] + ic * m[3]; r.d = ib * m[2] + id * m[3];
  r.e = ia * m[4] + ic * m[5] + ie; r.f = ib * m[4] + id * m[5] + jf;
  const double det = r.a * r.d - r.b * r.c;
  const double norm = std::max(std::max(std::abs(r.a), std::abs(r.b)), std::max(std::abs(r.c), std::abs(r.d)));
  if (!std::isfinite(det) || !(norm > 0) || !(std::abs(det) > 1e-9 * norm * norm)) return r;
  r.fits = kFitAffine;
  const double eps = 1e-9 * norm;
  r.sigma = det < 0 ? -1 : 1;
  const bool similar = r.sigma > 0 ? std::abs(r.a - r.d) <= eps && std::abs(r.b + r.c) <= eps
                                   : std::abs(r.a + r.d) <= eps && std::abs(r.b - r.c) <= eps;
  if (similar) { r.fits |= kFitSimilar; r.scale = std::sqrt(std::abs(det)); r.rotation = std::atan2(r.b, r.a); }
  if (std::abs(r.b) <= eps && std::abs(r.c) <= eps) {
    r.fits |= kFitAxis;
    if (r.a > 0 && r.d > 0) r.fits |= kFitPositive;
    if (std::abs(r.a - 1) <= 1e-9 && std::abs(r.d - 1) <= 1e-9) r.fits |= kFitTranslate;
  }
  return r;
}
float mapX(const CanvasPageFrame& r, float x, float y) { return static_cast<float>(r.a * x + r.c * y + r.e); }
float mapY(const CanvasPageFrame& r, float x, float y) { return static_cast<float>(r.b * x + r.d * y + r.f); }
void mapBox(const CanvasPageFrame& r, float& x, float& y, float& w, float& h) {
  const double nx = r.a * x + r.e, ny = r.d * y + r.f;
  w = static_cast<float>(w * r.a); h = static_cast<float>(h * r.d);
  x = static_cast<float>(nx); y = static_cast<float>(ny);
}
void mapAngles(float start, float end, bool ccw, double rotation, int sigma, float& outStart, float& outEnd) {
  const bool wasWhole = wholeTurn(start, end, ccw);
  const bool nowCcw = sigma < 0 ? !ccw : ccw;
  outStart = static_cast<float>(rotation + sigma * static_cast<double>(start));
  outEnd = static_cast<float>(static_cast<double>(outStart) + sigma * (static_cast<double>(end) - static_cast<double>(start)));
  for (int step = 0; step < 4 && wholeTurn(outStart, outEnd, nowCcw) != wasWhole; ++step) {
    const bool up = nowCcw ? !wasWhole : wasWhole;
    outEnd = std::nextafter(outEnd, up ? INFINITY : -INFINITY);
  }
}
Path2D::Segment mapSegment(const Path2D::Segment& g, const CanvasPageFrame& r, bool loose = false) {
  Path2D::Segment o = g;
  const float* v = g.v;
  const bool upright = (r.fits & kFitAxis) && (r.fits & kFitPositive);
  const auto point = [&](int at) { o.v[at] = mapX(r, v[at], v[at + 1]); o.v[at + 1] = mapY(r, v[at], v[at + 1]); };
  if (loose && !upright && wholeTurn(g)) {
    const bool ccw = r.sigma < 0 ? !g.counterClockwise : g.counterClockwise;
    const float end = ccw ? -kTwoPiFloat : kTwoPiFloat;
    o.counterClockwise = ccw;
    point(0);
    o.v[2] = static_cast<float>(v[2] * r.scale);
    if (g.command == Path2D::Command::Arc) { o.v[3] = 0.0f; o.v[4] = end; }
    else { o.v[3] = static_cast<float>(v[3] * r.scale); o.v[4] = static_cast<float>(r.rotation + r.sigma * static_cast<double>(v[4])); o.v[5] = 0.0f; o.v[6] = end; }
    return o;
  }
  switch (g.command) {
    case Path2D::Command::Move: case Path2D::Command::Line: point(0); break;
    case Path2D::Command::Quadratic: point(0); point(2); break;
    case Path2D::Command::Bezier: point(0); point(2); point(4); break;
    case Path2D::Command::Arc:
      point(0);
      o.v[2] = static_cast<float>(v[2] * r.scale);
      if (!upright) { mapAngles(v[3], v[4], g.counterClockwise, r.rotation, r.sigma, o.v[3], o.v[4]); o.counterClockwise = r.sigma < 0 ? !g.counterClockwise : g.counterClockwise; }
      break;
    case Path2D::Command::ArcTo: point(0); point(2); o.v[4] = static_cast<float>(v[4] * r.scale); break;
    case Path2D::Command::Ellipse:
      point(0);
      o.v[2] = static_cast<float>(v[2] * r.scale); o.v[3] = static_cast<float>(v[3] * r.scale);
      if (!upright) {
        o.v[4] = static_cast<float>(r.rotation + r.sigma * static_cast<double>(v[4]));
        mapAngles(v[5], v[6], g.counterClockwise, 0.0, r.sigma, o.v[5], o.v[6]);
        o.counterClockwise = r.sigma < 0 ? !g.counterClockwise : g.counterClockwise;
      }
      break;
    case Path2D::Command::Rect: mapBox(r, o.v[0], o.v[1], o.v[2], o.v[3]); break;
    case Path2D::Command::RoundRect: mapBox(r, o.v[0], o.v[1], o.v[2], o.v[3]); o.v[4] = static_cast<float>(v[4] * r.a); break;
    case Path2D::Command::Close: break;
  }
  return o;
}
canvas_detail::Matrix floatMatrix(const std::array<double, 6>& m) {
  return {static_cast<float>(m[0]), static_cast<float>(m[1]), static_cast<float>(m[2]), static_cast<float>(m[3]), static_cast<float>(m[4]), static_cast<float>(m[5])};
}
std::string css(Color c) {
  static constexpr char kHex[] = "0123456789abcdef";
  char buf[9] = {'#', kHex[c.r >> 4], kHex[c.r & 15], kHex[c.g >> 4], kHex[c.g & 15], kHex[c.b >> 4], kHex[c.b & 15], kHex[c.a >> 4], kHex[c.a & 15]};
  return std::string(buf, c.a == 255 ? 7 : 9);
}
}  // namespace

static OpSlot* opRecord(const Canvas& c, OpBuffer& b, int code) {
  // A full buffer is flushed and `c` keeps it. Anything a caller puts in an
  // arena is placed after this, so its offset is into the arena as it is now.
  if (b.count >= kOpCapacity || b.paths.size() >= kMaxArenaFloats || b.bytes.size() >= (32u << 20)) flushBuffer(c);
  if (b.count == 0) c2d_schedule_flush();
  OpSlot* r = b.slots.data() + static_cast<std::size_t>(b.count) * kOpSlots;
  b.meta[static_cast<std::size_t>(b.count)] = OpMeta{};
  ++b.count;
  if (code >= 0 && code < kCanvasOpCodes) ++gOpTypeCounts[code];
  r[0].i = code;
  for (int i = 1; i < kOpSlots; ++i) r[i].i = 0;
  return r;
}
#define BUF (claimOps(*this, *ops_))
static void push(const Canvas& c, OpBuffer& b, int code, std::initializer_list<float> args, const char* text = "") {
  const std::int32_t style = internStyle(text);
  OpSlot* r = opRecord(c, b, code);
  int i = 1;
  for (float a : args) r[i++].f = a;
  r[11].i = style;
}
static OpSlot* pushRef(const Canvas& c, OpBuffer& b, int code, std::int32_t ref, std::initializer_list<float> args) {
  OpSlot* r = opRecord(c, b, code);
  int i = 1;
  for (float a : args) r[i++].f = a;
  r[11].i = ref;
  return r;
}
#define OP(code, ...) push(*this, BUF, code, __VA_ARGS__)

// The meta of the record just written.
static OpMeta& lastMeta(OpBuffer& b) { return b.meta[static_cast<std::size_t>(b.count - 1)]; }

// ===========================================================================
// Culling.
// ===========================================================================
namespace {
bool boxCovered(const std::vector<std::uint64_t>& occ, int w, int x0, int y0, int x1, int y1) {
  const int words = (w + 63) / 64;
  for (int y = y0; y < y1; ++y) {
    const std::uint64_t* row = occ.data() + static_cast<std::size_t>(y) * words;
    int x = x0;
    while (x < x1) {
      const int wi = x >> 6, bit = x & 63, span = std::min(64 - bit, x1 - x);
      const std::uint64_t want = (span == 64 ? ~0ull : ((1ull << span) - 1)) << bit;
      if ((row[wi] & want) != want) return false;
      x += span;
    }
  }
  return true;
}
void markBox(std::vector<std::uint64_t>& occ, int w, int h, int x0, int y0, int x1, int y1) {
  x0 = std::max(0, x0); y0 = std::max(0, y0); x1 = std::min(w, x1); y1 = std::min(h, y1);
  const int words = (w + 63) / 64;
  for (int y = y0; y < y1; ++y) {
    std::uint64_t* row = occ.data() + static_cast<std::size_t>(y) * words;
    int x = x0;
    while (x < x1) {
      const int wi = x >> 6, bit = x & 63, span = std::min(64 - bit, x1 - x);
      row[wi] |= (span == 64 ? ~0ull : ((1ull << span) - 1)) << bit;
      x += span;
    }
  }
}
// Walks a buffer backwards and turns invisible paints into no-ops.
int cullBuffer(OpBuffer& b, int w, int h, std::int32_t currentGen) {
  if (b.count < 2) return 0;
  // Clamped in float first: an unbounded paint's box is +-1e30, and a float
  // to int conversion out of range traps in WebAssembly.
  const auto toInt = [](float v) { return static_cast<int>(std::clamp(v, -1e8f, 1e8f)); };
  // The occlusion map covers only what this batch's occluders cover, not the
  // whole canvas: a batch is often a handful of calls between two switches of
  // canvas, and clearing a full-canvas map for each cost more than it saved.
  int mx0 = w, my0 = h, mx1 = 0, my1 = 0;
  for (int i = 0; i < b.count; ++i) {
    const OpMeta& m = b.meta[static_cast<std::size_t>(i)];
    if (m.kind != 1 || !(m.ox0 < m.ox1 && m.oy0 < m.oy1)) continue;
    mx0 = std::min(mx0, std::max(0, m.ox0)); my0 = std::min(my0, std::max(0, m.oy0));
    mx1 = std::max(mx1, std::min(w, m.ox1)); my1 = std::max(my1, std::min(h, m.oy1));
  }
  const bool occluders = mx0 < mx1 && my0 < my1;
  const int mw = occluders ? mx1 - mx0 : 0, mh = occluders ? my1 - my0 : 0;
  bool any = false;
  int culled = 0;
  // Page-path generations some surviving record still needs -- always the
  // current one, which a paint in the NEXT buffer may still use. A batch's
  // generations run from its first path to `currentGen` (a new one per
  // beginPath), so a flag per generation in that range.
  std::int32_t firstGen = currentGen;
  for (int i = 0; i < b.count; ++i) {
    const OpMeta& m = b.meta[static_cast<std::size_t>(i)];
    if (m.kind == 2 || m.usesPagePath) { firstGen = std::min(firstGen, m.pathGen); break; }
  }
  static std::vector<unsigned char> livePaths;
  livePaths.assign(static_cast<std::size_t>(currentGen - firstGen) + 1, 0);
  livePaths.back() = 1;
  const auto live = [&](std::int32_t g) { return g < firstGen || g > currentGen || livePaths[static_cast<std::size_t>(g - firstGen)]; };
  const auto keep = [&](std::int32_t g) { if (g >= firstGen && g <= currentGen) livePaths[static_cast<std::size_t>(g - firstGen)] = 1; };
  int layerDepth = 0;  // walking backwards: inside a layer while > 0
  for (int i = b.count - 1; i >= 0; --i) {
    OpMeta& m = b.meta[static_cast<std::size_t>(i)];
    OpSlot* r = b.slots.data() + static_cast<std::size_t>(i) * kOpSlots;
    const int code = r[0].i;
    if (code == 60) ++layerDepth;
    if (code == 59 && layerDepth > 0) --layerDepth;
    if (m.kind == 2) {
      // Path building: dropped with its generation when nothing uses it.
      if (!live(m.pathGen)) { r[0].i = kNop; }
      continue;
    }
    if (m.kind != 1 || layerDepth > 0) {
      if (m.usesPagePath) keep(m.pathGen);
      continue;
    }
    const int x0 = std::max(0, toInt(std::floor(m.x0))), y0 = std::max(0, toInt(std::floor(m.y0)));
    const int x1 = std::min(w, toInt(std::ceil(m.x1)) + 1), y1 = std::min(h, toInt(std::ceil(m.y1)) + 1);
    const bool emptyBox = x0 >= x1 || y0 >= y1;
    // Covered only if the box lies inside the map, and the map says so.
    const bool inMap = any && x0 >= mx0 && y0 >= my0 && x1 <= mx1 && y1 <= my1;
    if (emptyBox || (inMap && boxCovered(b.occlusion, mw, x0 - mx0, y0 - my0, x1 - mx0, y1 - my0))) {
      ++culled;
      if (m.keepsPath) r[1].f = 3.f;               // keep the geometry, skip the paint
      else if (m.tempImage) r[0].i = 57;            // just drop the upload
      else r[0].i = kNop;
      continue;
    }
    if (m.usesPagePath) keep(m.pathGen);
    if (occluders && m.ox0 < m.ox1 && m.oy0 < m.oy1) {
      if (!any) { b.occlusion.assign(static_cast<std::size_t>((mw + 63) / 64) * mh, 0); any = true; }
      markBox(b.occlusion, mw, mh, m.ox0 - mx0, m.oy0 - my0, m.ox1 - mx0, m.oy1 - my0);
    }
  }
  return culled;
}
}  // namespace

// ===========================================================================
// Flushing.
// ===========================================================================
namespace {
std::vector<std::string>& droppedObjects() { static auto* v = new std::vector<std::string>(); return *v; }
std::vector<int>& droppedSnapshots() { static auto* v = new std::vector<int>(); return *v; }
struct ImageKey { std::uint32_t key; int w, h; bool opaque; };
std::unordered_map<std::uint32_t, ImageKey>& imageKeys() { static auto* m = new std::unordered_map<std::uint32_t, ImageKey>(); return *m; }
std::deque<ImageKey>& imageOrder() { static auto* d = new std::deque<ImageKey>(); return *d; }
}  // namespace

// Page objects go once nothing can name them: at the next full flush.
CanvasGradient::Data::~Data() { if (webId) droppedObjects().push_back("g" + std::to_string(webId)); }
CanvasPattern::Data::~Data() {
  if (webId) droppedObjects().push_back("p" + std::to_string(webId));
  if (webSnapshot) droppedSnapshots().push_back(webSnapshot);
}

void canvasFlushOps() { flushAllBuffers(); }
extern "C" EMSCRIPTEN_KEEPALIVE void cpp_canvas_flush_all() { flushAllBuffers(); }

namespace {
void presentGpuCanvases();
void flushAllBuffers() {
  flushOwner();
  presentGpuCanvases();
  // Page objects nothing can name any more.
  for (const std::string& name : droppedObjects()) c2d_object_drop(name.c_str());
  droppedObjects().clear();
  for (int id : droppedSnapshots()) c2d_pattern_source_drop(id);
  droppedSnapshots().clear();
}
}  // namespace

#include <optional>
#include "webgpu.h"

// ===========================================================================
// Canvas: construction.
// ===========================================================================
Canvas::Canvas(int w, int h, std::string id, bool desynchronized)
    : width_(std::max(1, w)), height_(std::max(1, h)), elementId_(std::move(id)), logicalWidth_(width_), logicalHeight_(height_) {
  ops_ = std::make_unique<WebOps>();
  // WebGL2 when the page has it: the element is the GPU's, and the canvas's
  // 2D context a hidden surface of its own for what the GPU replays.
  if (!desynchronized && c2d_gpu_wanted()) {
    contextId_ = c2d_create("", width_, height_, 1, 0);
    auto gpu = std::make_shared<WebGpu>();
    if (gpu->init(elementId_, width_, height_, contextId_)) {
      ops_->gpu = gpu;
      gpuCanvases()[this] = gpu.get();
    } else {
      c2d_destroy(contextId_);
      contextId_ = -1;
    }
  }
  if (contextId_ < 0) contextId_ = c2d_create(elementId_.c_str(), width_, height_, 0, desynchronized ? 1 : 0);
  liveCanvases()[contextId_] = this;
}
Canvas::Canvas(int w, int h, bool virtualCanvas)
    : width_(std::max(1, w)), height_(std::max(1, h)), virtual_(virtualCanvas), logicalWidth_(width_), logicalHeight_(height_) {
  contextId_ = c2d_create("", width_, height_, 1, 0);
  liveCanvases()[contextId_] = this;
  ops_ = std::make_unique<WebOps>();
}
Canvas Canvas::createVirtual(int w, int h) { return Canvas(w, h, true); }
void Canvas::setLogicalSize(int w, int h) { logicalWidth_ = w > 0 ? w : width_; logicalHeight_ = h > 0 ? h : height_; }


Canvas::Canvas(Canvas&& o) noexcept
    : width_(o.width_), height_(o.height_), contextId_(o.contextId_), virtual_(o.virtual_), elementId_(std::move(o.elementId_)),
      logicalWidth_(o.logicalWidth_), logicalHeight_(o.logicalHeight_), fill_(o.fill_), stroke_(o.stroke_), lineWidth_(o.lineWidth_),
      currentPath_(std::move(o.currentPath_)), tint_(o.tint_), cullingEnabled_(o.cullingEnabled_) {
  // Everything that names the old object by address goes first.
  o.flush();
  releaseOps(&o);
  o.contextId_ = -1;
  if (contextId_ >= 0) liveCanvases()[contextId_] = this;
  web_ = std::move(o.web_); webStack_ = std::move(o.webStack_);
  applied_ = std::move(o.applied_); appliedStack_ = std::move(o.appliedStack_);
  dirty_ = o.dirty_; o.dirty_ = 0;
  pathMode_ = o.pathMode_; pagePathDirty_ = o.pagePathDirty_; pagePathSegments_ = o.pagePathSegments_; pathMatrix_ = o.pathMatrix_;
  pathHasSubpath_ = o.pathHasSubpath_; pageSeamMoved_ = o.pageSeamMoved_; pathUniform_ = o.pathUniform_;
  layerDepth_ = o.layerDepth_;
  ops_ = std::move(o.ops_);
  o.ops_ = std::make_unique<WebOps>();
  gpuCanvases().erase(&o);
  if (ops_->gpu) gpuCanvases()[this] = ops_->gpu.get();
}
Canvas& Canvas::operator=(Canvas&& o) noexcept {
  if (this == &o) return *this;
  if (contextId_ >= 0) { flushOwner(); c2d_destroy(contextId_); liveCanvases().erase(contextId_); }
  releaseOps(this);
  o.flush();
  releaseOps(&o);
  width_ = o.width_; height_ = o.height_; contextId_ = o.contextId_; virtual_ = o.virtual_;
  elementId_ = std::move(o.elementId_); fill_ = o.fill_; stroke_ = o.stroke_; lineWidth_ = o.lineWidth_;
  currentPath_ = std::move(o.currentPath_); o.contextId_ = -1; tint_ = o.tint_; cullingEnabled_ = o.cullingEnabled_;
  logicalWidth_ = o.logicalWidth_; logicalHeight_ = o.logicalHeight_;
  if (contextId_ >= 0) liveCanvases()[contextId_] = this;
  web_ = std::move(o.web_); webStack_ = std::move(o.webStack_);
  applied_ = std::move(o.applied_); appliedStack_ = std::move(o.appliedStack_);
  dirty_ = o.dirty_; o.dirty_ = 0;
  pathMode_ = o.pathMode_; pagePathDirty_ = o.pagePathDirty_; pagePathSegments_ = o.pagePathSegments_; pathMatrix_ = o.pathMatrix_;
  pathHasSubpath_ = o.pathHasSubpath_; pageSeamMoved_ = o.pageSeamMoved_; pathUniform_ = o.pathUniform_;
  layerDepth_ = o.layerDepth_;
  gpuCanvases().erase(this);
  ops_ = std::move(o.ops_);
  o.ops_ = std::make_unique<WebOps>();
  gpuCanvases().erase(&o);
  if (ops_->gpu) gpuCanvases()[this] = ops_->gpu.get();
  return *this;
}
Canvas::~Canvas() {
  // Whatever is buffered -- this canvas's calls, or another's that read it --
  // goes before the surface does.
  if (contextId_ >= 0) {
    flushOwner();
    c2d_destroy(contextId_);
    liveCanvases().erase(contextId_);
  }
  gpuCanvases().erase(this);
  releaseOps(this);
}
void Canvas::present(const std::string& id) {
  flush();
  if (ops_->gpu) ops_->gpu->syncShadow();
  c2d_present(contextId_, id.c_str());
}

// ----- dependencies -----
// With one buffer there is nothing to track: a canvas that records while
// another holds the buffer flushes it first, so calls reach the page in the
// order they were made, reads included.
void Canvas::dependOn(const Canvas&) const {}
void Canvas::beforeWrite() const {}
void Canvas::dropDependencies() const {}

#include "web_canvas.h"
