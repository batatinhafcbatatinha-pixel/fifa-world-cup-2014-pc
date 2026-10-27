// 2014 FIFA World Cup Brazil look for the PC settings, after the game's own front end: a pale
// wash over the paused frame, a yellow brush-stroke panel, navy Futura Std text, Bauhaus Std
// headings (as the home screen's tiles), the selection as a rounded navy bar with white text
// (as the game's dialog buttons) and the green A / B button discs. The fonts are read at run
// time from the user's data4.big (pc_font.h).
#include "pc_font.h"
#include "pc_settings.h"

#include <rex/logging.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <string>
#include <vector>

namespace {

constexpr int kEm = 48;
constexpr uint32_t kNavy = 0xFF0B1E7Eu, kGreen = 0xFF2E8B2Eu, kWhite = 0xFFFFFFFFu;
constexpr uint32_t kYellow = 0xF2FFE45Cu, kMuted = 0xFF7D86A8u;

float Scale(int em) { return float(em) / float(kEm); }

void LoadTheme(pcs::Theme& t) {
  try {
    auto root = pcfont::GameRoot("D:/Copa2014-game");
    pcfont::LoadTtf(root, "data/ui/fonts/futurastd-book.ttf", kEm, t.font);
    pcfont::LoadTtf(root, "data/ui/fonts/bauhausstd-medium.ttf", kEm, t.title_font);
    pcfont::LoadTtf(root, "data/ui/fonts/futurastd-bold.ttf", kEm, t.extra_font);
  } catch (const std::exception& e) {
    t.font = {};
    REXLOG_WARN("[pc] World Cup menu fonts: {} (the menu keeps its own look)", e.what());
  }
}

// A rounded rectangle as one convex polygon.
void Rounded(pcs::Canvas& c, int x, int y, int w, int h, int r, uint32_t argb) {
  // Clockwise on screen (y down): top right, bottom right, bottom left, top left corners.
  std::vector<int> xy;
  const int cx[4] = {x + w - r, x + w - r, x + r, x + r}, cy[4] = {y + r, y + h - r, y + h - r, y + r};
  for (int corner = 0; corner < 4; ++corner)
    for (int k = 0; k <= 4; ++k) {
      double a = (corner * 90 - 90 + k * 22.5) * 3.14159265 / 180;
      xy.push_back(cx[corner] + int(std::lround(r * std::cos(a))));
      xy.push_back(cy[corner] + int(std::lround(r * std::sin(a))));
    }
  c.Polygon(xy.data(), int(xy.size() / 2), argb);
}

void Disc(pcs::Canvas& c, int cx, int cy, int r, uint32_t argb) {
  int xy[32];
  for (int k = 0; k < 16; ++k) {
    xy[2 * k] = cx + int(std::lround(r * std::cos(k * 3.14159265 / 8)));
    xy[2 * k + 1] = cy + int(std::lround(r * std::sin(k * 3.14159265 / 8)));
  }
  c.Polygon(xy, 16, argb);
}

// The game's brush-stroke panels: a rectangle whose edges wander a little (fixed pattern).
void Brush(pcs::Canvas& c, int x, int y, int w, int h, uint32_t argb) {
  static const int jitter[] = {0, 6, -4, 9, 2, -7, 5, -2, 8, -5, 3, 7, -6, 1, 9, -3};
  c.Fill(x, y, w, h, argb);
  for (int i = 0; i * 18 < w; ++i) {
    int j = jitter[i % 16], k = jitter[(i + 5) % 16];
    c.Fill(x + i * 18, y - 6 - j / 2, 18, 6 + j / 2, argb);
    c.Fill(x + i * 18, y + h, 18, 6 + k / 2, argb);
  }
  for (int i = 0; i * 18 < h; ++i) {
    int j = jitter[(i + 3) % 16], k = jitter[(i + 9) % 16];
    c.Fill(x - 8 - j / 2, y + i * 18, 8 + j / 2, 18, argb);
    c.Fill(x + w, y + i * 18, 8 + k / 2, 18, argb);
  }
}

struct Line {
  const pcs::MenuRow* row;
  const std::wstring* heading;
};

void Draw(const pcs::Theme& t, pcs::Canvas& c, pcs::MenuView& v) {
  const pcs::ThemeFont& book = t.font;
  const pcs::ThemeFont& head = t.title_font.glyphs.empty() ? book : t.title_font;
  const pcs::ThemeFont& bold = t.extra_font.glyphs.empty() ? book : t.extra_font;
  auto text_y = [&](const pcs::ThemeFont& f, int em, int top, int height) {
    return top + (height - int(f.line_height * Scale(em))) / 2;
  };

  c.Fill(0, 0, 1280, 720, 0xE8FFFBEAu);

  // Title as the home screen's selected tab: navy Bauhaus with a rule under it.
  c.Text(book, v.subtitle, 116, 46, 900, Scale(17), kGreen, 0, true);
  int tw = c.Text(head, v.title, 116, 66, 900, Scale(40), kNavy, 0, true);
  c.Fill(116, 112, tw, 2, kNavy);

  std::vector<Line> lines;
  int selected_line = 0;
  for (const pcs::MenuRow& r : v.rows) {
    if (!r.section.empty()) lines.push_back({nullptr, &r.section});
    if (r.selected) selected_line = int(lines.size());
    lines.push_back({&r, nullptr});
  }
  constexpr int kTop = 160, kRow = 33, kVisible = 13;
  static int first = 0;
  if (selected_line < first + 1) first = std::max(0, selected_line - 1);
  if (selected_line > first + kVisible - 2) first = selected_line - kVisible + 2;
  first = std::clamp(first, 0, std::max(0, int(lines.size()) - kVisible));
  const int shown = std::min(kVisible, int(lines.size()) - first);
  const int x0 = 116, x1 = 790;
  Brush(c, x0 - 16, kTop - 12, x1 - x0 + 32, shown * kRow + 24, kYellow);
  for (int i = 0; i < shown; ++i) {
    const Line& line = lines[first + i];
    const int y = kTop + i * kRow;
    if (line.heading) {
      c.Text(head, *line.heading, x0 + 6, text_y(head, 20, y, kRow), 400, Scale(20), kGreen, 0, true);
      continue;
    }
    const pcs::MenuRow& r = *line.row;
    std::wstring value = r.value + (r.restart ? L" *" : L"");
    uint32_t ink = r.greyed ? kMuted : kNavy;
    if (r.selected) {
      Rounded(c, x0 - 4, y + 2, x1 - x0 + 8, kRow - 4, 9, kNavy);
      ink = kWhite;
      if (!r.value.empty()) {
        int vw = c.Measure(book, value, Scale(21), false);
        int right = x1 - 14;
        const int arrow_r[] = {right - 8, y + 10, right - 8, y + kRow - 10, right, y + kRow / 2};
        c.Polygon(arrow_r, 3, kWhite);
        int lx = right - 24 - vw - 16;
        const int arrow_l[] = {lx + 8, y + 10, lx + 8, y + kRow - 10, lx, y + kRow / 2};
        c.Polygon(arrow_l, 3, kWhite);
        v.hits.push_back({r.id, lx - 6, y, lx + 18, y + kRow, true});
        c.Text(book, value, right - 18 - vw, text_y(book, 21, y, kRow), vw + 2, Scale(21), kWhite, 0, false);
      }
    } else if (!r.value.empty()) {
      c.Text(book, value, x0 + 330, text_y(book, 20, y, kRow), x1 - x0 - 340, Scale(20), ink, 1, false);
    }
    c.Text(r.selected ? bold : book, r.label, x0 + 8, text_y(book, 21, y, kRow), 330, Scale(21), ink, 0, false);
    v.hits.push_back({r.id, x0, y, x1, y + kRow, false});
  }
  if (first > 0) {
    const int up[] = {x1 - 30, kTop - 2, x1 - 14, kTop - 2, x1 - 22, kTop - 12};
    c.Polygon(up, 3, kNavy);
  }
  if (first + shown < int(lines.size())) {
    const int b = kTop + shown * kRow;
    const int down[] = {x1 - 30, b + 2, x1 - 14, b + 2, x1 - 22, b + 12};
    c.Polygon(down, 3, kNavy);
  }

  // Help beside the panel: Bauhaus heading, Futura text.
  if (!v.help_title.empty())
    c.Text(head, v.help_title, 850, 172, 380, Scale(26), kNavy, 0, true);
  if (!v.help.empty()) {
    int y = 214;
    size_t p = 0;
    while (p < v.help.size() && y < 560) {
      size_t fit = p, end = p;
      while (true) {
        size_t space = v.help.find(L' ', end);
        if (space == std::wstring::npos) space = v.help.size();
        if (c.Measure(book, v.help.substr(p, space - p), Scale(20), false) > 360 && fit > p) break;
        fit = space;
        end = space + 1;
        if (space >= v.help.size()) break;
      }
      if (fit == p) fit = std::min(v.help.size(), p + 1);
      c.Text(book, v.help.substr(p, fit - p), 850, y, 380, Scale(20), kNavy, 0, false);
      p = fit + 1;
      y += 26;
    }
  }
  if (!v.restart_note.empty()) c.Text(bold, v.restart_note, 850, 570, 380, Scale(17), kGreen, 0, true);

  // Bottom left, as the home screen: green button discs and their captions.
  int hx = 240;
  auto hint = [&](wchar_t letter, const std::wstring& caption, int id) {
    Disc(c, hx + 12, 664, 12, 0xFF3DA33Du);
    c.Text(bold, std::wstring(1, letter), hx, text_y(bold, 17, 652, 24), 24, Scale(17), kWhite, 2, false);
    int w = c.Text(book, caption, hx + 30, text_y(book, 19, 652, 24), 300, Scale(19), kNavy, 0, true);
    v.hits.push_back({id, hx, 650, hx + 34 + w, 678, false});
    hx += 34 + w + 30;
  };
  hint(L'A', v.select, -1);
  hint(L'B', v.back, -2);
  c.Text(book, v.keys, hx + 10, text_y(book, 15, 652, 24), 1180 - hx, Scale(15), kMuted, 0, false);
}

}  // namespace

const pcs::Theme* PcCopaTheme() {
  static pcs::Theme theme = [] {
    pcs::Theme t;
    t.load = LoadTheme;
    t.draw = Draw;
    return t;
  }();
  return &theme;
}
