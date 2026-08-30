#include "canvas.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <utility>
#ifndef __EMSCRIPTEN__
#include <SDL.h>
#endif

namespace {
Path2D::Segment segment(Path2D::Command command, std::initializer_list<float> values, bool ccw = false) {
    Path2D::Segment result{}; result.command = command; result.counterClockwise = ccw;
    std::copy(values.begin(), values.end(), result.v); return result;
}
}

void Path2D::moveTo(float x, float y) { segments_.push_back(segment(Command::Move, {x, y})); }
void Path2D::lineTo(float x, float y) { segments_.push_back(segment(Command::Line, {x, y})); }
void Path2D::closePath() { segments_.push_back(segment(Command::Close, {})); }
void Path2D::quadraticCurveTo(float a,float b,float c,float d) { segments_.push_back(segment(Command::Quadratic,{a,b,c,d})); }
void Path2D::bezierCurveTo(float a,float b,float c,float d,float e,float f) { segments_.push_back(segment(Command::Bezier,{a,b,c,d,e,f})); }
void Path2D::arc(float a,float b,float c,float d,float e,bool f) { segments_.push_back(segment(Command::Arc,{a,b,c,d,e},f)); }
void Path2D::arcTo(float a,float b,float c,float d,float e) { segments_.push_back(segment(Command::ArcTo,{a,b,c,d,e})); }
void Path2D::ellipse(float a,float b,float c,float d,float e,float f,float g,bool h) { segments_.push_back(segment(Command::Ellipse,{a,b,c,d,e,f,g},h)); }
void Path2D::rect(float a,float b,float c,float d) { segments_.push_back(segment(Command::Rect,{a,b,c,d})); }
void Path2D::roundRect(float a,float b,float c,float d,float e) { segments_.push_back(segment(Command::RoundRect,{a,b,c,d,e})); }
void Path2D::addPath(const Path2D& other) { segments_.insert(segments_.end(), other.segments_.begin(), other.segments_.end()); }

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// One registry makes each C++ Canvas an independent CanvasRenderingContext2D.
EM_JS(int, c2d_create, (const char* element, int width, int height, int virtualCanvas), {
  Module.cppCanvasContexts ||= [];
  let surface;
  if (virtualCanvas) surface = typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(width, height) : document.createElement('canvas');
  else { const id = UTF8ToString(element); surface = document.getElementById(id) || Object.assign(document.createElement('canvas'), {id}); if (!surface.parentNode) document.body.appendChild(surface); }
  surface.width = width; surface.height = height;
  Module.cppCanvasContexts.push({surface, ctx: surface.getContext('2d')});
  return Module.cppCanvasContexts.length - 1;
});
EM_JS(void, c2d_destroy, (int id), { if (Module.cppCanvasContexts) Module.cppCanvasContexts[id] = null; });
EM_JS(void, c2d_present, (int id, const char* target), {
  const item = Module.cppCanvasContexts[id], name = UTF8ToString(target);
  let canvas = document.getElementById(name);
  if (!canvas) { canvas = document.createElement('canvas'); canvas.id = name; document.body.appendChild(canvas); }
  canvas.width = item.surface.width; canvas.height = item.surface.height; canvas.getContext('2d').drawImage(item.surface, 0, 0);
});
EM_JS(void, c2d_op, (int id, int op, double a,double b,double c,double d,double e,double f,double g,double h, const char* text), {
  const x = Module.cppCanvasContexts[id].ctx, s = text ? UTF8ToString(text) : "";
  switch(op) {
    case 0:x.save();break; case 1:x.restore();break; case 2: x.reset ? x.reset() : (x.setTransform(1,0,0,1,0,0),x.clearRect(0,0,x.canvas.width,x.canvas.height));break;
    case 3:x.scale(a,b);break; case 4:x.rotate(a);break; case 5:x.translate(a,b);break; case 6:x.transform(a,b,c,d,e,f);break; case 7:x.setTransform(a,b,c,d,e,f);break; case 8:x.resetTransform();break;
    case 9:x.fillStyle=s;break; case 10:x.strokeStyle=s;break; case 11:x.globalAlpha=a;break; case 12:x.globalCompositeOperation=s;break; case 13:x.filter=s;break;
    case 14:x.lineWidth=a;break; case 15:x.lineCap=s;break; case 16:x.lineJoin=s;break; case 17:x.miterLimit=a;break; case 18:x.lineDashOffset=a;break;
    case 19:x.shadowColor=s;x.shadowBlur=a;x.shadowOffsetX=b;x.shadowOffsetY=c;break; case 20:x.font=s;break; case 21:x.textAlign=s;break; case 22:x.textBaseline=s;break; case 23:x.direction=s;break; case 24:x.imageSmoothingEnabled=!!a;break; case 25:x.imageSmoothingQuality=s;break;
    case 30:x.clearRect(a,b,c,d);break; case 31:x.fillRect(a,b,c,d);break; case 32:x.strokeRect(a,b,c,d);break;
    case 33:x.beginPath();break; case 34:x.closePath();break; case 35:x.moveTo(a,b);break; case 36:x.lineTo(a,b);break; case 37:x.quadraticCurveTo(a,b,c,d);break; case 38:x.bezierCurveTo(a,b,c,d,e,f);break; case 39:x.arc(a,b,c,d,e,!!f);break; case 40:x.arcTo(a,b,c,d,e);break; case 41:x.ellipse(a,b,c,d,e,f,g,!!h);break; case 42:x.rect(a,b,c,d);break; case 43:x.roundRect(a,b,c,d,e);break;
    case 44:x.fill(s || 'nonzero');break; case 45:x.stroke();break; case 46:x.clip(s || 'nonzero');break;
    case 47:a < 0 ? x.fillText(s,b,c) : x.fillText(s,b,c,a);break; case 48:a < 0 ? x.strokeText(s,b,c) : x.strokeText(s,b,c,a);break;
  }
});
EM_JS(void, c2d_dash, (int id, const float* data, int length), { Module.cppCanvasContexts[id].ctx.setLineDash(Array.from(HEAPF32.subarray(data>>2,(data>>2)+length))); });
EM_JS(void, c2d_path, (int id, const float* data, int count, int action, const char* rule), {
  const x=Module.cppCanvasContexts[id].ctx, p=new Path2D(), v=HEAPF32.subarray(data>>2,(data>>2)+count*10), r=UTF8ToString(rule);
  for(let i=0;i<count;i++){const q=i*10,n=v[q],cc=!!v[q+1],a=v.subarray(q+2,q+10); switch(n){case 0:p.moveTo(a[0],a[1]);break;case 1:p.lineTo(a[0],a[1]);break;case 2:p.quadraticCurveTo(a[0],a[1],a[2],a[3]);break;case 3:p.bezierCurveTo(a[0],a[1],a[2],a[3],a[4],a[5]);break;case 4:p.arc(a[0],a[1],a[2],a[3],a[4],cc);break;case 5:p.arcTo(a[0],a[1],a[2],a[3],a[4]);break;case 6:p.ellipse(a[0],a[1],a[2],a[3],a[4],a[5],a[6],cc);break;case 7:p.rect(a[0],a[1],a[2],a[3]);break;case 8:p.roundRect(a[0],a[1],a[2],a[3],a[4]);break;case 9:p.closePath();break;}}
  if(action===0)x.fill(p,r||'nonzero'); else if(action===1)x.stroke(p); else x.clip(p,r||'nonzero');
});
EM_JS(int, c2d_hit, (int id,double a,double b,int stroke,const char* rule), { const x=Module.cppCanvasContexts[id].ctx; return stroke ? x.isPointInStroke(a,b) : x.isPointInPath(a,b,UTF8ToString(rule)); });
EM_JS(double, c2d_measure, (int id,const char* text), { return Module.cppCanvasContexts[id].ctx.measureText(UTF8ToString(text)).width; });
EM_JS(void, c2d_draw, (int dst,int src,double a,double b,double c,double d,int sized), { const x=Module.cppCanvasContexts[dst].ctx, image=Module.cppCanvasContexts[src].surface; sized ? x.drawImage(image,a,b,c,d) : x.drawImage(image,a,b); });
EM_JS(int, c2d_get_pixels, (int id,int x,int y,int w,int h,std::uint8_t* out), { const d=Module.cppCanvasContexts[id].ctx.getImageData(x,y,w,h).data; HEAPU8.set(d,out); return d.length; });
EM_JS(void, c2d_put_pixels, (int id,const std::uint8_t* data,int sw,int sh,int dx,int dy), { const d=new ImageData(new Uint8ClampedArray(HEAPU8.slice(data,data+sw*sh*4)),sw,sh); Module.cppCanvasContexts[id].ctx.putImageData(d,dx,dy); });
#define OP(code,a,b,c,d,e,f,g,h,s) c2d_op(contextId_,code,a,b,c,d,e,f,g,h,s)
#else
#define OP(code,a,b,c,d,e,f,g,h,s) ((void)0)
#endif

