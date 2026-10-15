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
const float CX            = 76.f;   // center of the highway
const float LANE_W        = 112.f;  // width of the highway at the bottom
const float FAR_SCALE     = 0.55f;  // how narrow it gets at the top (smaller = more perspective)
const float PERSP_K       = 0.3f;   // how much bars speed up near the line (0 = perfectly even)
const float HIT_Y         = 34.f;   // height of the hit bar
const float TOP_Y         = 215.f;  // height of the top of the highway
const float NOTE_FRAC     = 0.80f;  // bar width compared to the lane
const float MIN_H         = 11.f;   // shortest a bar can be
const float GAP           = 8.f;    // empty space between back-to-back bars
const float GRID_UNITS    = 80.f;   // spacing of the faint rungs
const float GREEN_SEC     = 0.05f;  // how close to the line a bar must be to turn green

const float PERFECT_MS    = 20.f;   // click within this = PERFECT
const float GOOD_MS       = 60.f;   // click within this = GOOD (later = MISS)

const float DEATH_MARGIN  = 100.f;  // clicks this close to where you died are not saved
const float CONNECT_SLACK = 150.f;  // attempt must start near saved part to add to it
const float REPAIR_UNITS  = 190.f;  // how much chart is forgotten when it killed you
const float REPAIR_SLACK  = 20.f;

// ================= COLORS =================
const ccColor4F COL_LANE   = {0.02f, 0.05f, 0.07f, 0.80f};
const ccColor4F COL_RAIL   = {0.45f, 0.90f, 1.f, 0.90f};
const ccColor4F COL_GRID   = {0.45f, 0.90f, 1.f, 0.16f};
const ccColor4F COL_A      = {0.25f, 0.85f, 1.f, 1.f};    // cyan
const ccColor4F COL_B      = {1.f, 0.45f, 0.85f, 1.f};    // pink
const ccColor4F COL_ACTIVE = {0.35f, 1.f, 0.55f, 1.f};    // green
const ccColor4F COL_GOLD   = {1.f, 0.80f, 0.25f, 1.f};
const ccColor3B C_PERFECT = {255, 214, 92};
const ccColor3B C_GOOD    = {110, 255, 150};
const ccColor3B C_MISS    = {255, 90, 90};

// menu colors
const ccColor4F M_PANEL  = {0.055f, 0.062f, 0.080f, 0.98f};
const ccColor4F M_BORDER = {1.f, 1.f, 1.f, 0.10f};
const ccColor4F M_ROW    = {1.f, 1.f, 1.f, 0.05f};
const ccColor4F M_ACCENT = {0.22f, 0.95f, 0.70f, 1.f};

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
static float g_speed     = 1.f;     // bar speed multiplier
static float g_offset    = 0.f;     // timing offset in milliseconds
static bool  g_popupOpen = false;
static bool  g_enabled   = true;    // highway on/off

static ccColor4F withAlpha(ccColor4F c, float a) {
    c.a = a;
    return c;
}

// Filled four-sided shape (bottom-left, bottom-right, top-right, top-left)
static void quad(CCDrawNode* n, CCPoint a, CCPoint b, CCPoint c, CCPoint d,
                 ccColor4F fill, float borderW = 0.f,
                 ccColor4F border = ccColor4F{0.f, 0.f, 0.f, 0.f}) {
    CCPoint pts[4] = {a, b, c, d};
    n->drawPolygon(pts, 4, fill, borderW, border);
}

// Filled rounded rectangle (x, y = bottom-left corner)
static void roundRect(CCDrawNode* n, float x, float y, float w, float h, float r,
                      ccColor4F fill, float borderW = 0.f,
                      ccColor4F border = ccColor4F{0.f, 0.f, 0.f, 0.f}) {
    if (w <= 0.f || h <= 0.f) return;
    r = std::min(r, std::min(w, h) * 0.5f);
    const int seg = 10;
    const float PI = 3.14159265f;
    std::vector<CCPoint> pts;
    auto arc = [&](float ax, float ay, float startDeg) {
        for (int i = 0; i <= seg; i++) {
            float deg = startDeg + 90.f * static_cast<float>(i) / static_cast<float>(seg);
            float a = deg * PI / 180.f;
            pts.push_back(CCPoint(ax + r * cosf(a), ay + r * sinf(a)));
        }
    };
    arc(x + w - r, y + r, -90.f);
    arc(x + w - r, y + h - r, 0.f);
    arc(x + r, y + h - r, 90.f);
    arc(x + r, y + r, 180.f);
    n->drawPolygon(pts.data(), static_cast<unsigned int>(pts.size()), fill, borderW, border);
}

