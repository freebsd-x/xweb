// 2D canvases: the raster work behind CanvasRenderingContext2D. The context itself (its
// state, transforms and paths) is written in JavaScript in the prelude; what it draws
// comes here, as polygons in canvas pixels, onto a Canvas kept for each <canvas>.

#include <cstring>

#include "script_impl.h"
#include "xweb/css.h"
#include "xweb/style.h"

namespace fbx::xweb {

using js::Object;
using js::PropertyKey;
using js::Runtime;
using js::Value;

namespace {

// A canvas's size from its attributes: 300 by 150 unless they say otherwise.
std::pair<uint32_t, uint32_t> sizeOf(const Node* canvas) {
    const auto dim = [&](const char* name, uint32_t fallback) {
        const std::string* v = canvas->attr(name);
        if (!v) return fallback;
        char* end = nullptr;
        const long n = std::strtol(v->c_str(), &end, 10);
        if (end == v->c_str() || n < 0) return fallback;
        return static_cast<uint32_t>(std::min<long>(n, kMaxRasterSide));
    };
    uint32_t w = dim("width", 300), h = dim("height", 150);
    // Not more than a raster may cost: what's beyond is left off.
    while (static_cast<uint64_t>(w) * h > kMaxRasterPixels && h > 1) h /= 2;
    return {w, h};
}

// Numbers a typed array holds (Float32Array or Int32Array), as floats.
std::vector<float> floatsOf(Runtime& rt, const Value& v, bool ints) {
    std::string bytes;
    std::vector<float> out;
    if (!rt.bufferBytes(v, bytes)) return out;
    const size_t n = bytes.size() / 4;
    out.resize(n);
    for (size_t i = 0; i < n; ++i) {
        if (ints) {
            int32_t x;
            std::memcpy(&x, &bytes[i * 4], 4);
            out[i] = static_cast<float>(x);
        } else {
            std::memcpy(&out[i], &bytes[i * 4], 4);
        }
    }
    return out;
}

// Points and the length of each contour, flattened by the prelude, back into polygons.
Contours contoursOf(Runtime& rt, const Value& points, const Value& counts) {
    const std::vector<float> p = floatsOf(rt, points, false);
    const std::vector<float> c = floatsOf(rt, counts, true);
    Contours out;
    size_t at = 0;
    for (const float n : c) {
        const size_t count = static_cast<size_t>(std::max(0.0f, n));
        if (at + count * 2 > p.size()) break;
        std::vector<Point> contour(count);
        for (size_t i = 0; i < count; ++i) contour[i] = {p[at + i * 2], p[at + i * 2 + 1]};
        at += count * 2;
        out.push_back(std::move(contour));
    }
    return out;
}

bool colorOf(const std::string& text, Color& out) {
    const std::vector<css::Token> tokens = css::tokenize(text);
    return parseColor(tokens, Color{0, 0, 0, 255}, out);
}

Matrix matrixOf(Runtime& rt, const Value& v) {
    const std::vector<float> m = floatsOf(rt, v, false);
    if (m.size() < 6) return {};
    return {m[0], m[1], m[2], m[3], m[4], m[5]};
}

// A fill or stroke style: a CSS colour, or a gradient the prelude described
// ({kind, coords: Float32Array, stops: [[offset, colour]...]}) with the transform it's under.
bool paintOf(Runtime& rt, const Value& style, const Value& matrix, float alpha, Paint& out) {
    out.opacity = std::clamp(alpha, 0.0f, 1.0f);
    if (style.isString()) return colorOf(js::utf8(style.asString()), out.color);
    if (!style.isObject()) return false;
    auto g = std::make_shared<GradientPaint>();
    const std::string kind = js::utf8(rt.toStringValue(rt.get(style, u"kind")));
    const std::vector<float> c = floatsOf(rt, rt.get(style, u"coords"), false);
    if (kind == "linear" && c.size() >= 4) {
        g->kind = GradientPaint::Kind::Linear;
        g->x1 = c[0], g->y1 = c[1], g->x2 = c[2], g->y2 = c[3];
    } else if (kind == "radial" && c.size() >= 6) {
        // Two circles; drawn as the second, focused at the first's centre.
        g->kind = GradientPaint::Kind::Radial;
        g->fx = c[0], g->fy = c[1], g->cx = c[3], g->cy = c[4], g->r = std::max(0.001f, c[5]);
    } else if (kind == "conic" && c.size() >= 3) {
        g->kind = GradientPaint::Kind::Conic;
        g->start_angle = c[0] + 1.5707963f;   // canvas starts at 3 o'clock, ours at 12
        g->cx = c[1], g->cy = c[2];
    } else {
        return false;
    }
    const Value stops = rt.get(style, u"stops");
    const double n = rt.toNumber(rt.get(stops, u"length"));
    for (double i = 0; i < n && i < 256; ++i) {
        const Value s = rt.get(stops, PropertyKey(Runtime::numberToString(i)));
        ColorStop stop;
        stop.offset = std::clamp(static_cast<float>(rt.toNumber(rt.get(s, u"0"))), 0.0f, 1.0f);
        if (!colorOf(js::utf8(rt.toStringValue(rt.get(s, u"1"))), stop.color)) continue;
        g->stops.push_back(stop);
    }
    if (g->stops.empty()) return false;
    std::stable_sort(g->stops.begin(), g->stops.end(), [](const ColorStop& a, const ColorStop& b) { return a.offset < b.offset; });
    g->to_canvas = matrixOf(rt, matrix);
    out.gradient = std::move(g);
    return true;
}

// A PNG of these pixels, its image data in stored (uncompressed) deflate blocks.
std::string encodePng(const Raster& r) {
    static uint32_t crc_table[256];
    static bool made = false;
    if (!made) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[n] = c;
        }
        made = true;
    }
    const auto crc = [&](const std::string& data) {
        uint32_t c = 0xFFFFFFFFu;
        for (const char ch : data) c = crc_table[(c ^ static_cast<uint8_t>(ch)) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    };
    const auto be32 = [](std::string& s, uint32_t v) {
        for (int i = 3; i >= 0; --i) s += static_cast<char>((v >> (i * 8)) & 0xFF);
    };
    std::string out = "\x89PNG\r\n\x1a\n";
    const auto chunk = [&](const char* type, const std::string& data) {
        be32(out, static_cast<uint32_t>(data.size()));
        const std::string body = std::string(type, 4) + data;
        out += body;
        be32(out, crc(body));
    };
    std::string ihdr;
    be32(ihdr, r.width);
    be32(ihdr, r.height);
    ihdr += std::string("\x08\x06\x00\x00\x00", 5);   // 8 bits, RGBA, deflate, no filter, no interlace
    chunk("IHDR", ihdr);
    std::string raw;
    raw.reserve(static_cast<size_t>(r.height) * (r.width * 4 + 1));
    for (uint32_t y = 0; y < r.height; ++y) {
        raw += '\0';
        raw.append(reinterpret_cast<const char*>(&r.rgba[static_cast<size_t>(y) * r.width * 4]), static_cast<size_t>(r.width) * 4);
    }
    std::string z = "\x78\x01";   // zlib, no compression
    for (size_t at = 0; at < raw.size() || at == 0;) {
        const size_t len = std::min<size_t>(65535, raw.size() - at);
        const bool last = at + len >= raw.size();
        z += static_cast<char>(last ? 1 : 0);
        z += static_cast<char>(len & 0xFF);
        z += static_cast<char>(len >> 8);
        z += static_cast<char>(~len & 0xFF);
        z += static_cast<char>((~len >> 8) & 0xFF);
        z.append(raw, at, len);
        at += len;
        if (last) break;
    }
    uint32_t a = 1, b = 0;
    for (const char ch : raw) {
        a = (a + static_cast<uint8_t>(ch)) % 65521;
        b = (b + a) % 65521;
    }
    be32(z, (b << 16) | a);
    chunk("IDAT", z);
    chunk("IEND", "");
    return out;
}

std::string base64(const std::string& in) {
    static const char* const table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        const uint32_t n = (static_cast<uint8_t>(in[i]) << 16) | (i + 1 < in.size() ? static_cast<uint8_t>(in[i + 1]) << 8 : 0) |
                           (i + 2 < in.size() ? static_cast<uint8_t>(in[i + 2]) : 0);
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
        out += i + 1 < in.size() ? table[(n >> 6) & 63] : '=';
        out += i + 2 < in.size() ? table[n & 63] : '=';
    }
    return out;
}

}  // namespace