Canvas::Canvas(int w,int h,std::string id) : width_(std::max(1,w)),height_(std::max(1,h)),elementId_(std::move(id)) {
#ifdef __EMSCRIPTEN__
  contextId_=c2d_create(elementId_.c_str(),width_,height_,0);
#else
  pixels_.assign(static_cast<size_t>(width_)*height_,Color{255,255,255});
#endif
}
Canvas::Canvas(int w,int h,bool virtualCanvas) : width_(std::max(1,w)),height_(std::max(1,h)),virtual_(virtualCanvas) {
#ifdef __EMSCRIPTEN__
  contextId_=c2d_create("",width_,height_,1);
#else
  pixels_.assign(static_cast<size_t>(width_)*height_,Color{0,0,0,0});
#endif
}
Canvas Canvas::createVirtual(int w,int h) { return Canvas(w,h,true); }
Canvas::Canvas(Canvas&& other) noexcept
    : width_(other.width_), height_(other.height_), contextId_(other.contextId_), virtual_(other.virtual_),
      elementId_(std::move(other.elementId_)), fill_(other.fill_), stroke_(other.stroke_),
      lineWidth_(other.lineWidth_), currentPath_(std::move(other.currentPath_)), pixels_(std::move(other.pixels_)) {
  other.contextId_ = -1;
}
Canvas& Canvas::operator=(Canvas&& other) noexcept {
  if (this == &other) return *this;
#ifdef __EMSCRIPTEN__
  if (contextId_ >= 0) c2d_destroy(contextId_);
#endif
  width_=other.width_; height_=other.height_; contextId_=other.contextId_; virtual_=other.virtual_;
  elementId_=std::move(other.elementId_); fill_=other.fill_; stroke_=other.stroke_; lineWidth_=other.lineWidth_;
  currentPath_=std::move(other.currentPath_); pixels_=std::move(other.pixels_); other.contextId_=-1;
  return *this;
}
Canvas::~Canvas() {
#ifdef __EMSCRIPTEN__
  if (contextId_ >= 0) c2d_destroy(contextId_);
#endif
}
void Canvas::present(const std::string& id) {
#ifdef __EMSCRIPTEN__
  c2d_present(contextId_, id.c_str());
#else
  (void)id;
#endif
}
static std::string css(Color c) { return "rgba("+std::to_string(c.r)+","+std::to_string(c.g)+","+std::to_string(c.b)+","+std::to_string(c.a/255.0f)+")"; }
void Canvas::save() {
  OP(0,0,0,0,0,0,0,0,0,"");
#ifndef __EMSCRIPTEN__
  matrixStack_.push_back(matrix_);
#endif
}
void Canvas::restore() {
  OP(1,0,0,0,0,0,0,0,0,"");
#ifndef __EMSCRIPTEN__
  if (!matrixStack_.empty()) { matrix_=matrixStack_.back(); matrixStack_.pop_back(); }
#endif
}
void Canvas::reset() { OP(2,0,0,0,0,0,0,0,0,""); resetTransform(); }
void Canvas::scale(float a,float b) { OP(3,a,b,0,0,0,0,0,0,"");
#ifndef __EMSCRIPTEN__
  matrix_[0]*=a; matrix_[1]*=a; matrix_[2]*=b; matrix_[3]*=b;
#endif
}
void Canvas::rotate(float a) { OP(4,a,0,0,0,0,0,0,0,"");
#ifndef __EMSCRIPTEN__
  const float c=std::cos(a), s=std::sin(a), m0=matrix_[0],m1=matrix_[1],m2=matrix_[2],m3=matrix_[3];
  matrix_[0]=m0*c+m2*s; matrix_[1]=m1*c+m3*s; matrix_[2]=m2*c-m0*s; matrix_[3]=m3*c-m1*s;
#endif
}
void Canvas::translate(float a,float b) { OP(5,a,b,0,0,0,0,0,0,"");
#ifndef __EMSCRIPTEN__
  matrix_[4]+=matrix_[0]*a+matrix_[2]*b; matrix_[5]+=matrix_[1]*a+matrix_[3]*b;
#endif
}
void Canvas::transform(float a,float b,float c,float d,float e,float f) {
  OP(6,a,b,c,d,e,f,0,0,"");
#ifndef __EMSCRIPTEN__
  const auto m=matrix_; matrix_[0]=m[0]*a+m[2]*b; matrix_[1]=m[1]*a+m[3]*b;
  matrix_[2]=m[0]*c+m[2]*d; matrix_[3]=m[1]*c+m[3]*d; matrix_[4]=m[0]*e+m[2]*f+m[4]; matrix_[5]=m[1]*e+m[3]*f+m[5];
#endif
}
void Canvas::setTransform(float a,float b,float c,float d,float e,float f) { OP(7,a,b,c,d,e,f,0,0,"");
#ifndef __EMSCRIPTEN__
  matrix_={a,b,c,d,e,f};
#endif
}
void Canvas::resetTransform() { OP(8,0,0,0,0,0,0,0,0,"");
#ifndef __EMSCRIPTEN__
  matrix_={1,0,0,1,0,0};
#endif
}
void Canvas::setFillStyle(Color c){fill_=c;setFillStyle(css(c));} void Canvas::setStrokeStyle(Color c){stroke_=c;setStrokeStyle(css(c));} void Canvas::setFillStyle(const std::string&s){OP(9,0,0,0,0,0,0,0,0,s.c_str());} void Canvas::setStrokeStyle(const std::string&s){OP(10,0,0,0,0,0,0,0,0,s.c_str());}
void Canvas::setGlobalAlpha(float a){OP(11,a,0,0,0,0,0,0,0,"");} void Canvas::setGlobalCompositeOperation(const std::string&s){OP(12,0,0,0,0,0,0,0,0,s.c_str());} void Canvas::setFilter(const std::string&s){OP(13,0,0,0,0,0,0,0,0,s.c_str());} void Canvas::setLineWidth(float a){lineWidth_=std::max(0.f,a);OP(14,lineWidth_,0,0,0,0,0,0,0,"");}
void Canvas::setLineCap(const std::string&s){OP(15,0,0,0,0,0,0,0,0,s.c_str());} void Canvas::setLineJoin(const std::string&s){OP(16,0,0,0,0,0,0,0,0,s.c_str());} void Canvas::setMiterLimit(float a){OP(17,a,0,0,0,0,0,0,0,"");}
void Canvas::setLineDash(const std::vector<float>&v) {
#ifdef __EMSCRIPTEN__
  c2d_dash(contextId_, v.data(), static_cast<int>(v.size()));
#else
  (void)v;
#endif
}
void Canvas::setLineDashOffset(float a){OP(18,a,0,0,0,0,0,0,0,"");}
void Canvas::setShadow(Color c,float a,float b,float d){auto s=css(c);OP(19,a,b,d,0,0,0,0,0,s.c_str());} void Canvas::setFont(const std::string&s){OP(20,0,0,0,0,0,0,0,0,s.c_str());} void Canvas::setTextAlign(const std::string&s){OP(21,0,0,0,0,0,0,0,0,s.c_str());} void Canvas::setTextBaseline(const std::string&s){OP(22,0,0,0,0,0,0,0,0,s.c_str());} void Canvas::setDirection(const std::string&s){OP(23,0,0,0,0,0,0,0,0,s.c_str());} void Canvas::setImageSmoothingEnabled(bool a){OP(24,a,0,0,0,0,0,0,0,"");} void Canvas::setImageSmoothingQuality(const std::string&s){OP(25,0,0,0,0,0,0,0,0,s.c_str());}
void Canvas::clear(Color c) {
#ifdef __EMSCRIPTEN__
  save(); resetTransform(); setFillStyle(c); fillRect(0,0,width_,height_); restore();
#else
  std::fill(pixels_.begin(), pixels_.end(), c);
#endif
}
void Canvas::clearRect(float a,float b,float c,float d) {
  OP(30,a,b,c,d,0,0,0,0,"");
#ifndef __EMSCRIPTEN__
  for (int y=std::max(0,(int)std::floor(b)); y<std::min(height_,(int)std::ceil(b+d)); ++y)
    for (int x=std::max(0,(int)std::floor(a)); x<std::min(width_,(int)std::ceil(a+c)); ++x)
      pixels_[size_t(y)*width_+x]=Color{0,0,0,0};
#endif
}
void Canvas::fillRect(float a,float b,float c,float d) {
  OP(31,a,b,c,d,0,0,0,0,"");
#ifndef __EMSCRIPTEN__
  for(int y=std::max(0,(int)std::floor(b));y<std::min(height_,(int)std::ceil(b+d));++y)
    for(int x=std::max(0,(int)std::floor(a));x<std::min(width_,(int)std::ceil(a+c));++x) blendPixel(x,y,fill_);
#endif
}
void Canvas::strokeRect(float a,float b,float c,float d) {
  OP(32,a,b,c,d,0,0,0,0,"");
#ifndef __EMSCRIPTEN__
  drawLine(a,b,a+c,b);drawLine(a+c,b,a+c,b+d);drawLine(a+c,b+d,a,b+d);drawLine(a,b+d,a,b);
#endif
}
void Canvas::beginPath(){currentPath_=Path2D{};OP(33,0,0,0,0,0,0,0,0,"");} void Canvas::closePath(){currentPath_.closePath();OP(34,0,0,0,0,0,0,0,0,"");} void Canvas::moveTo(float a,float b){currentPath_.moveTo(a,b);OP(35,a,b,0,0,0,0,0,0,"");} void Canvas::lineTo(float a,float b){currentPath_.lineTo(a,b);OP(36,a,b,0,0,0,0,0,0,"");}
void Canvas::quadraticCurveTo(float a,float b,float c,float d){currentPath_.quadraticCurveTo(a,b,c,d);OP(37,a,b,c,d,0,0,0,0,"");} void Canvas::bezierCurveTo(float a,float b,float c,float d,float e,float f){currentPath_.bezierCurveTo(a,b,c,d,e,f);OP(38,a,b,c,d,e,f,0,0,"");} void Canvas::arc(float a,float b,float c,float d,float e,bool f){currentPath_.arc(a,b,c,d,e,f);OP(39,a,b,c,d,e,f,0,0,"");} void Canvas::arcTo(float a,float b,float c,float d,float e){currentPath_.arcTo(a,b,c,d,e);OP(40,a,b,c,d,e,0,0,0,"");} void Canvas::ellipse(float a,float b,float c,float d,float e,float f,float g,bool h){currentPath_.ellipse(a,b,c,d,e,f,g,h);OP(41,a,b,c,d,e,f,g,h,"");} void Canvas::rect(float a,float b,float c,float d){currentPath_.rect(a,b,c,d);OP(42,a,b,c,d,0,0,0,0,"");} void Canvas::roundRect(float a,float b,float c,float d,float e){currentPath_.roundRect(a,b,c,d,e);OP(43,a,b,c,d,e,0,0,0,"");}
void Canvas::fill(const std::string&s){
  OP(44,0,0,0,0,0,0,0,0,s.c_str());
#ifndef __EMSCRIPTEN__
  fill(currentPath_, s);
#endif
}
void Canvas::stroke() {
  OP(45,0,0,0,0,0,0,0,0,"");
#ifndef __EMSCRIPTEN__
  const auto& v=currentPath_.segments(); float x=0,y=0,startX=0,startY=0;
  for(auto&q:v) {
    if(q.command==Path2D::Command::Move) { x=startX=q.v[0]; y=startY=q.v[1]; }
    else if(q.command==Path2D::Command::Line) { const auto a=mapPoint(x,y),b=mapPoint(q.v[0],q.v[1]); drawLine(a.first,a.second,b.first,b.second); x=q.v[0]; y=q.v[1]; }
    else if(q.command==Path2D::Command::Close) { const auto a=mapPoint(x,y),b=mapPoint(startX,startY); drawLine(a.first,a.second,b.first,b.second); x=startX; y=startY; }
  }
#endif
}
void Canvas::clip(const std::string&s){OP(46,0,0,0,0,0,0,0,0,s.c_str());}
static std::vector<float> pack(const Path2D&p){std::vector<float>r;p.segments().size();r.reserve(p.segments().size()*10);for(auto&q:p.segments()){r.push_back((float)q.command);r.push_back(q.counterClockwise);for(float x:q.v)r.push_back(x);}return r;}
void Canvas::fill(const Path2D&p,const std::string&s) {
#ifdef __EMSCRIPTEN__
  auto v=pack(p); c2d_path(contextId_,v.data(),static_cast<int>(p.segments().size()),0,s.c_str());
#else
  (void)s;
  // Even-odd scan conversion for line/rectangle SVG paths. Curved segments are
  // intentionally approximated by their endpoint in this software fallback.
  std::vector<std::vector<std::pair<float,float>>> contours;
  std::vector<std::pair<float,float>> contour;
  const auto point = [this](float x, float y) { return mapPoint(x, y); };
  for (const auto& q : p.segments()) {
    if (q.command == Path2D::Command::Move) { if (!contour.empty()) contours.push_back(contour); contour={point(q.v[0],q.v[1])}; }
    else if (q.command == Path2D::Command::Line) contour.push_back(point(q.v[0],q.v[1]));
    else if (q.command == Path2D::Command::Rect) {
      if (!contour.empty()) contours.push_back(contour);
      contour={point(q.v[0],q.v[1]),point(q.v[0]+q.v[2],q.v[1]),point(q.v[0]+q.v[2],q.v[1]+q.v[3]),point(q.v[0],q.v[1]+q.v[3])};
    } else if (q.command == Path2D::Command::RoundRect) {
      if (!contour.empty()) contours.push_back(contour);
      contour.clear();
      const float x=q.v[0], y=q.v[1], w=q.v[2], h=q.v[3];
      const float r=std::clamp(q.v[4], 0.f, std::min(std::abs(w),std::abs(h))/2.f);
      const float left=std::min(x,x+w), right=std::max(x,x+w), top=std::min(y,y+h), bottom=std::max(y,y+h);
      // Clockwise quarter-arcs make the same retained path work in software and Canvas 2D.
      for (int corner=0;corner<4;++corner) {
        const float cx=corner==0?right-r:corner==1?right-r:corner==2?left+r:left+r;
        const float cy=corner==0?top+r:corner==1?bottom-r:corner==2?bottom-r:top+r;
        const float start=-1.5707963f+corner*1.5707963f;
        for (int step=0;step<=8;++step) { const float t=start+step*1.5707963f/8; contour.push_back(point(cx+std::cos(t)*r,cy+std::sin(t)*r)); }
      }
    }
    else if (q.command == Path2D::Command::Arc || q.command == Path2D::Command::Ellipse) {
      if (!contour.empty()) contours.push_back(contour); contour.clear();
      const float cx=q.v[0], cy=q.v[1], rx=q.v[2], ry=q.command==Path2D::Command::Ellipse?q.v[3]:q.v[2];
      for (int i=0;i<32;++i) { float t=6.2831853f*i/32; contour.push_back(point(cx+std::cos(t)*rx,cy+std::sin(t)*ry)); }
    } else if (q.command == Path2D::Command::Quadratic) contour.push_back(point(q.v[2],q.v[3]));
    else if (q.command == Path2D::Command::Bezier) contour.push_back(point(q.v[4],q.v[5]));
  }
  if (!contour.empty()) contours.push_back(contour);
  if (contours.empty()) return;
  float minX=static_cast<float>(width_), minY=static_cast<float>(height_), maxX=0, maxY=0;
  for (const auto& c : contours) for (const auto& point : c) {
    minX=std::min(minX,point.first); minY=std::min(minY,point.second);
    maxX=std::max(maxX,point.first); maxY=std::max(maxY,point.second);
  }
  const int startX=std::max(0,static_cast<int>(std::floor(minX)));
  const int startY=std::max(0,static_cast<int>(std::floor(minY)));
  const int endX=std::min(width_,static_cast<int>(std::ceil(maxX)));
  const int endY=std::min(height_,static_cast<int>(std::ceil(maxY)));
  for (int y=startY;y<endY;++y) for (int x=startX;x<endX;++x) {
    bool inside=false; const float py=y+.5f, px=x+.5f;
    for (const auto& c:contours) for(size_t i=0,j=c.size()?c.size()-1:0;i<c.size();j=i++) {
      const auto& a=c[i];const auto& b=c[j];
      if ((a.second>py)!=(b.second>py) && px<(b.first-a.first)*(py-a.second)/(b.second-a.second)+a.first) inside=!inside;
    }
    if(inside) blendPixel(x,y,fill_);
  }
#endif
}
void Canvas::stroke(const Path2D&p) {
#ifdef __EMSCRIPTEN__
  auto v=pack(p); c2d_path(contextId_,v.data(),static_cast<int>(p.segments().size()),1,"");
#else
  auto old=currentPath_; currentPath_=p; stroke(); currentPath_=std::move(old);
#endif
}
void Canvas::clip(const Path2D&p,const std::string&s) {
#ifdef __EMSCRIPTEN__
  auto v=pack(p); c2d_path(contextId_,v.data(),static_cast<int>(p.segments().size()),2,s.c_str());
#else
  (void)p; (void)s;
#endif
}
bool Canvas::isPointInPath(float a,float b,const std::string&s)const {
#ifdef __EMSCRIPTEN__
  return c2d_hit(contextId_,a,b,0,s.c_str());
#else
  (void)a;(void)b;(void)s; return false;
#endif
}
bool Canvas::isPointInStroke(float a,float b)const {
#ifdef __EMSCRIPTEN__
  return c2d_hit(contextId_,a,b,1,"");
#else
  (void)a;(void)b; return false;
#endif
}
void Canvas::fillText(const std::string&s,float a,float b,float c){OP(47,c,a,b,0,0,0,0,0,s.c_str());}
void Canvas::strokeText(const std::string&s,float a,float b,float c){OP(48,c,a,b,0,0,0,0,0,s.c_str());}
float Canvas::measureText(const std::string&s)const {
#ifdef __EMSCRIPTEN__
  return c2d_measure(contextId_,s.c_str());
#else
  (void)s; return 0;
#endif
}
void Canvas::drawCanvas(const Canvas&s,float a,float b) {
#ifdef __EMSCRIPTEN__
  c2d_draw(contextId_,s.contextId_,a,b,0,0,0);
#else
  drawCanvas(s, a, b, static_cast<float>(s.width_), static_cast<float>(s.height_));
#endif
}
void Canvas::drawCanvas(const Canvas&s,float a,float b,float c,float d) {
#ifdef __EMSCRIPTEN__
  c2d_draw(contextId_,s.contextId_,a,b,c,d,1);
#else
  if (c <= 0 || d <= 0) return;
  for (int y = std::max(0, static_cast<int>(std::floor(b))); y < std::min(height_, static_cast<int>(std::ceil(b + d))); ++y)
    for (int x = std::max(0, static_cast<int>(std::floor(a))); x < std::min(width_, static_cast<int>(std::ceil(a + c))); ++x) {
      const int sx = std::clamp(static_cast<int>((x - a) * s.width_ / c), 0, s.width_ - 1);
      const int sy = std::clamp(static_cast<int>((y - b) * s.height_ / d), 0, s.height_ - 1);
      blendPixel(x, y, s.pixels_[static_cast<size_t>(sy) * s.width_ + sx]);
    }
#endif
}
std::vector<std::uint8_t> Canvas::getImageData(int a,int b,int c,int d)const {
  std::vector<std::uint8_t>r(std::max(0,c)*std::max(0,d)*4);
#ifdef __EMSCRIPTEN__
  c2d_get_pixels(contextId_,a,b,c,d,r.data());
#else
  for(int y=0;y<d;y++)for(int x=0;x<c;x++) if(a+x>=0&&b+y>=0&&a+x<width_&&b+y<height_){auto p=pixels_[size_t(b+y)*width_+a+x];auto i=(size_t(y)*c+x)*4;r[i]=p.r;r[i+1]=p.g;r[i+2]=p.b;r[i+3]=p.a;}
#endif
  return r;
}
void Canvas::putImageData(const std::vector<std::uint8_t>&r,int a,int b,int c,int d) {
  if(r.size() < size_t(a)*b*4) return;
#ifdef __EMSCRIPTEN__
  c2d_put_pixels(contextId_,r.data(),a,b,c,d);
#else
  for(int y=0;y<b;y++)for(int x=0;x<a;x++){auto i=(size_t(y)*a+x)*4;blendPixel(c+x,d+y,Color{r[i],r[i+1],r[i+2],r[i+3]});}
#endif
}
void Canvas::fillCircle(float a,float b,float c){beginPath();arc(a,b,c,0,6.283185307f);fill();}void Canvas::strokeCircle(float a,float b,float c){beginPath();arc(a,b,c,0,6.283185307f);stroke();}
bool Canvas::savePPM(const std::string& path)const {
#ifdef __EMSCRIPTEN__
  (void)path; return true;
#else
  std::ofstream f(path,std::ios::binary); if(!f)return false;
  f<<"P6\n"<<width_<<" "<<height_<<"\n255\n"; for(auto&p:pixels_){f.put(p.r);f.put(p.g);f.put(p.b);} return !!f;
#endif
}

