#include "svg.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <regex>
#include <sstream>
#include <unordered_map>

namespace {
using Attributes = std::unordered_map<std::string, std::string>;
Attributes attributes(const std::string& text) {
    Attributes result;
    const std::regex pair(R"(([A-Za-z_:][-A-Za-z0-9_:.]*)\s*=\s*["']([^"']*)["'])");
    for (std::sregex_iterator i(text.begin(), text.end(), pair), end; i != end; ++i) result[(*i)[1]] = (*i)[2];
    return result;
}
float number(const Attributes& a, const char* key, float fallback = 0) {
    const auto it = a.find(key); if (it == a.end()) return fallback;
    try { return std::stof(it->second); } catch (...) { return fallback; }
}
float duration(const std::string& value) {
    try { return value.find("ms") != std::string::npos ? std::stof(value) / 1000.f : std::stof(value); } catch (...) { return 0; }
}
Color color(const std::string& value, Color fallback) {
    if (value == "none") return Color{0,0,0,0};
    if (value.size() == 7 && value[0] == '#') {
        try { return Color{static_cast<std::uint8_t>(std::stoi(value.substr(1,2),nullptr,16)),static_cast<std::uint8_t>(std::stoi(value.substr(3,2),nullptr,16)),static_cast<std::uint8_t>(std::stoi(value.substr(5,2),nullptr,16))}; } catch (...) {}
    }
    if (value == "red") return Color{255,0,0}; if (value == "green") return Color{0,128,0}; if (value == "blue") return Color{0,0,255};
    if (value == "white") return Color{255,255,255}; if (value == "black") return Color{0,0,0};
    return fallback;
}
std::vector<float> floats(const std::string& value) {
    std::vector<float> out; const std::regex n(R"([-+]?(?:\d*\.?\d+)(?:[eE][-+]?\d+)?)");
    for (std::sregex_iterator i(value.begin(),value.end(),n), end; i != end; ++i) out.push_back(std::stof((*i).str()));
    return out;
}
Path2D parsePath(const std::string& source) {
    std::vector<std::string> tokens; const std::regex re(R"([AaCcHhLlMmQqSsTtVvZz]|[-+]?(?:\d*\.?\d+)(?:[eE][-+]?\d+)?)");
    for (std::sregex_iterator i(source.begin(),source.end(),re), end; i != end; ++i) tokens.push_back((*i).str());
    Path2D path; size_t i=0; char cmd=0; float x=0,y=0,startX=0,startY=0;
    auto n = [&]() { return i<tokens.size() ? std::stof(tokens[i++]) : 0.f; };
    while (i < tokens.size()) {
        if (std::isalpha(static_cast<unsigned char>(tokens[i][0]))) cmd=tokens[i++][0];
        const bool relative=std::islower(static_cast<unsigned char>(cmd));
        switch (std::toupper(static_cast<unsigned char>(cmd))) {
        case 'M': { x=n();y=n();if(relative){x+=startX;y+=startY;}path.moveTo(x,y);startX=x;startY=y;cmd=relative?'l':'L';break; }
        case 'L': { float nx=n(),ny=n();if(relative){nx+=x;ny+=y;}path.lineTo(nx,ny);x=nx;y=ny;break; }
        case 'H': { float nx=n();if(relative)nx+=x;path.lineTo(nx,y);x=nx;break; }
        case 'V': { float ny=n();if(relative)ny+=y;path.lineTo(x,ny);y=ny;break; }
        case 'C': { float a=n(),b=n(),c=n(),d=n(),e=n(),f=n();if(relative){a+=x;b+=y;c+=x;d+=y;e+=x;f+=y;}path.bezierCurveTo(a,b,c,d,e,f);x=e;y=f;break; }
        case 'Q': { float a=n(),b=n(),c=n(),d=n();if(relative){a+=x;b+=y;c+=x;d+=y;}path.quadraticCurveTo(a,b,c,d);x=c;y=d;break; }
        case 'A': { n();n();n();n();n();float nx=n(),ny=n();if(relative){nx+=x;ny+=y;}path.lineTo(nx,ny);x=nx;y=ny;break; } // endpoint fallback for elliptical SVG arcs
        case 'Z': path.closePath();x=startX;y=startY;break;
        default: ++i; break;
        }
    }
    return path;
}
}

struct SvgDocument::Shape {
    std::string kind;
    Attributes attrs;
    struct Animation { bool transform = false; Attributes attrs; };
    std::vector<Animation> animations;
};

SvgDocument::~SvgDocument() = default;

