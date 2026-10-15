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
    if (t.empty()) return 0.f;
    return (static_cast<float>(t.size()) * 6.f - 1.f) * px;
}

// y is the vertical center of the text
static void pixelText(CCDrawNode* n, std::string const& text, float x, float y, float px,
                      ccColor4F col, bool center) {
    float startX = x;
    if (center) startX = x - pixelTextWidth(text, px) * 0.5f;
    float baseY = y - 3.5f * px;
    for (size_t i = 0; i < text.size(); i++) {
        auto g = findGlyph(text[i]);
        if (!g) continue;
        float gx = startX + static_cast<float>(i) * 6.f * px;
        for (int r = 0; r < 7; r++) {
            for (int c = 0; c < 5; c++) {
                if (g->r[r][c] != '1') continue;
                float x0 = gx + static_cast<float>(c) * px;
                float y0 = baseY + static_cast<float>(6 - r) * px;
                quad(n, CCPoint(x0, y0), CCPoint(x0 + px, y0),
                        CCPoint(x0 + px, y0 + px), CCPoint(x0, y0 + px), col);
            }
        }
    }
}

// A square button made of blocks, drawn around (0, 0)
static void paintBlockButton(CCDrawNode* d, std::string const& text, float w, float h, float px,
                             ccColor4F edge, ccColor4F fill, ccColor4F txt) {
    d->clear();
    float hw = w * 0.5f;
    float hh = h * 0.5f;
    quad(d, CCPoint(-hw, -hh), CCPoint(hw, -hh), CCPoint(hw, hh), CCPoint(-hw, hh), edge);
    quad(d, CCPoint(-hw + 2.f, -hh + 2.f), CCPoint(hw - 2.f, -hh + 2.f),
            CCPoint(hw - 2.f, hh - 2.f), CCPoint(-hw + 2.f, hh - 2.f), fill);
    pixelText(d, text, 0.f, 0.f, px, txt, true);
}

static CCNode* newBlockRoot(float w, float h, CCDrawNode*& d) {
    auto root = CCNode::create();
    root->setContentSize(CCSize(w, h));
    root->setAnchorPoint(CCPoint(0.5f, 0.5f));
    d = CCDrawNode::create();
    float halfW = w * 0.5f;
    float halfH = h * 0.5f;
    d->setPosition(CCPoint(halfW, halfH));
    root->addChild(d);
    return root;
}

// ================= SAVING CLICKS =================
// Save this attempt's new clicks into the level's chart
static void commitAttempt(bool finished, float finishX) {
    if (!g_attemptActive) return;
    g_attemptActive = false;

    auto& chart = g_charts[g_attemptId];

    // You hit every click near your death PERFECT and still died inside the saved part:
    // the chart is wrong there, so forget that stretch and record it again.
    if (!finished && g_enabled && g_state.size() == chart.notes.size()
        && g_attemptMaxX <= chart.reach + REPAIR_SLACK) {
        float from = g_attemptMaxX - REPAIR_UNITS;
        int count = 0;
        bool allPerfect = true;
        for (size_t i = 0; i < chart.notes.size(); i++) {
            float s = chart.notes[i].start;
            if (s < from || s > g_attemptMaxX) continue;
            count++;
            if (g_state[i] != 1) allPerfect = false;
        }
        if (count > 0 && allPerfect) {
            chart.notes.erase(
                std::remove_if(chart.notes.begin(), chart.notes.end(),
                               [&](Note const& n) { return n.start >= from; }),
                chart.notes.end());
            chart.reach = std::max(from, 0.f);
            g_attempt.clear();
            return;
        }
    }

    float cutoff = finished ? finishX + 1.f : g_attemptMaxX - DEATH_MARGIN;

    if (g_attemptStartX <= chart.reach + CONNECT_SLACK && cutoff > chart.reach) {
        for (auto n : g_attempt) {
            if (n.start <= chart.reach || n.start > cutoff) continue;
            if (n.end < n.start) n.end = cutoff; // still held
            if (n.end > cutoff) n.end = cutoff;
            chart.notes.push_back(n);
        }
        chart.reach = cutoff;
    }
    g_attempt.clear();
}

// Start tracking a fresh attempt
static void startAttempt(PlayLayer* pl) {
    g_layer = pl;
    g_attempt.clear();
    g_presses.clear();
    g_holding = false;
    g_attemptId = pl->m_level->m_levelID.value();
    g_attemptStartX = pl->m_player1 ? pl->m_player1->getPositionX() : 0.f;
    g_attemptMaxX = g_attemptStartX;
    g_attemptActive = true;
    g_lastX = g_attemptStartX;
    g_vel = 311.58f;

    auto& notes = g_charts[g_attemptId].notes;
    g_state.assign(notes.size(), 0);
    for (size_t i = 0; i < notes.size(); i++) {
        // notes behind where we spawned (practice checkpoints) don't count
        if (notes[i].start < g_attemptStartX) g_state[i] = 2;
    }
}

