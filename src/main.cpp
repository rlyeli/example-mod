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
const float LOOK_SECONDS  = 1.6f;   // seconds ahead the bars show at 1x (bigger = bars travel slower)
const float MARGIN        = 10.f;   // distance from the screen corner
const float CX            = 66.f;   // center of the highway
const float LANE_W        = 90.f;   // width of the highway at the bottom
const float FAR_SCALE     = 0.50f;  // how narrow it gets at the top (smaller = more perspective)
const float PERSP_K       = 1.0f;   // how much bars speed up near the line (lower = more even spacing)
const float HIT_Y         = 30.f;   // height of the hit bar
const float TOP_Y         = 205.f;  // height of the top of the highway (taller = more room between bars)
const float NOTE_FRAC     = 0.70f;  // bar width compared to the lane
const float MIN_H         = 7.f;    // shortest a bar can be
const float GAP           = 7.f;    // empty space between back-to-back bars
const float GRID_UNITS    = 80.f;   // spacing of the faint rungs
const float GREEN_SEC     = 0.05f;  // how close to the line a bar must be to turn green

const float PERFECT_MS    = 35.f;   // click within this = PERFECT
const float GOOD_MS       = 90.f;   // click within this = GOOD (later = MISS)

const float DEATH_MARGIN  = 100.f;  // clicks this close to where you died are not saved
const float CONNECT_SLACK = 150.f;  // attempt must start near saved part to add to it

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
static std::vector<char>  g_state;      // per saved note: 0 waiting, 1 hit, 2 missed/skipped
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
static bool  g_glass     = false;   // menu style: false = classic, true = glass

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

// Filled rounded rectangle (used for the glass menu)
static void roundRect(CCDrawNode* n, float x, float y, float w, float h, float r,
                      ccColor4F fill, float borderW = 0.f,
                      ccColor4F border = {0.f, 0.f, 0.f, 0.f}) {
    r = std::min(r, std::min(w, h) / 2.f);
    const int seg = 10;
    const float PI = 3.14159265f;
    CCPoint centers[4] = {{x + w - r, y + r}, {x + w - r, y + h - r}, {x + r, y + h - r}, {x + r, y + r}};
    float startDeg[4] = {-90.f, 0.f, 90.f, 180.f};

    std::vector<CCPoint> pts;
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i <= seg; i++) {
            float a = (startDeg[k] + 90.f * i / seg) * PI / 180.f;
            pts.push_back({centers[k].x + r * cosf(a), centers[k].y + r * sinf(a)});
        }
    }
    n->drawPolygon(pts.data(), (unsigned int)pts.size(), fill, borderW, border);
}