SvgDocument SvgDocument::fromFile(const std::string& path) {
    std::ifstream input(path); std::stringstream source; source << input.rdbuf(); return fromString(source.str());
}

SvgDocument SvgDocument::fromString(const std::string& source) {
    SvgDocument document;
    const std::regex tag(R"(<\s*([A-Za-z][A-Za-z0-9]*)\b([^>]*)>)");
    for (std::sregex_iterator i(source.begin(), source.end(), tag), end; i != end; ++i) {
        const std::string name=(*i)[1], body=(*i)[2]; const auto attrs=attributes(body);
        if (name == "svg") { document.width_=number(attrs,"width"); document.height_=number(attrs,"height"); continue; }
        if (name == "animate" || name == "animateTransform") {
            if (!document.shapes_.empty()) document.shapes_.back().animations.push_back({name=="animateTransform",attrs});
            continue;
        }
        if (name=="rect" || name=="circle" || name=="ellipse" || name=="line" || name=="polyline" || name=="polygon" || name=="path") document.shapes_.push_back({name,attrs,{}});
        else if (name=="linearGradient" || name=="radialGradient" || name=="image" || name=="text") document.warnings_.push_back("Unsupported SVG element: "+name);
    }
    return document;
}

bool SvgDocument::render(Canvas& canvas, float time) const {
    for (const Shape& shape : shapes_) {
        Attributes a=shape.attrs;
        float opacity=number(a,"opacity",1);
        for (const auto& animation : shape.animations) {
            const float dur=duration(animation.attrs.count("dur")?animation.attrs.at("dur"):"0");
            if (dur <= 0) continue;
            float t=std::fmod(std::max(0.f,time),dur)/dur;
            if (animation.attrs.count("repeatCount") && animation.attrs.at("repeatCount")=="1") t=std::clamp(time/dur,0.f,1.f);
            const auto& name=animation.attrs.count("attributeName")?animation.attrs.at("attributeName"):"";
            auto values=floats(animation.attrs.count("values")?animation.attrs.at("values"):"");
            if (values.empty()) values={number(animation.attrs,"from"),number(animation.attrs,"to")};
            if (animation.transform) {
                const std::string type=animation.attrs.count("type")?animation.attrs.at("type"):"translate";
                canvas.save();
                if (type=="translate") canvas.translate(values[0]*(1-t)+(values.size()>1?values[1]:0)*t, values.size()>3?values[1]*(1-t)+values[3]*t:0);
                else if (type=="rotate") canvas.rotate((values[0]*(1-t)+(values.size()>1?values[1]:0)*t)*3.14159265f/180.f);
            } else if (values.size() >= 2) a[name]=std::to_string(values[0]*(1-t)+values[1]*t);
        }
        canvas.save();
        canvas.setGlobalAlpha(opacity);
        if (a.count("fill")) canvas.setFillStyle(color(a["fill"],Color{0,0,0}));
        if (a.count("stroke")) canvas.setStrokeStyle(color(a["stroke"],Color{0,0,0}));
        canvas.setLineWidth(number(a,"stroke-width",1));
        Path2D path;
        if (shape.kind=="rect") { if(number(a,"rx")>0) path.roundRect(number(a,"x"),number(a,"y"),number(a,"width"),number(a,"height"),number(a,"rx")); else path.rect(number(a,"x"),number(a,"y"),number(a,"width"),number(a,"height")); }
        else if (shape.kind=="circle") path.ellipse(number(a,"cx"),number(a,"cy"),number(a,"r"),number(a,"r"),0,0,6.2831853f);
        else if (shape.kind=="ellipse") path.ellipse(number(a,"cx"),number(a,"cy"),number(a,"rx"),number(a,"ry"),0,0,6.2831853f);
        else if (shape.kind=="line") { path.moveTo(number(a,"x1"),number(a,"y1"));path.lineTo(number(a,"x2"),number(a,"y2")); }
        else if (shape.kind=="polyline" || shape.kind=="polygon") { auto p=floats(a["points"]);if(p.size()>=2){path.moveTo(p[0],p[1]);for(size_t j=2;j+1<p.size();j+=2)path.lineTo(p[j],p[j+1]);if(shape.kind=="polygon")path.closePath();} }
        else path=parsePath(a["d"]);
        if (a.find("fill")==a.end() && shape.kind!="line" || (a.count("fill") && a["fill"]!="none")) canvas.fill(path, a.count("fill-rule")?a["fill-rule"]:"nonzero");
        if (a.count("stroke") && a["stroke"]!="none") canvas.stroke(path);
        canvas.restore();
        for (const auto& animation:shape.animations) if(animation.transform) canvas.restore();
    }
    return true;
}
