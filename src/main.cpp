#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using namespace geode::prelude;

// ================= SETTINGS YOU CAN TWEAK =================
const float LOOK_SECONDS  = 2.4f;   // seconds ahead the bars show at 1x (bigger = bars travel slower)
const float MARGIN        = 10.f;   // distance from the screen corner
const float CX            = 66.f;   // center of the highway
const float LANE_W        = 90.f;   // width of the highway at the bottom
const float FAR_SCALE     = 0.50f;  // how narrow it gets at the top (smaller = more perspective)
const float PERSP_K       = 0.3f;   // how much bars speed up near the line (0 = perfectly even)
const float HIT_Y         = 30.f;   // height of the hit bar
const float TOP_Y         = 205.f;  // height of the top of the highway
const float NOTE_FRAC     = 0.70f;  // bar width compared to the lane
const float MIN_H         = 7.f;    // shortest a bar can be
const float GAP           = 7.f;    // empty space between back-to-back bars
const float GRID_UNITS    = 80.f;   // spacing of the faint rungs
const float GREEN_SEC     = 0.05f;  // how close to the line a bar must be to turn green

const float PERFECT_MS    = 20.f;   // click within this = PERFECT
const float GOOD_MS       = 60.f;   // click within this = GOOD (later = MISS)

const float DEATH_MARGIN  = 100.f;  // clicks this close to where you died are not saved
const float CONNECT_SLACK = 150.f;  // attempt must start near saved part to add to it
const float REPAIR_UNITS  = 190.f;  // how much chart is forgotten when it killed you
const float REPAIR_SLACK  = 20.f;

// ================= COLORS =================
const ccColor4F COL_LANE   = {0.02f, 0.06f, 0.05f, 0.72f};
const ccColor4F COL_RAIL   = {0.30f, 1.f, 0.55f, 0.95f};
const ccColor4F COL_GRID   = {0.30f, 1.f, 0.55f, 0.16f};
const ccColor4F COL_NOTE   = {0.30f, 0.80f, 1.f, 1.f};    // cyan
const ccColor4F COL_ACTIVE = {0.35f, 1.f, 0.55f, 1.f};    // green
const ccColor4F COL_GOLD   = {1.f, 0.78f, 0.25f, 1.f};
const ccColor3B C_PERFECT = {255, 214, 92};
const ccColor3B C_GOOD    = {110, 255, 150};
const ccColor3B C_MISS    = {255, 90, 90};

// menu colors
const ccColor4F UI_BLACK = {0.f, 0.f, 0.f, 1.f};
const ccColor4F UI_EDGE  = {0.50f, 0.50f, 0.55f, 1.f};
const ccColor4F UI_FILL  = {0.09f, 0.09f, 0.11f, 1.f};
const ccColor4F UI_TEXT  = {1.f, 1.f, 1.f, 1.f};
const ccColor4F UI_DIM   = {0.60f, 0.60f, 0.66f, 1.f};
const ccColor4F UI_GREEN = {0.35f, 1.f, 0.55f, 1.f};
const ccColor4F UI_RED   = {1.f, 0.38f, 0.38f, 1.f};

// ================= DATA =================
struct Note {
    float start; // where you pressed
    float end;   // where you let go (-1 while still held)
};

struct Chart {
    std::vector<Note> notes;
    float reach = 0.f; // how far into the level the chart is known
};

static std::map<int, Chart> g_charts;   // one chart per level ID

static std::vector<Note>  g_attempt;    // clicks of the current attempt
static std::vector<float> g_presses;    // presses waiting to be judged
// per saved note: 0 waiting, 1 hit PERFECT, 2 skipped, 3 missed, 4 hit GOOD
static std::vector<char>  g_state;
static int   g_attemptId     = 0;
static float g_attemptStartX = 0.f;
static float g_attemptMaxX   = 0.f;
static bool  g_attemptActive = false;
static PlayLayer* g_layer    = nullptr;

static float g_lastX     = 0.f;
static float g_vel       = 311.58f; // how fast the player moves (units per second), smoothed
static bool  g_holding   = false;
static float g_speed     = 1.f;
static bool  g_popupOpen = false;
static bool  g_enabled   = true;    // highway on/off

static ccColor4F withAlpha(ccColor4F c, float a) {
    c.a = a;
    return c;
}

// Filled four-sided shape (bottom-left, bottom-right, top-right, top-left)
static void quad(CCDrawNode* n, CCPoint a, CCPoint b, CCPoint c, CCPoint d,
                 ccColor4F fill, float borderW = 0.f,
                 ccColor4F border = {0.f, 0.f, 0.f, 0.f}) {
    CCPoint pts[4] = {a, b, c, d};
    n->drawPolygon(pts, 4, fill, borderW, border);
}

// ================= BLOCKY PIXEL FONT (used by the menu) =================
struct Glyph {
    char c;
    const char* r[7];
};