// ================= PART 1: record presses and releases =================
class $modify(RecordLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (!isPlayer1 || !m_player1 || !PlayLayer::get()) return;

        float x = m_player1->getPositionX();
        if (down) {
            g_holding = true;
            g_presses.push_back(x);
            g_attempt.push_back({x, -1.f});
        } else {
            g_holding = false;
            for (auto it = g_attempt.rbegin(); it != g_attempt.rend(); ++it) {
                if (it->end < 0.f) {
                    it->end = x;
                    break;
                }
            }
        }
    }
};

// ================= PART 2: the menu (solid black, blocky font) =================
class SpeedPopup : public Popup {
protected:
    TextInput* m_input = nullptr;
    CCDrawNode* m_onDraw = nullptr;

    void paintOnButton() {
        if (!m_onDraw) return;
        paintBlockButton(m_onDraw, g_enabled ? "ON" : "OFF", 80.f, 28.f, 3.f,
                         g_enabled ? UI_GREEN : UI_RED, UI_FILL, g_enabled ? UI_GREEN : UI_RED);
    }

    bool init() {
        if (!Popup::init(300.f, 240.f)) return false;

        g_enabled = Mod::get()->getSavedValue<bool>("enabled", true);

        // hide the default background and draw our own solid black one
        if (m_bgSprite) m_bgSprite->setVisible(false);

        auto bg = CCDrawNode::create();
        quad(bg, CCPoint(-150.f, -120.f), CCPoint(150.f, -120.f), CCPoint(150.f, 120.f), CCPoint(-150.f, 120.f), UI_EDGE);
        quad(bg, CCPoint(-148.f, -118.f), CCPoint(148.f, -118.f), CCPoint(148.f, 118.f), CCPoint(-148.f, 118.f), UI_BLACK);
        quad(bg, CCPoint(-130.f, 77.f), CCPoint(130.f, 77.f), CCPoint(130.f, 79.f), CCPoint(-130.f, 79.f), UI_GREEN);
        bg->setZOrder(-5);
        m_mainLayer->addChildAtPosition(bg, Anchor::Center, CCPoint(0.f, 0.f));

        // all the blocky text
        auto txt = CCDrawNode::create();
        txt->setZOrder(5);
        m_mainLayer->addChildAtPosition(txt, Anchor::Center, CCPoint(0.f, 0.f));
        pixelText(txt, "CLICK HIGHWAY", 0.f, 96.f, 3.f, UI_TEXT, true);
        pixelText(txt, "HIGHWAY", -130.f, 52.f, 2.f, UI_TEXT, false);
        pixelText(txt, "BAR SPEED", -130.f, 14.f, 2.f, UI_TEXT, false);
        pixelText(txt, "0.5 SLOWER  2 FASTER", 0.f, -22.f, 2.f, UI_DIM, true);

        // on / off button
        CCDrawNode* d = nullptr;
        auto onRoot = newBlockRoot(80.f, 28.f, d);
        m_onDraw = d;
        this->paintOnButton();
        auto onBtn = CCMenuItemSpriteExtra::create(onRoot, this, menu_selector(SpeedPopup::onToggle));
        m_buttonMenu->addChildAtPosition(onBtn, Anchor::Center, CCPoint(90.f, 52.f));

        // speed box
        m_input = TextInput::create(100.f, "1", "bigFont.fnt");
        m_input->setFilter("0123456789.");
        m_input->setMaxCharCount(5);
        m_input->setString(fmt::format("{}", g_speed));
        m_mainLayer->addChildAtPosition(m_input, Anchor::Center, CCPoint(82.f, 14.f));

        // apply and reset buttons
        CCDrawNode* d2 = nullptr;
        auto applyRoot = newBlockRoot(116.f, 38.f, d2);
        paintBlockButton(d2, "APPLY", 116.f, 38.f, 3.f, UI_GREEN, UI_FILL, UI_TEXT);
        auto applyBtn = CCMenuItemSpriteExtra::create(applyRoot, this, menu_selector(SpeedPopup::onApply));
        m_buttonMenu->addChildAtPosition(applyBtn, Anchor::Center, CCPoint(-70.f, -78.f));

        CCDrawNode* d3 = nullptr;
        auto resetRoot = newBlockRoot(116.f, 38.f, d3);
        paintBlockButton(d3, "RESET", 116.f, 38.f, 3.f, UI_EDGE, UI_FILL, UI_TEXT);
        auto resetBtn = CCMenuItemSpriteExtra::create(resetRoot, this, menu_selector(SpeedPopup::onReset));
        m_buttonMenu->addChildAtPosition(resetBtn, Anchor::Center, CCPoint(70.f, -78.f));

        return true;
    }

