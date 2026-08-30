#pragma once

#include "canvas.h"

#include <string>
#include <vector>

// A small SVG scene compiler. render() evaluates SVG animations at timeSeconds
// and emits only Canvas/Path2D commands; it never asks the browser to render SVG.
class SvgDocument {
public:
    ~SvgDocument();
    static SvgDocument fromString(const std::string& source);
    static SvgDocument fromFile(const std::string& path);

    float width() const { return width_; }
    float height() const { return height_; }
    bool render(Canvas& canvas, float timeSeconds = 0.0f) const;
    const std::vector<std::string>& warnings() const { return warnings_; }

private:
    struct Shape;
    float width_ = 0, height_ = 0;
    std::vector<Shape> shapes_;
    std::vector<std::string> warnings_;
};