static const Glyph GLYPHS[] = {
    {'A', {"01110","10001","10001","11111","10001","10001","10001"}},
    {'B', {"11110","10001","10001","11110","10001","10001","11110"}},
    {'C', {"01110","10001","10000","10000","10000","10001","01110"}},
    {'D', {"11110","10001","10001","10001","10001","10001","11110"}},
    {'E', {"11111","10000","10000","11110","10000","10000","11111"}},
    {'F', {"11111","10000","10000","11110","10000","10000","10000"}},
    {'G', {"01110","10001","10000","10111","10001","10001","01111"}},
    {'H', {"10001","10001","10001","11111","10001","10001","10001"}},
    {'I', {"01110","00100","00100","00100","00100","00100","01110"}},
    {'J', {"00111","00010","00010","00010","00010","10010","01100"}},
    {'K', {"10001","10010","10100","11000","10100","10010","10001"}},
    {'L', {"10000","10000","10000","10000","10000","10000","11111"}},
    {'M', {"10001","11011","10101","10101","10001","10001","10001"}},
    {'N', {"10001","11001","10101","10011","10001","10001","10001"}},
    {'O', {"01110","10001","10001","10001","10001","10001","01110"}},
    {'P', {"11110","10001","10001","11110","10000","10000","10000"}},
    {'Q', {"01110","10001","10001","10001","10101","10010","01101"}},
    {'R', {"11110","10001","10001","11110","10100","10010","10001"}},
    {'S', {"01111","10000","10000","01110","00001","00001","11110"}},
    {'T', {"11111","00100","00100","00100","00100","00100","00100"}},
    {'U', {"10001","10001","10001","10001","10001","10001","01110"}},
    {'V', {"10001","10001","10001","10001","10001","01010","00100"}},
    {'W', {"10001","10001","10001","10101","10101","11011","10001"}},
    {'X', {"10001","10001","01010","00100","01010","10001","10001"}},
    {'Y', {"10001","10001","01010","00100","00100","00100","00100"}},
    {'Z', {"11111","00001","00010","00100","01000","10000","11111"}},
    {'0', {"01110","10001","10011","10101","11001","10001","01110"}},
    {'1', {"00100","01100","00100","00100","00100","00100","01110"}},
    {'2', {"01110","10001","00001","00010","00100","01000","11111"}},
    {'3', {"11110","00001","00001","01110","00001","00001","11110"}},
    {'4', {"00010","00110","01010","10010","11111","00010","00010"}},
    {'5', {"11111","10000","11110","00001","00001","10001","01110"}},
    {'6', {"00110","01000","10000","11110","10001","10001","01110"}},
    {'7', {"11111","00001","00010","00100","01000","01000","01000"}},
    {'8', {"01110","10001","10001","01110","10001","10001","01110"}},
    {'9', {"01110","10001","10001","01111","00001","00010","01100"}},
    {'.', {"00000","00000","00000","00000","00000","01100","01100"}},
    {':', {"00000","01100","01100","00000","01100","01100","00000"}},
    {'-', {"00000","00000","00000","11111","00000","00000","00000"}},
};

static const Glyph* findGlyph(char c) {
    for (auto const& g : GLYPHS) {
        if (g.c == c) return &g;
    }
    return nullptr; // spaces and unknown characters
}

static float pixelTextWidth(std::string const& t, float px) {
    return t.empty() ? 0.f : (t.size() * 6.f - 1.f) * px;
}

// y is the vertical center of the text
static void pixelText(CCDrawNode* n, std::string const& text, float x, float y, float px,
                      ccColor4F col, bool center) {
    float startX = center ? x - pixelTextWidth(text, px) / 2.f : x;
    float baseY = y - 3.5f * px;
    for (size_t i = 0; i < text.size(); i++) {
        auto g = findGlyph(text[i]);
        if (!g) continue;
        float gx = startX + i * 6.f * px;
        for (int r = 0; r < 7; r++) {
            for (int c = 0; c < 5; c++) {
                if (g->r[r][c] != '1') continue;
                float x0 = gx + c * px;
                float y0 = baseY + (6 - r) * px;
                quad(n, {x0, y0}, {x0 + px, y0}, {x0 + px, y0 + px}, {x0, y0 + px}, col);
            }
        }
    }
}

// A square button made of blocks, drawn around (0, 0)
static void paintBlockButton(CCDrawNode* d, std::string const& text, float w, float h, float px,
                             ccColor4F edge, ccColor4F fill, ccColor4F txt) {
    d->clear();
    float hw = w / 2.f, hh = h / 2.f;
    quad(d, {-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}, edge);
    quad(d, {-hw + 2.f, -hh + 2.f}, {hw - 2.f, -hh + 2.f}, {hw - 2.f, hh - 2.f}, {-hw + 2.f, hh - 2.f}, fill);
    pixelText(d, text, 0.f, 0.f, px, txt, true);
}

static CCNode* newBlockRoot(float w, float h, CCDrawNode*& d) {
    auto root = CCNode::create();
    root->setContentSize({w, h});
    root->setAnchorPoint({0.5f, 0.5f});
    d = CCDrawNode::create();
    d->setPosition({w
