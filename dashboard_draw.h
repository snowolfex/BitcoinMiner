#pragma once

#include <gdiplus.h>
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <deque>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

namespace dashboard {
using namespace Gdiplus;

struct Runtime {
    ULONG_PTR token = 0;
    Runtime() {
        GdiplusStartupInput input;
        if (GdiplusStartup(&token, &input, nullptr) != Ok)
            throw std::runtime_error("Windows could not initialize dashboard drawing");
    }
    ~Runtime() { GdiplusShutdown(token); }
};

inline Color color(COLORREF c, BYTE alpha = 255) {
    return Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
}
inline RectF rect(const RECT& r) {
    return RectF(static_cast<REAL>(r.left), static_cast<REAL>(r.top),
        static_cast<REAL>(r.right - r.left), static_cast<REAL>(r.bottom - r.top));
}
inline void rounded(GraphicsPath& path, RectF r, REAL radius) {
    REAL d = std::min(radius * 2, std::min(r.Width, r.Height));
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    path.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    path.CloseFigure();
}
inline void panel(Graphics& g, RectF r, REAL radius, Color top, Color bottom, Color edge) {
    GraphicsPath path;
    rounded(path, r, radius);
    LinearGradientBrush fill(PointF(r.X, r.Y), PointF(r.X, r.GetBottom()), top, bottom);
    Pen border(edge, 1);
    g.FillPath(&fill, &path);
    g.DrawPath(&border, &path);
}
inline void text(Graphics& g, HDC dc, HFONT font, const std::wstring& value,
                 RectF r, Color ink, bool centered = true) {
    Font face(dc, font);
    SolidBrush brush(ink);
    StringFormat format;
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    format.SetAlignment(centered ? StringAlignmentCenter : StringAlignmentNear);
    format.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(value.c_str(), static_cast<INT>(value.size()), &face, r, &format, &brush);
}
inline void gauge(Graphics& g, HDC dc, HFONT font, RECT bounds, COLORREF accent,
                  double value, bool valid, const wchar_t* caption, int dpi) {
    RectF r = rect(bounds);
    REAL scale = dpi / 96.0f;
    REAL size = std::min(r.Width, r.Height);
    r.X += (r.Width - size) / 2;
    r.Width = r.Height = size;
    r.Inflate(-5 * scale, -5 * scale);
    SolidBrush center(Color(255, 13, 21, 37));
    g.FillEllipse(&center, r);
    Pen track(Color(255, 43, 55, 81), 7 * scale);
    Pen light(color(accent), 7 * scale);
    light.SetStartCap(LineCapRound);
    light.SetEndCap(LineCapRound);
    g.DrawEllipse(&track, r);
    if (valid && value > 0) g.DrawArc(&light, r, -90.0f, static_cast<REAL>(3.6 * std::min(100.0, value)));
    std::wstring number = valid ? std::to_wstring(static_cast<int>(std::round(value))) + L"%" : L"--";
    text(g, dc, font, number, RectF(r.X, r.Y + size * .2f, size, size * .35f), Color(255, 238, 241, 255));
    text(g, dc, font, caption, RectF(r.X, r.Y + size * .52f, size, size * .25f), color(accent));
}
inline void hardware(Graphics& g, RECT bounds, COLORREF accent, bool gpu) {
    RectF r = rect(bounds);
    REAL scale = r.Height / 70;
    Color ink = color(accent), border(255, 65, 81, 113);
    Pen line(ink, std::max(1.0f, scale));
    if (!gpu) {
        REAL size = r.Height * .76f;
        RectF chip(r.X + 12 * scale, r.Y + 6 * scale, size, size);
        for (int i = 1; i < 7; ++i) {
            REAL offset = size * i / 7;
            g.DrawLine(&line, chip.X - 5 * scale, chip.Y + offset, chip.X, chip.Y + offset);
            g.DrawLine(&line, chip.GetRight(), chip.Y + offset, chip.GetRight() + 5 * scale, chip.Y + offset);
            g.DrawLine(&line, chip.X + offset, chip.Y - 5 * scale, chip.X + offset, chip.Y);
            g.DrawLine(&line, chip.X + offset, chip.GetBottom(), chip.X + offset, chip.GetBottom() + 5 * scale);
        }
        panel(g, chip, 5 * scale, Color(255, 61, 58, 105), Color(255, 24, 31, 51), ink);
        chip.Inflate(-10 * scale, -10 * scale);
        panel(g, chip, 3 * scale, Color(255, 100, 103, 140), Color(255, 48, 57, 84), border);
    } else {
        RectF card(r.X + 4 * scale, r.Y + 8 * scale, std::min(r.Width - 8 * scale, 160 * scale), r.Height * .72f);
        panel(g, card, 6 * scale, Color(255, 37, 61, 72), Color(255, 20, 33, 48), ink);
        REAL diameter = card.Height * .72f;
        for (int i = 0; i < 2; ++i) {
            REAL cx = card.X + card.Width * (i == 0 ? .27f : .73f);
            REAL cy = card.Y + card.Height / 2;
            g.DrawEllipse(&line, cx - diameter / 2, cy - diameter / 2, diameter, diameter);
            for (int blade = 0; blade < 7; ++blade) {
                double angle = blade * 6.283185307 / 7;
                g.DrawLine(&line, cx, cy,
                    cx + static_cast<REAL>(std::cos(angle) * diameter * .4),
                    cy + static_cast<REAL>(std::sin(angle) * diameter * .4));
            }
        }
    }
}
inline void graph(Graphics& g, RECT bounds, const std::deque<double>& samples, COLORREF accent) {
    RectF r = rect(bounds);
    Pen grid(Color(255, 36, 48, 69), 1);
    for (int i = 1; i <= 3; ++i) {
        REAL y = r.Y + r.Height * i / 4;
        g.DrawLine(&grid, r.X, y, r.GetRight(), y);
    }
    if (samples.size() < 2) return;
    double peak = std::max(1.0, *std::max_element(samples.begin(), samples.end()));
    std::vector<PointF> points;
    for (size_t i = 0; i < samples.size(); ++i)
        points.emplace_back(r.X + r.Width * static_cast<REAL>(i) / 59,
            r.GetBottom() - static_cast<REAL>(samples[i] / peak) * (r.Height - 2));
    Pen trace(color(accent), 2);
    g.DrawLines(&trace, points.data(), static_cast<INT>(points.size()));
}
}