bool Canvas::showWindow(const std::string& title, const std::function<void(Canvas&, float)>& drawFrame) {
#ifdef __EMSCRIPTEN__
  (void)title; (void)drawFrame;
  return true;
#else
  if (SDL_Init(SDL_INIT_VIDEO) != 0) return false;
  SDL_Window* window = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                        width_, height_, SDL_WINDOW_SHOWN);
  SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
  SDL_Texture* texture = renderer ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width_, height_) : nullptr;
  if (!texture) { if(renderer) SDL_DestroyRenderer(renderer); if(window) SDL_DestroyWindow(window); SDL_Quit(); return false; }
  std::vector<std::uint8_t> rgba(static_cast<size_t>(width_) * height_ * 4);
  for (size_t i=0;i<pixels_.size();++i) {
    const Color& p=pixels_[i]; const float a=p.a/255.f;
    rgba[i*4]=static_cast<std::uint8_t>(p.r*a+255*(1-a)); rgba[i*4+1]=static_cast<std::uint8_t>(p.g*a+255*(1-a));
    rgba[i*4+2]=static_cast<std::uint8_t>(p.b*a+255*(1-a)); rgba[i*4+3]=255;
  }
  const Uint64 start=SDL_GetPerformanceCounter(), frequency=SDL_GetPerformanceFrequency();
  bool running=true; while(running) { const Uint64 frameStart=SDL_GetPerformanceCounter(); SDL_Event event; while(SDL_PollEvent(&event)) if(event.type==SDL_QUIT) running=false;
    if (drawFrame) drawFrame(*this, static_cast<float>(SDL_GetPerformanceCounter()-start)/frequency);
    for (size_t i=0;i<pixels_.size();++i) {
      const Color& p=pixels_[i]; const float a=p.a/255.f;
      rgba[i*4]=static_cast<std::uint8_t>(p.r*a+255*(1-a)); rgba[i*4+1]=static_cast<std::uint8_t>(p.g*a+255*(1-a));
      rgba[i*4+2]=static_cast<std::uint8_t>(p.b*a+255*(1-a)); rgba[i*4+3]=255;
    }
    SDL_UpdateTexture(texture, nullptr, rgba.data(), width_ * 4);
    SDL_RenderClear(renderer); SDL_RenderCopy(renderer, texture, nullptr, nullptr); SDL_RenderPresent(renderer);
    const Uint32 elapsed=static_cast<Uint32>((SDL_GetPerformanceCounter()-frameStart)*1000/frequency);
    if (elapsed < 16) SDL_Delay(16-elapsed);
  }
  SDL_DestroyTexture(texture); SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit(); return true;