PageScript::Impl::CanvasSurface& PageScript::Impl::surfaceOf(Node* canvas) {
    auto& slot = canvases[canvas];
    if (!slot) {
        slot = std::make_unique<CanvasSurface>();
        const auto [w, h] = sizeOf(canvas);
        slot->pixels = std::make_unique<Canvas>(std::max(1u, w), std::max(1u, h));
    }
    return *slot;
}

void PageScript::Impl::installCanvas() {
    Impl* self = this;
    Object* n = rt.newObject();
    const auto canvasArg = [self](Runtime& r, Args a) -> Node* {
        Node* c = self->node(arg(a, 0));
        if (!c || !c->isHtml("canvas")) r.throwError("TypeError", "not a canvas");
        return c;
    };
    // Something was drawn: the page shows it at the next paint.
    const auto changed = [self](CanvasSurface& s) {
        ++s.version;
        self->dirty = true;
    };
    // Draws `draw` onto the canvas, through its clip if it has one.
    const auto clipped = [](CanvasSurface& s, const std::function<void(Canvas&)>& draw) {
        if (!s.clip) {
            draw(*s.pixels);
            return;
        }
        Canvas layer(s.pixels->width(), s.pixels->height());
        draw(layer);
        s.pixels->composite(layer, 1, s.clip.get());
    };
    rt.method(n, u"size", 1, [canvasArg](Runtime& r, const Value&, Args a) {
        const auto [w, h] = sizeOf(canvasArg(r, a));
        return Value::object(r.newArray({Value::number(w), Value::number(h)}));
    });
    // A new size (or the same one set again): a fresh, clear bitmap.
    rt.method(n, u"reset", 1, [self, canvasArg, changed](Runtime& r, const Value&, Args a) {
        Node* c = canvasArg(r, a);
        auto it = self->canvases.find(c);
        if (it == self->canvases.end()) return Value::undefined();
        const auto [w, h] = sizeOf(c);
        it->second->pixels = std::make_unique<Canvas>(std::max(1u, w), std::max(1u, h));
        it->second->clip.reset();
        changed(*it->second);
        return Value::undefined();
    });
    // fill(canvas, points, counts, evenOdd, style, matrix, alpha)
    rt.method(n, u"fill", 7, [self, canvasArg, changed, clipped](Runtime& r, const Value&, Args a) {
        CanvasSurface& s = self->surfaceOf(canvasArg(r, a));
        const Contours shape = contoursOf(r, arg(a, 1), arg(a, 2));
        Paint paint;
        if (shape.empty() || !paintOf(r, arg(a, 4), arg(a, 5), static_cast<float>(r.toNumber(arg(a, 6))), paint)) return Value::undefined();
        const bool even_odd = r.toBoolean(arg(a, 3));
        clipped(s, [&](Canvas& c) { c.fill(shape, even_odd, paint); });
        changed(s);
        return Value::undefined();
    });
    // stroke(canvas, points, counts, closed (Int32Array), width, cap, join, miter, style, matrix, alpha)
    rt.method(n, u"stroke", 11, [self, canvasArg, changed, clipped](Runtime& r, const Value&, Args a) {
        CanvasSurface& s = self->surfaceOf(canvasArg(r, a));
        const Contours lines = contoursOf(r, arg(a, 1), arg(a, 2));
        const std::vector<float> closed = floatsOf(r, arg(a, 3), true);
        StrokeStyle style;
        style.width = std::max(0.0f, static_cast<float>(r.toNumber(arg(a, 4))));
        style.cap = static_cast<uint8_t>(std::clamp(r.toNumber(arg(a, 5)), 0.0, 2.0));
        style.join = static_cast<uint8_t>(std::clamp(r.toNumber(arg(a, 6)), 0.0, 2.0));
        style.miter_limit = static_cast<float>(r.toNumber(arg(a, 7)));
        Paint paint;
        if (lines.empty() || style.width <= 0 || !paintOf(r, arg(a, 8), arg(a, 9), static_cast<float>(r.toNumber(arg(a, 10))), paint))
            return Value::undefined();
        Contours outline;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].size() < 2) continue;
            Contours part = strokeOutline(lines[i], i < closed.size() && closed[i] != 0, style, 1);
            outline.insert(outline.end(), part.begin(), part.end());
        }
        clipped(s, [&](Canvas& c) { c.fill(outline, false, paint); });
        changed(s);
        return Value::undefined();
    });
    // clip(canvas, points, counts, evenOdd): the clip narrows to this shape too.
    rt.method(n, u"clip", 4, [self, canvasArg](Runtime& r, const Value&, Args a) {
        CanvasSurface& s = self->surfaceOf(canvasArg(r, a));
        Canvas shape = Canvas::coverage(s.pixels->width(), s.pixels->height(), contoursOf(r, arg(a, 1), arg(a, 2)), r.toBoolean(arg(a, 3)));
        if (s.clip) {
            Canvas both(s.pixels->width(), s.pixels->height());
            both.composite(shape, 1, s.clip.get());
            s.clip = std::make_unique<Canvas>(std::move(both));
        } else {
            s.clip = std::make_unique<Canvas>(std::move(shape));
        }
        return Value::undefined();
    });
    rt.method(n, u"unclip", 1, [self, canvasArg](Runtime& r, const Value&, Args a) {
        self->surfaceOf(canvasArg(r, a)).clip.reset();
        return Value::undefined();
    });
    rt.method(n, u"clearRect", 5, [self, canvasArg, changed](Runtime& r, const Value&, Args a) {
        CanvasSurface& s = self->surfaceOf(canvasArg(r, a));
        const auto i = [&](size_t k) { return static_cast<int>(std::clamp(std::floor(r.toNumber(arg(a, k))), -1e8, 1e8)); };
        const int x = i(1), y = i(2);
        const int x1 = static_cast<int>(std::clamp(std::ceil(r.toNumber(arg(a, 1)) + r.toNumber(arg(a, 3))), -1e8, 1e8));
        const int y1 = static_cast<int>(std::clamp(std::ceil(r.toNumber(arg(a, 2)) + r.toNumber(arg(a, 4))), -1e8, 1e8));
        s.pixels->clearRect(x, y, x1 - x, y1 - y);
        changed(s);
        return Value::undefined();
    });
    // getImageData(canvas, x, y, w, h): the RGBA bytes, unpremultiplied.
    rt.method(n, u"read", 5, [self, canvasArg](Runtime& r, const Value&, Args a) {
        CanvasSurface& s = self->surfaceOf(canvasArg(r, a));
        const auto i = [&](size_t k) { return static_cast<int>(std::clamp(r.toNumber(arg(a, k)), -1e8, 1e8)); };
        const int w = i(3), h = i(4);
        if (w <= 0 || h <= 0 || static_cast<uint64_t>(w) * h > kMaxRasterPixels) r.throwError("RangeError", "IndexSizeError: bad image data size");
        const Raster got = s.pixels->readRect(i(1), i(2), w, h);
        return Value::object(r.newArrayBuffer(std::string_view(reinterpret_cast<const char*>(got.rgba.data()), got.rgba.size())));
    });
    // putImageData(canvas, bytes, w, h, dx, dy)
    rt.method(n, u"write", 6, [self, canvasArg, changed](Runtime& r, const Value&, Args a) {
        CanvasSurface& s = self->surfaceOf(canvasArg(r, a));
        Raster img;
        img.width = static_cast<uint32_t>(std::clamp(r.toNumber(arg(a, 2)), 0.0, 1e5));
        img.height = static_cast<uint32_t>(std::clamp(r.toNumber(arg(a, 3)), 0.0, 1e5));
        std::string bytes;
        if (!r.bufferBytes(arg(a, 1), bytes) || bytes.size() < static_cast<size_t>(img.width) * img.height * 4) return Value::undefined();
        img.rgba.assign(bytes.begin(), bytes.begin() + static_cast<ptrdiff_t>(static_cast<size_t>(img.width) * img.height * 4));
        s.pixels->writeRect(img, static_cast<int>(std::clamp(r.toNumber(arg(a, 4)), -1e8, 1e8)), static_cast<int>(std::clamp(r.toNumber(arg(a, 5)), -1e8, 1e8)));
        changed(s);
        return Value::undefined();
    });
    // drawCanvas(target, source, matrix (source pixels to target), alpha)
    rt.method(n, u"drawCanvas", 4, [self, canvasArg, changed, clipped](Runtime& r, const Value&, Args a) {
        CanvasSurface& s = self->surfaceOf(canvasArg(r, a));
        Node* src = self->node(arg(a, 1));
        if (!src || !src->isHtml("canvas")) return Value::undefined();
        const Raster image = self->surfaceOf(src).pixels->toRaster();
        const Matrix m = matrixOf(r, arg(a, 2));
        const float alpha = static_cast<float>(std::clamp(r.toNumber(arg(a, 3)), 0.0, 1.0));
        clipped(s, [&](Canvas& c) { c.drawRaster(image, m, alpha); });
        changed(s);
        return Value::undefined();
    });
    // text(canvas, text, font, x, y, align (-1 left, 0 centre, 1 right), baseline shift
    // (fraction: 0 alphabetic, 1 top, 0.5 middle, -1 bottom), matrix, style, alpha, maxWidth)
    rt.method(n, u"text", 11, [self, canvasArg, changed, clipped](Runtime& r, const Value&, Args a) {
        CanvasSurface& s = self->surfaceOf(canvasArg(r, a));
        if (!self->host.canvas_text) return Value::undefined();
        const std::string text = js::utf8(r.toStringValue(arg(a, 1)));
        const std::string font = js::utf8(r.toStringValue(arg(a, 2)));
        const Matrix m = matrixOf(r, arg(a, 7));
        const float scale = std::clamp(m.scale(), 0.05f, 16.0f);
        const auto got = self->host.canvas_text(text, font, scale, true);
        if (!got || got->mask.empty()) return Value::undefined();
        Paint paint;
        if (!paintOf(r, arg(a, 8), arg(a, 7), static_cast<float>(r.toNumber(arg(a, 9))), paint)) return Value::undefined();
        // A gradient's text is drawn in its first colour (as a mask takes one colour).
        Color color = paint.gradient && !paint.gradient->stops.empty() ? paint.gradient->stops.front().color : paint.color;
        const float x = static_cast<float>(r.toNumber(arg(a, 3))), y = static_cast<float>(r.toNumber(arg(a, 4)));
        const double align = r.toNumber(arg(a, 5)), shift = r.toNumber(arg(a, 6));
        float width = got->advance;
        float squeeze = 1;
        const double max_width = r.toNumber(arg(a, 10));
        if (std::isfinite(max_width) && max_width > 0 && width > max_width) squeeze = static_cast<float>(max_width) / width, width = static_cast<float>(max_width);
        const float left = x - (align < 0 ? 0 : align > 0 ? width : width / 2);
        // Where the baseline goes below the top of the text.
        const float drop = shift > 0 ? got->ascent * static_cast<float>(shift) : shift < 0 ? -got->descent : 0;
        const float top = y + drop - got->ascent;
        // The mask's pixels are `scale` to a px: back to px, squeezed, placed, transformed.
        const Matrix place{squeeze / scale, 0, 0, 1 / scale, left, top};
        const Matrix to_canvas = m.after(place);
        const float opacity = paint.opacity * color.a / 255.0f;
        color.a = 255;
        clipped(s, [&](Canvas& c) { c.drawRaster(got->mask, to_canvas, opacity, &color); });
        changed(s);
        return Value::undefined();
    });
    // measure(text, font): [advance, ascent, descent], or null without fonts.
    rt.method(n, u"measure", 2, [self](Runtime& r, const Value&, Args a) {
        if (!self->host.canvas_text) return Value::null();
        const auto got = self->host.canvas_text(js::utf8(r.toStringValue(arg(a, 0))), js::utf8(r.toStringValue(arg(a, 1))), 1, false);
        if (!got) return Value::null();
        return Value::object(r.newArray({Value::number(got->advance), Value::number(got->ascent), Value::number(got->descent)}));
    });
    // drawImage(canvas, url, matrix (image pixels to canvas), alpha): false if it isn't loaded.
    rt.method(n, u"drawImage", 4, [self, canvasArg, changed, clipped](Runtime& r, const Value&, Args a) {
        CanvasSurface& s = self->surfaceOf(canvasArg(r, a));
        if (!self->host.image_pixels) return Value::boolean(false);
        const std::shared_ptr<const Raster> image = self->host.image_pixels(js::utf8(r.toStringValue(arg(a, 1))));
        if (!image || image->empty()) return Value::boolean(false);
        const Matrix m = matrixOf(r, arg(a, 2));
        const float alpha = static_cast<float>(std::clamp(r.toNumber(arg(a, 3)), 0.0, 1.0));
        clipped(s, [&](Canvas& c) { c.drawRaster(*image, m, alpha); });
        changed(s);
        return Value::boolean(true);
    });
    // The size a loaded image's pixels are (0, 0 if it isn't).
    rt.method(n, u"imageSize", 1, [self](Runtime& r, const Value&, Args a) {
        std::shared_ptr<const Raster> image;
        if (self->host.image_pixels) image = self->host.image_pixels(js::utf8(r.toStringValue(arg(a, 0))));
        return Value::object(r.newArray({Value::number(image ? image->width : 0), Value::number(image ? image->height : 0)}));
    });
    rt.method(n, u"png", 1, [self, canvasArg](Runtime& r, const Value&, Args a) {
        const std::string png = encodePng(self->surfaceOf(canvasArg(r, a)).pixels->toRaster());
        return Value::object(r.newArrayBuffer(png));
    });
    rt.method(n, u"dataUrl", 1, [self, canvasArg](Runtime& r, const Value&, Args a) {
        const std::string png = encodePng(self->surfaceOf(canvasArg(r, a)).pixels->toRaster());
        return Value::string(js::utf16("data:image/png;base64," + base64(png)));
    });
    rt.put(rt.global(), u"__xwebCanvas2d", Value::object(n), js::kWritable | js::kConfigurable);
    // For live lists (childNodes, children, getElementsBy*): what they hold may have
    // changed once this has.
    rt.put(rt.global(), u"__xwebDomVersion", Value::object(rt.newFunction(u"domVersion", 0, [self](Runtime&, const Value&, Args) {
        return Value::number(static_cast<double>(self->dom_version));
    })), js::kWritable | js::kConfigurable);
}

std::shared_ptr<const Raster> PageScript::canvasImage(const Node* canvas, uint64_t& version) const {
    const auto it = impl_->canvases.find(canvas);
    if (it == impl_->canvases.end()) return nullptr;
    const Impl::CanvasSurface& s = *it->second;
    if (!s.shown || s.shown_version != s.version) {
        s.shown = std::make_shared<const Raster>(s.pixels->toRaster());
        s.shown_version = s.version;
    }
    version = s.version;
    return s.shown;
}

}  // namespace fbx::xweb