// ================= SAVING CHARTS TO DISK =================
static std::string encodeChart(Chart const& c) {
    std::string s = fmt::format("{:.2f}", c.reach);
    for (auto const& n : c.notes) {
        s += fmt::format(";{:.2f},{:.2f}", n.start, n.end);
    }
    return s;
}

static Chart decodeChart(std::string const& s) {
    Chart c;
    const char* p = s.c_str();
    char* e = nullptr;
    c.reach = std::strtof(p, &e);
    p = e;
    while (*p == ';') {
        ++p;
        float a = std::strtof(p, &e);
        p = e;
        if (*p != ',') break;
        ++p;
        float b = std::strtof(p, &e);
        p = e;
        c.notes.push_back({a, b});
    }
    return c;
}

static void saveChart(int id) {
    Mod::get()->setSavedValue<std::string>(fmt::format("chart_{}", id), encodeChart(g_charts[id]));

    std::string idx = Mod::get()->getSavedValue<std::string>("chart_ids", "");
    std::string hay = "," + idx + ",";
    std::string tok = fmt::format(",{},", id);
    if (hay.find(tok) == std::string::npos) {
        if (!idx.empty()) idx += ",";
        idx += std::to_string(id);
        Mod::get()->setSavedValue<std::string>("chart_ids", idx);
    }
}

static void clearAllCharts() {
    std::string idx = Mod::get()->getSavedValue<std::string>("chart_ids", "");
    const char* p = idx.c_str();
    char* e = nullptr;
    while (*p) {
        long id = std::strtol(p, &e, 10);
        if (e == p) break;
        Mod::get()->setSavedValue<std::string>(fmt::format("chart_{}", id), "");
        p = e;
        if (*p == ',') ++p;
    }
    Mod::get()->setSavedValue<std::string>("chart_ids", "");
    g_charts.clear();
    g_state.clear();
}

// ================= RECORDING =================
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
            saveChart(g_attemptId);
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
        saveChart(g_attemptId);
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

    // load the saved chart for this level the first time we see it
    if (g_charts.find(g_attemptId) == g_charts.end()) {
        std::string saved = Mod::get()->getSavedValue<std::string>(fmt::format("chart_{}", g_attemptId), "");
        if (!saved.empty()) g_charts[g_attemptId] = decodeChart(saved);
    }

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
// a rounded button with text inside
static CCNode* pillRoot(std::string const& text, float w, float h, bool filled) {
    auto root = CCNode::create();
    root->setContentSize(CCSize(w, h));
    root->setAnchorPoint(CCPoint(0.5f, 0.5f));

    float hw = w * 0.5f;
    float hh = h * 0.5f;

    auto d = CCDrawNode::create();
    d->setPosition(CCPoint(hw, hh));
    if (filled) {
        roundRect(d, -hw, -hh, w, h, hh, M_ACCENT);
    } else {
        roundRect(d, -hw, -hh, w, h, hh, ccColor4F{1.f, 1.f, 1.f, 0.07f}, 1.2f, ccColor4F{1.f, 1.f, 1.f, 0.25f});
    }
    root->addChild(d);

    auto label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
    label->setScale(0.45f);
    label->setColor(filled ? ccColor3B{12, 16, 20} : ccColor3B{235, 240, 245});
    label->setPosition(CCPoint(hw, hh));
    root->addChild(label);
    return root;
}

class SpeedPopup : public Popup {
protected:
    TextInput* m_speedInput = nullptr;
    TextInput* m_offsetInput = nullptr;
    CCDrawNode* m_switchDraw = nullptr;

    void paintSwitch() {
        if (!m_switchDraw) return;
        m_switchDraw->clear();
        ccColor4F track = g_enabled ? M_ACCENT : ccColor4F{1.f, 1.f, 1.f, 0.16f};
        roundRect(m_switchDraw, -25.f, -13.f, 50.f, 26.f, 13.f, track);
        float knobX = g_enabled ? 12.f : -12.f;
        m_switchDraw->drawDot(CCPoint(knobX, 0.f), 9.f, ccColor4F{1.f, 1.f, 1.f, 1.f});
    }