#endif
}
#ifndef __EMSCRIPTEN__
std::pair<float,float> Canvas::mapPoint(float x,float y) const { return {matrix_[0]*x+matrix_[2]*y+matrix_[4], matrix_[1]*x+matrix_[3]*y+matrix_[5]}; }
void Canvas::blendPixel(int x,int y,Color c) {
  if(x<0||y<0||x>=width_||y>=height_) return;
  Color& d=pixels_[size_t(y)*width_+x];
  const float sa=c.a/255.f, da=d.a/255.f, oa=sa+da*(1-sa);
  if(oa==0) { d=Color{0,0,0,0}; return; }
  d=Color{static_cast<std::uint8_t>((c.r*sa+d.r*da*(1-sa))/oa),
          static_cast<std::uint8_t>((c.g*sa+d.g*da*(1-sa))/oa),
          static_cast<std::uint8_t>((c.b*sa+d.b*da*(1-sa))/oa),
          static_cast<std::uint8_t>(oa*255)};
}
void Canvas::drawLine(float a,float b,float c,float d){int n=std::max(1,(int)std::ceil(std::max(std::abs(c-a),std::abs(d-b))));for(int i=0;i<=n;i++){float t=float(i)/n;int x=std::lround(a+(c-a)*t),y=std::lround(b+(d-b)*t);for(int yy=y-lineWidth_/2;yy<=y+lineWidth_/2;yy++)for(int xx=x-lineWidth_/2;xx<=x+lineWidth_/2;xx++)blendPixel(xx,yy,stroke_);}}
#endif