    void onToggle(CCObject*) {
        g_enabled = !g_enabled;
        Mod::get()->setSavedValue<bool>("enabled", g_enabled);
        this->paintOnButton();
    }

    void onApply(CCObject*) {
        float v = std::strtof(std::string(m_input->getString()).c_str(), nullptr);
        if (v < 0.1f) v = 0.1f;
        if (v > 4.f)  v = 4.f;
        g_speed = v;
        Mod::get()->setSavedValue<float>("speed", v);
        this->keyBackClicked(); // closes the menu
    }

    void onReset(CCObject*) {
        g_charts.clear();
        g_state.clear();
        FLAlertLayer::create("Click Highway", "All saved charts were cleared.", "OK")->show();
    }

public:
    static SpeedPopup* create() {
        auto ret = new SpeedPopup();
        if (ret->init()) {
            ret->autorelease();
            g_popupOpen = true;
            return ret;
        }
        delete ret;
        return nullptr;
    }

    ~SpeedPopup() {
        g_popupOpen = false;
    }
};

// Shift + H opens the menu
class $modify(KeyHook, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double timestamp) {
        if (down && !repeat && key == enumKeyCodes::KEY_H
            && this->getShiftKeyPressed() && !g_popupOpen) {
            if (auto popup = SpeedPopup::create()) popup->show();
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
    }
};

// ================= PART 3: the highway =================
class $modify(HighwayLayer, PlayLayer) {
    struct Fields {
        CCDrawNode* draw = nullptr;
        CCLabelBMFont* judge = nullptr;
    };

    void resetLevel() {
        commitAttempt(false, 0.f); // save what the last attempt learned
        PlayLayer::resetLevel();
        startAttempt(this);
    }

    void levelComplete() {
        float x = m_player1 ? m_player1->getPositionX() : g_attemptMaxX;
        commitAttempt(true, std::max(x, g_attemptMaxX));
        PlayLayer::levelComplete();
    }