    // add a text label; ax = 0 left aligned, 0.5 centered, 1 right aligned
    void addText(std::string const& text, char const* font, float scale, float x, float y,
                 float ax, ccColor3B color, unsigned char opacity) {
        auto label = CCLabelBMFont::create(text.c_str(), font);
        label->setScale(scale);
        label->setColor(color);
        label->setOpacity(opacity);
        label->setAnchorPoint(CCPoint(ax, 0.5f));
        label->setPosition(CCPoint(0.f, 0.f));
        auto holder = CCNode::create();
        holder->addChild(label);
        m_mainLayer->addChildAtPosition(holder, Anchor::Center, CCPoint(x, y));
    }

    bool init() {
        if (!Popup::init(300.f, 280.f)) return false;

        g_enabled = Mod::get()->getSavedValue<bool>("enabled", true);

        // hide the default background and close button, we draw our own
        if (m_bgSprite) m_bgSprite->setVisible(false);
        if (m_closeBtn) m_closeBtn->setVisible(false);

        // ---- card, rows and divider ----
        auto bg = CCDrawNode::create();
        bg->setZOrder(-5);
        roundRect(bg, -146.f, -148.f, 292.f, 280.f, 24.f, ccColor4F{0.f, 0.f, 0.f, 0.35f});
        roundRect(bg, -150.f, -140.f, 300.f, 280.f, 22.f, M_PANEL, 1.5f, M_BORDER);
        roundRect(bg, -134.f, 102.f, 4.f, 22.f, 2.f, M_ACCENT);
        quad(bg, CCPoint(-132.f, 81.f), CCPoint(132.f, 81.f), CCPoint(132.f, 82.f), CCPoint(-132.f, 82.f),
             ccColor4F{1.f, 1.f, 1.f, 0.10f});
        roundRect(bg, -132.f, 25.f, 264.f, 46.f, 12.f, M_ROW);
        roundRect(bg, -132.f, -29.f, 264.f, 46.f, 12.f, M_ROW);
        roundRect(bg, -132.f, -83.f, 264.f, 46.f, 12.f, M_ROW);
        m_mainLayer->addChildAtPosition(bg, Anchor::Center, CCPoint(0.f, 0.f));

        // ---- text ----
        ccColor3B white = {255, 255, 255};
        ccColor3B dim = {150, 160, 175};
        this->addText("Click Highway", "bigFont.fnt", 0.55f, -122.f, 113.f, 0.f, white, 255);
        this->addText("Settings", "chatFont.fnt", 0.70f, -122.f, 94.f, 0.f, dim, 255);

        this->addText("Highway", "bigFont.fnt", 0.42f, -120.f, 56.f, 0.f, white, 255);
        this->addText("Show or hide the bars", "chatFont.fnt", 0.62f, -120.f, 39.f, 0.f, dim, 255);

        this->addText("Bar speed", "bigFont.fnt", 0.42f, -120.f, 2.f, 0.f, white, 255);
        this->addText("0.5 slower  -  2 faster", "chatFont.fnt", 0.62f, -120.f, -15.f, 0.f, dim, 255);

        this->addText("Timing offset", "bigFont.fnt", 0.42f, -120.f, -52.f, 0.f, white, 255);
        this->addText("ms  -  raise it if you click late", "chatFont.fnt", 0.62f, -120.f, -69.f, 0.f, dim, 255);

        // ---- on / off switch ----
        auto swRoot = CCNode::create();
        swRoot->setContentSize(CCSize(56.f, 30.f));
        swRoot->setAnchorPoint(CCPoint(0.5f, 0.5f));
        m_switchDraw = CCDrawNode::create();
        m_switchDraw->setPosition(CCPoint(28.f, 15.f));
        swRoot->addChild(m_switchDraw);
        this->paintSwitch();
        auto swBtn = CCMenuItemSpriteExtra::create(swRoot, this, menu_selector(SpeedPopup::onToggle));
        m_buttonMenu->addChildAtPosition(swBtn, Anchor::Center, CCPoint(98.f, 48.f));

        // ---- number boxes ----
        m_speedInput = TextInput::create(76.f, "1", "bigFont.fnt");
        m_speedInput->setFilter("0123456789.");
        m_speedInput->setMaxCharCount(5);
        m_speedInput->setString(fmt::format("{}", g_speed));
        m_mainLayer->addChildAtPosition(m_speedInput, Anchor::Center, CCPoint(86.f, -6.f));

        m_offsetInput = TextInput::create(76.f, "0", "bigFont.fnt");
        m_offsetInput->setFilter("0123456789.");
        m_offsetInput->setMaxCharCount(5);
        m_offsetInput->setString(fmt::format("{}", g_offset));
        m_mainLayer->addChildAtPosition(m_offsetInput, Anchor::Center, CCPoint(86.f, -60.f));

        // ---- buttons ----
        auto applyBtn = CCMenuItemSpriteExtra::create(
            pillRoot("Apply", 128.f, 36.f, true), this, menu_selector(SpeedPopup::onApply));
        m_buttonMenu->addChildAtPosition(applyBtn, Anchor::Center, CCPoint(-68.f, -110.f));

        auto resetBtn = CCMenuItemSpriteExtra::create(
            pillRoot("Reset all", 128.f, 36.f, false), this, menu_selector(SpeedPopup::onReset));
        m_buttonMenu->addChildAtPosition(resetBtn, Anchor::Center, CCPoint(68.f, -110.f));

        // ---- little close button in the corner ----
        auto xRoot = CCNode::create();
        xRoot->setContentSize(CCSize(28.f, 28.f));
        xRoot->setAnchorPoint(CCPoint(0.5f, 0.5f));
        auto xd = CCDrawNode::create();
        xd->setPosition(CCPoint(14.f, 14.f));
        xd->drawDot(CCPoint(0.f, 0.f), 13.f, ccColor4F{1.f, 1.f, 1.f, 0.08f});
        xd->drawSegment(CCPoint(-4.5f, -4.5f), CCPoint(4.5f, 4.5f), 1.1f, ccColor4F{1.f, 1.f, 1.f, 0.85f});
        xd->drawSegment(CCPoint(-4.5f, 4.5f), CCPoint(4.5f, -4.5f), 1.1f, ccColor4F{1.f, 1.f, 1.f, 0.85f});
        xRoot->addChild(xd);
        auto xBtn = CCMenuItemSpriteExtra::create(xRoot, this, menu_selector(SpeedPopup::onCloseBtn));
        m_buttonMenu->addChildAtPosition(xBtn, Anchor::Center, CCPoint(124.f, 114.f));

        return true;
    }

