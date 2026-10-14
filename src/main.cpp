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
const float BASE_LOOKAHEAD = 300.f; // how far ahead bars show (at speed 1x)
const float PANEL_W        = 70.f;
const float PANEL_H        = 170.f;
const float MARGIN         = 10.f;  // distance from the screen corner
const float HIT_Y          = 38.f;  // height of the hit zone
const float TOP_Y          = PANEL_H - 14.f;
const float NOTE_W         = 28.f;  // bar width
const float MIN_H          = 6.f;   // shortest a bar can be
const float GAP            = 3.f;   // empty space between back-to-back bars
const float GREEN_ZONE     = 14.f;  // how close to the line a bar must be to go green

const float PERFECT_W      = 10.f;  // click within this distance = PERFECT
const float GOOD_W         = 28.f;  // click within this distance = GOOD (later = MISS)

const float DEATH_MARGIN   = 100.f; // clicks this close to where you died are not saved
const float CONNECT_SLACK  = 150.f; // attempt must start near saved part to add to it

// ================= COLORS =================
const ccColor4F COL_PANEL       = {0.05f, 0.06f, 0.11f, 0.78f};
const ccColor4F COL_BORDER      = {1.f, 1.f, 1.f, 0.14f};
const ccColor4F COL_LANE        = {1.f, 1.f, 1.f, 0.045f};
const ccColor4F COL_ZONE_OFF    = {1.f, 1.f, 1.f, 0.10f};
const ccColor4F COL_ZONE_ON     = {0.35f, 1.f, 0.55f, 0.35f};
const ccColor4F COL_ZONE_BORDER = {1.f, 1.f, 1.f, 0.45f};
const ccColor4F COL_NOTE        = {0.30f, 0.80f, 1.f, 1.f};   // cyan
const ccColor4F COL_ACTIVE      = {0.35f, 1.f, 0.55f, 1.f};   // green
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

static bool  g_holding   = false;
static float g_speed     = 1.f;
static bool  g_popupOpen = false;

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

    auto& notes = g_charts[g_attemptId].notes;
    g_state.assign(notes.size(), 0);
    for (size_t i = 0; i < notes.size(); i++) {
        // notes behind where we spawned (practice checkpoints) don't count
        if (notes[i].start < g_attemptStartX) g_state[i] = 2;
    }
}

