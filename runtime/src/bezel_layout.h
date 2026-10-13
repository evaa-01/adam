#pragma once
// Bezel layout math (bezels.cpp), kept free of UI code so it can be tested on its own (scripts\test_bezel_layout.cpp).
//
// The picture is repeated endlessly in both directions (tiles). At tile = 100 one tile is scaled to cover the window
// (scale = max(w / picture width, h / picture height)); a smaller tile value shrinks the tiles, so more of them show.
// pos_x / pos_y (0-100) slide the tiling: 50 / 50 = one tile centered on the window; 0 and 100 are one tile apart, so
// they look the same and moving loops without a seam.
// A bar panel covering window columns [x, x + bar) and all rows shows the tiling there as a grid of segments: one per
// (column piece, row piece) between tile joints, each an image with its own source rectangle.
#include <algorithm>
#include <cmath>
#include <vector>

namespace adam::bezels::layout {
    struct Segment {
        double src_x, src_y, src_w, src_h;   // source rectangle (picture pixels)
        double left, top, width, height;     // position inside the panel, as fractions of the panel's width / height
    };

    struct Panel {
        std::vector<Segment> segments;
    };

    // Pieces along one axis: [start, start + length) in window pixels, the tiling's origin there, the tile size in window
    // pixels and in picture pixels. Each piece: {source offset, source length, fraction offset, fraction length}.
    struct Piece {
        double src, src_len, frac, frac_len;
    };

    constexpr size_t max_pieces = 24; // per axis (tile 10% in a tall window: ~11 rows)

    inline std::vector<Piece> pieces(double start, double length, double origin, double scale, double picture_len) {
        std::vector<Piece> out;
        double u = std::fmod((start - origin) / scale, picture_len); // picture coordinate at the start
        if (u < 0) {
            u += picture_len;
        }
        const double total = length / scale;
        double remaining = total, done = 0;
        while (remaining > 1e-6 && out.size() < max_pieces) {
            double take = std::min(picture_len - u, remaining);
            if (take > 1e-6) {
                out.push_back(Piece{ u, take, done / total, take / total });
            }
            done += take;
            remaining -= take;
            u = 0;
        }
        return out;
    }

    inline double tile_scale(double w, double h, double picture_w, double picture_h, double tile) {
        return std::max(w / picture_w, h / picture_h) * std::clamp(tile, 1.0, 100.0) / 100.0;
    }

    inline Panel place(double x, double bar, double w, double h, double picture_w, double picture_h, double pos_x, double pos_y,
        double tile) {
        Panel out;
        if (bar <= 0 || w <= 0 || h <= 0 || picture_w <= 0 || picture_h <= 0) {
            return out;
        }
        double scale = tile_scale(w, h, picture_w, picture_h, tile); // window pixels per picture pixel
        double tile_w = picture_w * scale, tile_h = picture_h * scale;
        double origin_x = (w - tile_w) / 2.0 + (pos_x - 50.0) / 100.0 * tile_w;
        double origin_y = (h - tile_h) / 2.0 + (pos_y - 50.0) / 100.0 * tile_h;
        std::vector<Piece> cols = pieces(x, bar, origin_x, scale, picture_w);
        std::vector<Piece> rows = pieces(0, h, origin_y, scale, picture_h);
        for (const Piece& r : rows) {
            for (const Piece& c : cols) {
                out.segments.push_back(Segment{ c.src, r.src, c.src_len, r.src_len, c.frac, r.frac, c.frac_len, r.frac_len });
            }
        }
        return out;
    }
}