    void onToggle(CCObject*) {
        g_enabled = !g_enabled;
        Mod::get()->setSavedValue<bool>("enabled", g_enabled);
        this->paintSwitch();
    }

    void onCloseBtn(CCObject*) {
        this->keyBackClicked();
    }

    void onApply(CCObject*) {
        float v = std::strtof(std::string(m_speedInput->getString()).c_str(), nullptr);
        if (v < 0.1f) v = 0.1f;
        if (v > 4.f)  v = 4.f;
        g_speed = v;
        Mod::get()->setSavedValue<float>("speed", v);

        float o = std::strtof(std::string(m_offsetInput->getString()).c_str(), nullptr);
        if (o < 0.f)   o = 0.f;
        if (o > 200.f) o = 200.f;
        g_offset = o;
        Mod::get()->setSavedValue<float>("offset", o);

        this->keyBackClicked(); // closes the menu
    }

    void onReset(CCObject*) {
        clearAllCharts();
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
        l->setScale(0.40f);
        l->runAction(CCScaleTo::create(0.1f, 0.32f));
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
            g_offset = Mod::get()->getSavedValue<float>("offset", 0.f);
            g_enabled = Mod::get()->getSavedValue<bool>("enabled", true);

            auto node = CCDrawNode::create();
            node->setPosition(CCPoint(MARGIN, MARGIN));
            node->setZOrder(1000);
            m_uiLayer->addChild(node);
            m_fields->draw = node;

            auto label = CCLabelBMFont::create("", "bigFont.fnt");
            label->setScale(0.32f);
            label->setOpacity(0);
            label->setPosition(CCPoint(MARGIN + CX, MARGIN + 8.f));
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
        float shift  = g_offset * 0.001f * g_vel; // timing offset in level units
        float gz     = GREEN_SEC * g_vel;

        // distance ahead -> how far up the highway (0 to 1)
        auto fOf = [&](float d) {
            float u = std::clamp(d / look, 0.f, 1.f);
            return (1.f + PERSP_K) * u / (1.f + PERSP_K * u);
        };
        auto yOfF = [&](float f) { return HIT_Y + f * yRange; };
        auto wOfF = [&](float f) { return LANE_W * (1.f - (1.f - FAR_SCALE) * f); };

        float y0 = HIT_Y - 16.f;
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

        // which bar is the next one you need to hit, and how close is it?
        int nextIdx = -1;
        for (size_t i = 0; i < notes.size(); i++) {
            if (g_state[i] == 0) { nextIdx = (int)i; break; }
        }
        float nextSoon = 0.f;
        if (nextIdx >= 0) {
            float dn = notes[nextIdx].start - px - shift;
            if (dn > -gz) nextSoon = 1.f - std::clamp(dn / (0.4f * g_vel), 0.f, 1.f);
        }

        // the bars
        for (size_t i = 0; i < notes.size(); i++) {
            float d0 = notes[i].start - px - shift; // distance to the press
            float d1 = notes[i].end - px - shift;   // distance to the release

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
                float nyb = yOfF(fOf(notes[i + 1].start - px - shift));
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

            // bars alternate cyan / pink so back-to-back clicks are easy to tell apart
            ccColor4F base = (i % 2 == 0) ? COL_A : COL_B;
            if (active) base = COL_ACTIVE;

            // the next bar to hit turns white and thick as it gets close
            float soon = ((int)i == nextIdx) ? nextSoon : 0.f;
            if (soon > 0.f) {
                base.r += (1.f - base.r) * soon * 0.75f;
                base.g += (1.f - base.g) * soon * 0.75f;
                base.b += (1.f - base.b) * soon * 0.75f;
            }

            if (active || soon > 0.3f) {
                ccColor4F glow = active ? COL_ACTIVE : base;
                quad(draw, CCPoint(CX - hwb - 3.f, yb - 2.f), CCPoint(CX + hwb + 3.f, yb - 2.f),
                           CCPoint(CX + hwt + 3.f, yt + 2.f), CCPoint(CX - hwt - 3.f, yt + 2.f),
                     withAlpha(glow, 0.20f * a));
            }

            // bar body: fill plus a bright outline
            quad(draw, CCPoint(CX - hwb, yb), CCPoint(CX + hwb, yb),
                       CCPoint(CX + hwt, yt), CCPoint(CX - hwt, yt),
                 withAlpha(base, (active ? 0.50f : 0.32f + 0.2f * soon) * a), 1.6f + soon, withAlpha(base, a));

            // bright cap on the bottom edge = the exact moment to click
            draw->drawSegment(CCPoint(CX - hwb, yb), CCPoint(CX + hwb, yb), 3.f + 2.f * soon, withAlpha(base, a));
        }

        // hit line flashes gold as the next click gets close
        if (nextSoon > 0.f) {
            quad(draw, CCPoint(CX - hw0, HIT_Y - 9.f), CCPoint(CX + hw0, HIT_Y - 9.f),
                       CCPoint(CX + hw0, HIT_Y + 9.f), CCPoint(CX - hw0, HIT_Y + 9.f),
                 withAlpha(COL_GOLD, 0.32f * nextSoon));
        }

        // hit zone glow (lights up while you hold)
        ccColor4F hitCol = g_holding ? COL_ACTIVE : COL_GOLD;
        if (g_holding) {
            quad(draw, CCPoint(CX - hw0, HIT_Y - 8.f), CCPoint(CX + hw0, HIT_Y - 8.f),
                       CCPoint(CX + hw0, HIT_Y + 8.f), CCPoint(CX - hw0, HIT_Y + 8.f),
                 withAlpha(COL_ACTIVE, 0.25f));
        }

        // hit bar
        draw->drawSegment(CCPoint(CX - hw0 - 3.f, HIT_Y), CCPoint(CX + hw0 + 3.f, HIT_Y), 2.4f, hitCol);

        // arrows pointing at the hit bar
        float lx = CX - hw0 - 14.f;
        float rx = CX + hw0 + 14.f;
        draw->drawSegment(CCPoint(lx - 5.f, HIT_Y + 10.f), CCPoint(lx + 5.f, HIT_Y), 1.8f, hitCol);
        draw->drawSegment(CCPoint(lx - 5.f, HIT_Y - 10.f), CCPoint(lx + 5.f, HIT_Y), 1.8f, hitCol);
        draw->drawSegment(CCPoint(rx + 5.f, HIT_Y + 10.f), CCPoint(rx - 5.f, HIT_Y), 1.8f, hitCol);
        draw->drawSegment(CCPoint(rx + 5.f, HIT_Y - 10.f), CCPoint(rx - 5.f, HIT_Y), 1.8f, hitCol);
    }
};