// Save this attempt's new clicks into the level's chart
static void commitAttempt(bool finished, float finishX) {
    if (!g_attemptActive) return;
    g_attemptActive = false;

    auto& chart = g_charts[g_attemptId];
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

// ================= PART 2: the menu =================
class SpeedPopup : public Popup {
protected:
    TextInput* m_input = nullptr;
    ButtonSprite* m_styleSpr = nullptr;
    CCMenuItemToggler* m_toggler = nullptr;

    bool init() {
        if (!Popup::init(280.f, 230.f)) return false;
        this->setTitle("Click Highway");

        // load saved settings
        g_glass = Mod::get()->getSavedValue<bool>("glass", false);
        g_enabled = Mod::get()->getSavedValue<bool>("enabled", true);

        // on / off switch
        auto onLabel = CCLabelBMFont::create("Highway on/off", "bigFont.fnt");
        onLabel->setScale(0.45f);
        m_mainLayer->addChildAtPosition(onLabel, Anchor::Center, {-25.f, 72.f});

        m_toggler = CCMenuItemToggler::createWithStandardSprites(
            this, menu_selector(SpeedPopup::onToggle), 0.7f);
        m_toggler->toggle(g_enabled);
        m_buttonMenu->addChildAtPosition(m_toggler, Anchor::Center, {85.f, 72.f});

        // speed
        auto label = CCLabelBMFont::create("Bar speed multiplier", "bigFont.fnt");
        label->setScale(0.45f);
        m_mainLayer->addChildAtPosition(label, Anchor::Center, {0.f, 34.f});

        m_input = TextInput::create(110.f, "1", "bigFont.fnt");
        m_input->setFilter("0123456789.");
        m_input->setMaxCharCount(5);
        m_input->setString(fmt::format("{}", g_speed));
        m_mainLayer->addChildAtPosition(m_input, Anchor::Center, {0.f, 4.f});

        auto hint = CCLabelBMFont::create("0.5 = slower bars    2 = faster bars", "chatFont.fnt");
        hint->setScale(0.6f);
        hint->setOpacity(160);
        m_mainLayer->addChildAtPosition(hint, Anchor::Center, {0.f, -22.f});

        // menu style switch
        m_styleSpr = ButtonSprite::create("Style: Classic");
        auto styleBtn = CCMenuItemSpriteExtra::create(
            m_styleSpr, this, menu_selector(SpeedPopup::onStyle));
        m_buttonMenu->addChildAtPosition(styleBtn, Anchor::Bottom, {0.f, 68.f});

        auto applyBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Apply"), this, menu_selector(SpeedPopup::onApply));
        m_buttonMenu->addChildAtPosition(applyBtn, Anchor::Bottom, {-50.f, 26.f});

        auto resetBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Reset"), this, menu_selector(SpeedPopup::onReset));
        m_buttonMenu->addChildAtPosition(resetBtn, Anchor::Bottom, {50.f, 26.f});

        this->applyStyle();
        return true;
    }

    // Swap between the original menu look and the glass look
    void applyStyle() {
        if (auto old = m_mainLayer->getChildByID("glass-bg")) old->removeFromParent();

        if (m_bgSprite) m_bgSprite->setVisible(!g_glass);

        if (g_glass) {
            auto size = m_mainLayer->getContentSize();
            auto node = CCDrawNode::create();
            node->setID("glass-bg");

            float w = size.width, h = size.height, r = 26.f;

            // soft shadow under the panel
            roundRect(node, 2.f, -4.f, w - 4.f, h, r, {0.f, 0.f, 0.f, 0.22f});
            // frosted body with a light edge
            roundRect(node, 0.f, 0.f, w, h, r,
                      {0.78f, 0.88f, 1.f, 0.17f}, 1.4f, {1.f, 1.f, 1.f, 0.55f});
            // glossy sheen across the top half
            roundRect(node, 4.f, h * 0.52f, w - 8.f, h * 0.48f - 4.f, r - 4.f,
                      {1.f, 1.f, 1.f, 0.09f});
            // thin bright highlight along the top edge
            node->drawSegment({r, h - 2.f}, {w - r, h - 2.f}, 0.8f, {1.f, 1.f, 1.f, 0.7f});

            m_mainLayer->addChild(node, -1);
        }

        if (m_styleSpr) m_styleSpr->setString(g_glass ? "Style: Glass" : "Style: Classic");
    }

    void onToggle(CCObject* sender) {
        // the toggler reports its state from before the click
        g_enabled = !static_cast<CCMenuItemToggler*>(sender)->isToggled();
        Mod::get()->setSavedValue<bool>("enabled", g_enabled);
    }

    void onStyle(CCObject*) {
        g_glass = !g_glass;
        Mod::get()->setSavedValue<bool>("glass", g_glass);
        this->applyStyle();
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
            node->setPosition({MARGIN, MARGIN});
            node->setZOrder(1000);
            m_uiLayer->addChild(node);
            m_fields->draw = node;

            auto label = CCLabelBMFont::create("", "bigFont.fnt");
            label->setScale(0.28f);
            label->setOpacity(0);
            label->setPosition({MARGIN + CX, MARGIN + 7.f});
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
                g_state[best] = 1;
                float ms = (pressX - notes[best].start) / g_vel * 1000.f;
                if (std::fabs(ms) <= PERFECT_MS) showJudge(fmt::format("PERFECT {:+.0f}ms", ms), C_PERFECT);
                else showJudge(fmt::format("GOOD {:+.0f}ms", ms), C_GOOD);
            }
        }
        g_presses.clear();

        // ---- notes you let pass = MISS ----
        for (size_t i = 0; i < notes.size(); i++) {
            if (g_state[i] == 0 && px > notes[i].start + goodU) {
                g_state[i] = 2;
                showJudge("MISS", C_MISS);
            }
        }

        if (notes.empty()) return;

        // ---- draw the highway ----
        // bigger speed multiplier = bars move faster, smaller = slower
        float look   = std::max(g_vel * LOOK_SECONDS / std::max(g_speed, 0.1f), 50.f);
        float yRange = TOP_Y - HIT_Y;

        // distance ahead -> how far up the highway (0 to 1), speeding up near the line
        auto fOf = [&](float d) {
            float u = std::clamp(d / look, 0.f, 1.f);
            return (1.f + PERSP_K) * u / (1.f + PERSP_K * u);
        };
        auto yOfF = [&](float f) { return HIT_Y + f * yRange; };
        auto wOfF = [&](float f) { return LANE_W * (1.f - (1.f - FAR_SCALE) * f); };

        float y0 = HIT_Y - 14.f;
        float w0 = wOfF(0.f);
        float w1 = wOfF(1.f);

        // lane background
        quad(draw, {CX - w0 / 2.f, y0}, {CX + w0 / 2.f, y0},
                   {CX + w1 / 2.f, TOP_Y}, {CX - w1 / 2.f, TOP_Y}, COL_LANE);

        // glowing side rails
        draw->drawSegment({CX - w0 / 2.f, y0}, {CX - w1 / 2.f, TOP_Y}, 3.5f, withAlpha(COL_RAIL, 0.12f));
        draw->drawSegment({CX + w0 / 2.f, y0}, {CX + w1 / 2.f, TOP_Y}, 3.5f, withAlpha(COL_RAIL, 0.12f));
        draw->drawSegment({CX - w0 / 2.f, y0}, {CX - w1 / 2.f, TOP_Y}, 1.2f, COL_RAIL);
        draw->drawSegment({CX + w0 / 2.f, y0}, {CX + w1 / 2.f, TOP_Y}, 1.2f, COL_RAIL);

        // faint rungs scrolling past
        float phase = fmodf(px, GRID_UNITS);
        if (phase < 0.f) phase += GRID_UNITS;
        for (float d = GRID_UNITS - phase; d < look; d += GRID_UNITS) {
            float f = fOf(d);
            float y = yOfF(f);
            float w = wOfF(f);
            draw->drawSegment({CX - w / 2.f, y}, {CX + w / 2.f, y}, 0.8f, withAlpha(COL_GRID, COL_GRID.a * (1.f - f)));
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
            float wb = wOfF(fb) * NOTE_FRAC;
            float wt = wOfF(ft) * NOTE_FRAC;

            // fade in near the top
            float a = std::clamp((1.f - d0 / look) / 0.2f, 0.f, 1.f);

            // green ONLY while you are holding and the bar is on the line
            bool active = g_holding && d0 <= gz && d1 >= -gz;
            ccColor4F base = active ? COL_ACTIVE : COL_NOTE;

            if (active) {
                quad(draw, {CX - wb / 2.f - 3.f, yb - 2.f}, {CX + wb / 2.f + 3.f, yb - 2.f},
                           {CX + wt / 2.f + 3.f, yt + 2.f}, {CX - wt / 2.f - 3.f, yt + 2.f},
                     withAlpha(COL_ACTIVE, 0.18f * a));
            }

            // slim bar body: light fill, thin bright outline
            quad(draw, {CX - wb / 2.f, yb}, {CX + wb / 2.f, yb},
                       {CX + wt / 2.f, yt}, {CX - wt / 2.f, yt},
                 withAlpha(base, (active ? 0.40f : 0.22f) * a), 1.2f, withAlpha(base, a));

            // bright cap on the bottom edge = the exact moment to click
            draw->drawSegment({CX - wb / 2.f, yb}, {CX + wb / 2.f, yb}, 2.4f, withAlpha(base, a));
        }

        // hit zone glow (lights up while you hold)
        ccColor4F hitCol = g_holding ? COL_ACTIVE : COL_GOLD;
        if (g_holding) {
            quad(draw, {CX - w0 / 2.f, HIT_Y - 7.f}, {CX + w0 / 2.f, HIT_Y - 7.f},
                       {CX + w0 / 2.f, HIT_Y + 7.f}, {CX - w0 / 2.f, HIT_Y + 7.f},
                 withAlpha(COL_ACTIVE, 0.22f));
        }

        // gold hit bar
        draw->drawSegment({CX - w0 / 2.f - 3.f, HIT_Y}, {CX + w0 / 2.f + 3.f, HIT_Y}, 2.2f, hitCol);

        // arrows pointing at the hit bar
        float lx = CX - w0 / 2.f - 14.f;
        float rx = CX + w0 / 2.f + 14.f;
        draw->drawSegment({lx - 5.f, HIT_Y + 10.f}, {lx + 5.f, HIT_Y}, 1.8f, hitCol);
        draw->drawSegment({lx - 5.f, HIT_Y - 10.f}, {lx + 5.f, HIT_Y}, 1.8f, hitCol);
        draw->drawSegment({rx + 5.f, HIT_Y + 10.f}, {rx - 5.f, HIT_Y}, 1.8f, hitCol);
        draw->drawSegment({rx + 5.f, HIT_Y - 10.f}, {rx - 5.f, HIT_Y}, 1.8f, hitCol);
    }
};