    void showJudge(std::string const& text, ccColor3B color) {
        auto l = m_fields->judge;
        if (!l) return;
        l->setString(text.c_str());
        l->setColor(color);
        l->stopAllActions();
        l->setOpacity(255);
        l->setScale(0.36f);
        l->runAction(CCScaleTo::create(0.1f, 0.28f));
        l->runAction(CCSequence::create(CCDelayTime::create(0.5f), CCFadeOut::create(0.3f), nullptr));
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        if (!m_player1) return;

        // safety: make sure an attempt is being tracked for this layer
        if (g_layer != this) {
            commitAttempt(false, 0.f);
            startAttempt(this);
        }

        // create the drawing area and the judgement text once
        if (!m_fields->draw) {
            g_speed = Mod::get()->getSavedValue<float>("speed", 1.f);
            g_enabled = Mod::get()->getSavedValue<bool>("enabled", true);

            auto node = CCDrawNode::create();
            node->setPosition(CCPoint(MARGIN, MARGIN));
            node->setZOrder(1000);
            m_uiLayer->addChild(node);
            m_fields->draw = node;

            auto label = CCLabelBMFont::create("", "bigFont.fnt");
            label->setScale(0.28f);
            label->setOpacity(0);
            label->setPosition(CCPoint(MARGIN + CX, MARGIN + 7.f));
            label->setZOrder(1001);
            m_uiLayer->addChild(label);
            m_fields->judge = label;
        }

        float px = m_player1->getPositionX();
        if (g_attemptActive && px > g_attemptMaxX) g_attemptMaxX = px;

        // measure how fast the player is moving (smoothed), so speed portals don't change the bars
        if (dt > 0.0001f && px > g_lastX) {
            float inst = (px - g_lastX) / dt;
            if (inst > 100.f && inst < 1000.f) {
                g_vel += (inst - g_vel) * (1.f - expf(-dt / 0.25f));
            }
        }
        g_lastX = px;

        auto draw = m_fields->draw;
        draw->clear();

        auto cit = g_charts.find(g_attemptId);
        if (cit == g_charts.end()) { g_presses.clear(); return; }
        auto const& notes = cit->second.notes;
        if (g_state.size() != notes.size()) g_state.resize(notes.size(), 0);

        float goodU = GOOD_MS / 1000.f * g_vel; // window in level units

        // ---- highway turned off: keep recording, but draw and judge nothing ----
        if (!g_enabled) {
            for (size_t i = 0; i < notes.size(); i++) {
                if (g_state[i] == 0 && px > notes[i].start + goodU) g_state[i] = 2; // skip silently
            }
            g_presses.clear();
            if (m_fields->judge) m_fields->judge->setOpacity(0);
            return;
        }

        // ---- judge your presses ----
        for (float pressX : g_presses) {
            int best = -1;
            float bestD = goodU + 0.01f;
            for (size_t i = 0; i < notes.size(); i++) {
                if (g_state[i] != 0) continue;
                float d = std::fabs(notes[i].start - pressX);
                if (d < bestD) { bestD = d; best = (int)i; }
            }
            if (best >= 0) {
                float ms = (pressX - notes[best].start) / g_vel * 1000.f;
                if (std::fabs(ms) <= PERFECT_MS) {
                    g_state[best] = 1;
                    showJudge(fmt::format("PERFECT {:+.0f}ms", ms), C_PERFECT);
                } else {
                    g_state[best] = 4;
                    showJudge(fmt::format("GOOD {:+.0f}ms", ms), C_GOOD);
                }
            }
        }
        g_presses.clear();

        // ---- notes you let pass = MISS ----
        for (size_t i = 0; i < notes.size(); i++) {
            if (g_state[i] == 0 && px > notes[i].start + goodU) {
                g_state[i] = 3;
                showJudge("MISS", C_MISS);
            }
        }

        if (notes.empty()) return;

        // ---- draw the highway ----
        // bigger speed multiplier = bars move faster, smaller = slower
        float look   = std::max(g_vel * LOOK_SECONDS / std::max(g_speed, 0.1f), 50.f);
        float yRange = TOP_Y - HIT_Y;

        // distance ahead -> how far up the highway (0 to 1)
        auto fOf = [&](float d) {
            float u = std::clamp(d / look, 0.f, 1.f);
            return (1.f + PERSP_K) * u / (1.f + PERSP_K * u);
        };
        auto yOfF = [&](float f) { return HIT_Y + f * yRange; };
        auto wOfF = [&](float f) { return LANE_W * (1.f - (1.f - FAR_SCALE) * f); };

        float y0 = HIT_Y - 14.f;
        float w0 = wOfF(0.f);
        float w1 = wOfF(1.f);
        float hw0 = w0 * 0.5f;
        float hw1 = w1 * 0.5f;

        // lane background
        quad(draw, CCPoint(CX - hw0, y0), CCPoint(CX + hw0, y0),
                   CCPoint(CX + hw1, TOP_Y), CCPoint(CX - hw1, TOP_Y), COL_LANE);

        // glowing side rails
        draw->drawSegment(CCPoint(CX - hw0, y0), CCPoint(CX - hw1, TOP_Y), 3.5f, withAlpha(COL_RAIL, 0.12f));
        draw->drawSegment(CCPoint(CX + hw0, y0), CCPoint(CX + hw1, TOP_Y), 3.5f, withAlpha(COL_RAIL, 0.12f));
        draw->drawSegment(CCPoint(CX - hw0, y0), CCPoint(CX - hw1, TOP_Y), 1.2f, COL_RAIL);
        draw->drawSegment(CCPoint(CX + hw0, y0), CCPoint(CX + hw1, TOP_Y), 1.2f, COL_RAIL);

        // faint rungs scrolling past
        float phase = fmodf(px, GRID_UNITS);
        if (phase < 0.f) phase += GRID_UNITS;
        for (float d = GRID_UNITS - phase; d < look; d += GRID_UNITS) {
            float f = fOf(d);
            float y = yOfF(f);
            float hw = wOfF(f) * 0.5f;
            draw->drawSegment(CCPoint(CX - hw, y), CCPoint(CX + hw, y), 0.8f,
                              withAlpha(COL_GRID, COL_GRID.a * (1.f - f)));
        }

        // which bar is the next one you need to hit?
        int nextIdx = -1;
        for (size_t i = 0; i < notes.size(); i++) {
            if (g_state[i] == 0) { nextIdx = (int)i; break; }
        }

        // the bars
        float gz = GREEN_SEC * g_vel;
        for (size_t i = 0; i < notes.size(); i++) {
            float d0 = notes[i].start - px; // distance to the press
            float d1 = notes[i].end - px;   // distance to the release

            if (d1 < -3.f) continue;  // already finished
            if (d0 > look) break;     // too far ahead (notes are in order)

            float fb = fOf(d0);
            float yb = yOfF(fb);
            float yt = yOfF(fOf(d1));
            float scaleB = wOfF(fb) / LANE_W; // 1 at the line, smaller far away

            // always keep a visible gap before the next bar
            bool hasNext = (i + 1 < notes.size());
            float maxTop = 0.f;
            if (hasNext) {
                float nyb = yOfF(fOf(notes[i + 1].start - px));
                maxTop = nyb - GAP * scaleB;
                yt = std::min(yt, maxTop);
            }
            float minH = MIN_H * scaleB;
            if (yt < yb + minH) {
                yt = yb + minH;
                // if the minimum height would swallow the gap, squeeze the bar instead
                if (hasNext && yt > maxTop) yt = std::max(yb + 3.f * scaleB, maxTop);
            }

            float ft = std::clamp((yt - HIT_Y) / yRange, 0.f, 1.f);
            float hwb = wOfF(fb) * NOTE_FRAC * 0.5f;
            float hwt = wOfF(ft) * NOTE_FRAC * 0.5f;

            // fade in near the top
            float a = std::clamp((1.f - d0 / look) / 0.2f, 0.f, 1.f);

            // green ONLY while you are holding and the bar is on the line
            bool active = g_holding && d0 <= gz && d1 >= -gz;
            ccColor4F base = active ? COL_ACTIVE : COL_NOTE;

            // the next bar to hit gets brighter and thicker as it gets close
            float soon = 0.f;
            if ((int)i == nextIdx && d0 > 0.f) {
                soon = 1.f - std::clamp(d0 / (0.4f * g_vel), 0.f, 1.f);
            }
            if (soon > 0.f) {
                base.r += (1.f - base.r) * soon * 0.75f;
                base.g += (1.f - base.g) * soon * 0.75f;
                base.b += (1.f - base.b) * soon * 0.75f;
            }

            if (active) {
                quad(draw, CCPoint(CX - hwb - 3.f, yb - 2.f), CCPoint(CX + hwb + 3.f, yb - 2.f),
                           CCPoint(CX + hwt + 3.f, yt + 2.f), CCPoint(CX - hwt - 3.f, yt + 2.f),
                     withAlpha(COL_ACTIVE, 0.18f * a));
            }

            // slim bar body: light fill, thin bright outline
            quad(draw, CCPoint(CX - hwb, yb), CCPoint(CX + hwb, yb),
                       CCPoint(CX + hwt, yt), CCPoint(CX - hwt, yt),
                 withAlpha(base, (active ? 0.40f : 0.22f + 0.2f * soon) * a), 1.2f + soon, withAlpha(base, a));

            // bright cap on the bottom edge = the exact moment to click
            draw->drawSegment(CCPoint(CX - hwb, yb), CCPoint(CX + hwb, yb), 2.4f + 2.2f * soon, withAlpha(base, a));
        }

        // hit zone glow (lights up while you hold)
        ccColor4F hitCol = g_holding ? COL_ACTIVE : COL_GOLD;
        if (g_holding) {
            quad(draw, CCPoint(CX - hw0, HIT_Y - 7.f), CCPoint(CX + hw0, HIT_Y - 7.f),
                       CCPoint(CX + hw0, HIT_Y + 7.f), CCPoint(CX - hw0, HIT_Y + 7.f),
                 withAlpha(COL_ACTIVE, 0.22f));
        }

        // gold hit bar
        draw->drawSegment(CCPoint(CX - hw0 - 3.f, HIT_Y), CCPoint(CX + hw0 + 3.f, HIT_Y), 2.2f, hitCol);

        // arrows pointing at the hit bar
        float lx = CX - hw0 - 14.f;
        float rx = CX + hw0 + 14.f;
        draw->drawSegment(CCPoint(lx - 5.f, HIT_Y + 10.f), CCPoint(lx + 5.f, HIT_Y), 1.8f, hitCol);
        draw->drawSegment(CCPoint(lx - 5.f, HIT_Y - 10.f), CCPoint(lx + 5.f, HIT_Y), 1.8f, hitCol);
        draw->drawSegment(CCPoint(rx + 5.f, HIT_Y + 10.f), CCPoint(rx - 5.f, HIT_Y), 1.8f, hitCol);
        draw->drawSegment(CCPoint(rx + 5.f, HIT_Y - 10.f), CCPoint(rx - 5.f, HIT_Y), 1.8f, hitCol);
    }
};
