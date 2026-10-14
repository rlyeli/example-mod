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
const float LOOK_SECONDS  = 1.0f;   // how many seconds ahead the bars show (at speed 1x)
const float MARGIN        = 10.f;   // distance from the screen corner
const float CX            = 62.f;   // center of the highway
const float LANE_W        = 84.f;   // width of the highway at the bottom
const float FAR_SCALE     = 0.42f;  // how narrow it gets at the top (smaller = more perspective)
const float PERSP_K       = 1.6f;   // how much bars speed up near the line
const float HIT_Y         = 30.f;   // height of the hit bar
const float TOP_Y         = 175.f;  // height of the top of the highway
const float NOTE_FRAC     = 0.82f;  // bar width compared to the lane
const float MIN_H         = 12.f;   // shortest a bar can be
const float GAP           = 4.f;    // empty space between back-to-back bars
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

    bool init() {
        if (!Popup::init(280.f, 170.f)) return false;
        this->setTitle("Click Highway");

        auto label = CCLabelBMFont::create("Bar speed multiplier", "bigFont.fnt");
        label->setScale(0.45f);
        m_mainLayer->addChildAtPosition(label, Anchor::Center, {0.f, 26.f});

        m_input = TextInput::create(110.f, "1", "bigFont.fnt");
        m_input->setFilter("0123456789.");
        m_input->setMaxCharCount(5);
        m_input->setString(fmt::format("{}", g_speed));
        m_mainLayer->addChildAtPosition(m_input, Anchor::Center, {0.f, -4.f});

        auto hint = CCLabelBMFont::create("0.5 = slower bars    2 = faster bars", "chatFont.fnt");
        hint->setScale(0.6f);
        hint->setOpacity(160);
        m_mainLayer->addChildAtPosition(hint, Anchor::Center, {0.f, -34.f});

        auto applyBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Apply"), this, menu_selector(SpeedPopup::onApply));
        m_buttonMenu->addChildAtPosition(applyBtn, Anchor::Bottom, {-50.f, 26.f});

        auto resetBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Reset"), this, menu_selector(SpeedPopup::onReset));
        m_buttonMenu->addChildAtPosition(resetBtn, Anchor::Bottom, {50.f, 26.f});

        return true;
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

        // ---- judge your presses ----
        float goodU = GOOD_MS / 1000.f * g_vel; // window in level units
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
        float look   = std::max(g_vel * LOOK_SECONDS * g_speed, 50.f);
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

            // keep a visible gap before the next bar
            if (i + 1 < notes.size()) {
                float nyb = yOfF(fOf(notes[i + 1].start - px));
                yt = std::min(yt, nyb - GAP * scaleB);
            }
            float minH = MIN_H * scaleB;
            if (yt < yb + minH) yt = yb + minH;

            float ft = std::clamp((yt - HIT_Y) / yRange, 0.f, 1.f);
            float wb = wOfF(fb) * NOTE_FRAC;
            float wt = wOfF(ft) * NOTE_FRAC;

            // fade in near the top
            float a = std::clamp((1.f - d0 / look) / 0.2f, 0.f, 1.f);

            // green ONLY while you are holding and the bar is on the line
            bool active = g_holding && d0 <= gz && d1 >= -gz;
            ccColor4F base = active ? COL_ACTIVE : COL_NOTE;

            if (active) {
                quad(draw, {CX - wb / 2.f - 4.f, yb - 3.f}, {CX + wb / 2.f + 4.f, yb - 3.f},
                           {CX + wt / 2.f + 4.f, yt + 3.f}, {CX - wt / 2.f - 4.f, yt + 3.f},
                     withAlpha(COL_ACTIVE, 0.20f * a));
            }

            // rung body: dark inside, bright outline
            quad(draw, {CX - wb / 2.f, yb}, {CX + wb / 2.f, yb},
                       {CX + wt / 2.f, yt}, {CX - wt / 2.f, yt},
                 withAlpha(base, (active ? 0.45f : 0.28f) * a), 1.6f, withAlpha(base, a));

            // little block in the middle of the bar
            float headZone = std::min(yt - yb, 16.f * scaleB);
            float hs = headZone * 0.5f;
            float hw = hs * 1.5f;
            float hy = yb + headZone * 0.25f;
            quad(draw, {CX - hw / 2.f, hy}, {CX + hw / 2.f, hy},
                       {CX + hw / 2.f, hy + hs}, {CX - hw / 2.f, hy + hs},
                 withAlpha(base, a));
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