// Filled rounded rectangle (optional border)
static void roundedRect(CCDrawNode* n, float x, float y, float w, float h, float r,
                        ccColor4F fill, float borderW = 0.f,
                        ccColor4F border = {0.f, 0.f, 0.f, 0.f}) {
    if (w <= 0.f || h <= 0.f) return;
    r = std::min(r, std::min(w, h) / 2.f);
    const float PI = 3.14159265f;
    const int seg = 8;
    std::vector<CCPoint> pts;
    auto arc = [&](float cx, float cy, float a0) {
        for (int i = 0; i <= seg; i++) {
            float a = a0 + (PI / 2.f) * i / seg;
            pts.push_back({cx + r * cosf(a), cy + r * sinf(a)});
        }
    };
    arc(x + w - r, y + r,     -PI / 2.f);
    arc(x + w - r, y + h - r, 0.f);
    arc(x + r,     y + h - r, PI / 2.f);
    arc(x + r,     y + r,     PI);
    n->drawPolygon(pts.data(), pts.size(), fill, borderW, border);
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

    void showJudge(char const* text, ccColor3B color) {
        auto l = m_fields->judge;
        if (!l) return;
        l->setString(text);
        l->setColor(color);
        l->stopAllActions();
        l->setOpacity(255);
        l->setScale(0.38f);
        l->runAction(CCScaleTo::create(0.1f, 0.3f));
        l->runAction(CCSequence::create(CCDelayTime::create(0.4f), CCFadeOut::create(0.3f), nullptr));
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
            label->setScale(0.3f);
            label->setOpacity(0);
            label->setPosition({MARGIN + PANEL_W / 2.f, MARGIN + 16.f});
            label->setZOrder(1001);
            m_uiLayer->addChild(label);
            m_fields->judge = label;
        }

        float px = m_player1->getPositionX();
        if (g_attemptActive && px > g_attemptMaxX) g_attemptMaxX = px;

        auto draw = m_fields->draw;
        draw->clear();

        auto cit = g_charts.find(g_attemptId);
        if (cit == g_charts.end()) { g_presses.clear(); return; }
        auto const& notes = cit->second.notes;
        if (g_state.size() != notes.size()) g_state.resize(notes.size(), 0);

        // ---- judge your presses ----
        for (float pressX : g_presses) {
            int best = -1;
            float bestD = GOOD_W + 0.01f;
            for (size_t i = 0; i < notes.size(); i++) {
                if (g_state[i] != 0) continue;
                float d = std::fabs(notes[i].start - pressX);
                if (d < bestD) { bestD = d; best = (int)i; }
            }
            if (best >= 0) {
                g_state[best] = 1;
                if (bestD <= PERFECT_W) showJudge("PERFECT", C_PERFECT);
                else showJudge("GOOD", C_GOOD);
            }
        }
        g_presses.clear();

        // ---- notes you let pass = MISS ----
        for (size_t i = 0; i < notes.size(); i++) {
            if (g_state[i] == 0 && px > notes[i].start + GOOD_W) {
                g_state[i] = 2;
                showJudge("MISS", C_MISS);
            }
        }

        if (notes.empty()) return;

        // ---- draw the highway ----
        float look = BASE_LOOKAHEAD * g_speed;
        float cx = PANEL_W / 2.f;

        // panel and lane
        roundedRect(draw, 0.f, 0.f, PANEL_W, PANEL_H, 14.f, COL_PANEL, 1.f, COL_BORDER);
        roundedRect(draw, cx - NOTE_W / 2.f - 5.f, 8.f, NOTE_W + 10.f, PANEL_H - 16.f, 10.f, COL_LANE);

        // hit zone (lights up while you hold)
        roundedRect(draw, cx - NOTE_W / 2.f - 5.f, HIT_Y - 8.f, NOTE_W + 10.f, 16.f, 8.f,
                    g_holding ? COL_ZONE_ON : COL_ZONE_OFF, 1.f,
                    g_holding ? COL_ACTIVE : COL_ZONE_BORDER);

        // distance ahead -> height on the panel
        auto yOf = [&](float d) {
            d = std::clamp(d, 0.f, look);
            return HIT_Y + (d / look) * (TOP_Y - HIT_Y);
        };

        for (size_t i = 0; i < notes.size(); i++) {
            float d0 = notes[i].start - px; // distance to the press
            float d1 = notes[i].end - px;   // distance to the release

            if (d1 < -3.f) continue;  // already finished
            if (d0 > look) break;     // too far ahead (notes are in order)

            float yb = yOf(d0);
            float yt = yOf(d1);

            // keep a visible gap before the next bar
            if (i + 1 < notes.size()) {
                float nb = yOf(notes[i + 1].start - px);
                yt = std::min(yt, nb - GAP);
            }
            if (yt < yb + MIN_H) yt = yb + MIN_H;

            // fade in near the top
            float a = std::clamp((look - d0) / (look * 0.15f), 0.f, 1.f);

            // green ONLY while you are holding and the bar is on the line
            bool active = g_holding && d0 <= GREEN_ZONE && d1 >= -GREEN_ZONE;

            ccColor4F col = active ? COL_ACTIVE : COL_NOTE;
            col.a *= a;

            if (active) {
                ccColor4F glow = COL_ACTIVE;
                glow.a = 0.22f * a;
                roundedRect(draw, cx - NOTE_W / 2.f - 4.f, yb - 4.f, NOTE_W + 8.f, (yt - yb) + 8.f, 10.f, glow);
            }

            roundedRect(draw, cx - NOTE_W / 2.f, yb, NOTE_W, yt - yb, 7.f, col);

            // little shine on top of longer bars
            if (yt - yb > 14.f) {
                roundedRect(draw, cx - NOTE_W / 2.f + 5.f, yt - 6.f, NOTE_W - 10.f, 2.f, 1.f,
                            {1.f, 1.f, 1.f, 0.28f * a});
            }
        }
    }
};
