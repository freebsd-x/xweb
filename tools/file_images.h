#pragma once

// Images from files next to a page, or data: URLs: bitmaps through xrender's PNG decoder,
// SVG through our own. For the command-line tools, which have no network.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>
#include <utility>

#include "xrender/image_decoder.h"
#include "xweb/layout.h"
#include "xweb/paint.h"
#include "xweb/url.h"

namespace fbx::xweb::tools {

struct Loaded {
    Raster bitmap;
    std::shared_ptr<const SvgImage> svg;
};

class FileImages : public ImageSizes {
public:
    explicit FileImages(std::filesystem::path base) : base_(std::move(base)) {}

    const Loaded* get(const std::string& src) {
        const auto it = cache_.find(src);
        if (it != cache_.end()) return it->second ? &*it->second : nullptr;
        std::optional<Loaded> got = load(src);
        auto& slot = cache_[src];
        slot = std::move(got);
        return slot ? &*slot : nullptr;
    }

    std::optional<std::pair<float, float>> imageSize(const std::string& src) override {
        const Loaded* l = get(src);
        if (!l) return std::nullopt;
        if (l->svg) {
            const SvgSize& s = l->svg->size();
            if (s.width && s.height) return std::make_pair(*s.width, *s.height);
            if (s.width && s.ratio > 0) return std::make_pair(*s.width, *s.width / s.ratio);
            if (s.height && s.ratio > 0) return std::make_pair(*s.height * s.ratio, *s.height);
            if (s.ratio > 0) return std::nullopt;
            return std::make_pair(300.0f, 150.0f);
        }
        return std::make_pair(static_cast<float>(l->bitmap.width), static_cast<float>(l->bitmap.height));
    }

    std::optional<float> imageRatio(const std::string& src) override {
        const Loaded* l = get(src);
        if (!l || !l->svg || l->svg->size().width || l->svg->size().height || l->svg->size().ratio <= 0) return std::nullopt;
        return l->svg->size().ratio;
    }

private:
    std::optional<Loaded> load(const std::string& src) {
        std::string body, type;
        if (!decodeDataUrl(src, body, type)) {
            const std::string path = src.starts_with("file://") ? src.substr(7) : (base_ / src.substr(0, src.find_first_of("?#"))).string();
            std::ifstream f(path, std::ios::binary);
            if (!f) return std::nullopt;
            body.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
        Loaded out;
        if (SvgImage::sniff(body, type, src)) {
            out.svg = SvgImage::parse(body);
            if (!out.svg) return std::nullopt;
            return out;
        }
        char name[] = "/tmp/xweb_paint_XXXXXX";
        const int fd = mkstemp(name);
        if (fd < 0) return std::nullopt;
        (void)!write(fd, body.data(), body.size());
        close(fd);
        auto decoded = fbx::xrender::ImageDecoder::decodePng(name);
        unlink(name);
        if (!decoded) return std::nullopt;
        out.bitmap.width = decoded->width_;
        out.bitmap.height = decoded->height_;
        out.bitmap.rgba = std::move(decoded->pixels_);
        return out;
    }

    std::filesystem::path base_;
    std::map<std::string, std::optional<Loaded>> cache_;
};

}  // namespace fbx::xweb::tools
