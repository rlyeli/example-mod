#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <cmath>

using namespace geode::prelude;

// ---- Settings you can tweak ----
const float LOOKAHEAD  = 300.f; // how far ahead bars appear (bigger = slower bars, more warning)
const float PANEL_W    = 56.f;  // width of the highway
const float PANEL_H    = 150.f; // height of the highway
const float HIT_LINE_Y = 24.f;  // where the "click now" line sits (from panel bottom)
const float NOTE_R     = 8.f;   // thickness of the bars (radius)
const float HIT_WINDOW = 30.f;  // bars turn green when this close

// Colors
const ccColor4F COL_PANEL  = {0.05f, 0.05f, 0.10f, 0.60f};
const ccColor4F COL_LINE   = {1.f, 1.f, 1.f, 0.85f};
const ccColor4F COL_NOTE   = {0.25f, 0.85f, 1.f, 1.f};   // cyan
const ccColor4F COL_ACTIVE = {0.45f, 1.f, 0.40f, 1.f};   // lime

// A note = one click. start = where you pressed, end = where you let go.
struct Note {
    float start;
    float end; // -1 while still held
};

// One saved chart PER LEVEL (the number is the level's ID)
static std::map<int, std::vector<Note>> g_savedClicks;
// Clicks from the current attempt
static std::vector<Note> g_currentClicks;

// PART 1: record presses and releases
class $modify(RecordLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (!m_player1) return;
        float x = m_player1->getPositionX();
        if (down) {
            g_currentClicks.push_back({x, -1.f});
        } else {
            // close the most recent note that is still open
            for (auto it = g_currentClicks.rbegin(); it != g_currentClicks.rend(); ++it) {
                if (it->end < 0.f) {
                    it->end = x;
                    break;
                }
            }
        }
    }
};

// helper: filled rounded rectangle
static void roundedRect(CCDrawNode* n, float x, float y, float w, float h, float r, ccColor4F c) {
    const float PI = 3.14159265f;
    const int seg = 6;
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
    n->drawPolygon(pts.data(), pts.size(), c, 0.f, c);
}

// PART 2: draw the highway
class $modify(HighwayLayer, PlayLayer) {
    struct Fields {
        CCDrawNode* draw = nullptr;
    };

    void resetLevel() {
        PlayLayer::resetLevel();
        g_currentClicks.clear();
    }

    // Finishing the level saves this run as the chart for THIS level only
    void levelComplete() {
        PlayLayer::levelComplete();
        // close a note that is still being held at the finish
        for (auto& n : g_currentClicks) {
            if (n.end < 0.f) n.end = m_player1->getPositionX();
        }
        int id = m_level->m_levelID.value();
        g_savedClicks[id] = g_currentClicks;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        // create the drawing area once
        if (!m_fields->draw) {
            auto node = CCDrawNode::create();
            node->setPosition({10.f, 10.f}); // bottom-left corner
            node->setZOrder(1000);
            m_uiLayer->addChild(node);
            m_fields->draw = node;
        }

        auto draw = m_fields->draw;
        draw->clear();

        // only draw if this level has a saved chart
        int id = m_level->m_levelID.value();
        auto it = g_savedClicks.find(id);
        if (it == g_savedClicks.end()) return;

        // rounded panel
        roundedRect(draw, 0, 0, PANEL_W, PANEL_H, 10.f, COL_PANEL);

        // hit line (smooth rounded line)
        draw->drawSegment({6.f, HIT_LINE_Y}, {PANEL_W - 6.f, HIT_LINE_Y}, 1.5f, COL_LINE);

        // distance ahead -> height on the panel
        float topY = PANEL_H - 2.f * NOTE_R - 6.f;
        auto yOf = [&](float d) {
            if (d < 0.f) d = 0.f;
            if (d > LOOKAHEAD) d = LOOKAHEAD;
            return HIT_LINE_Y + (d / LOOKAHEAD) * (topY - HIT_LINE_Y);
        };

        float playerX = m_player1->getPositionX();
        float cx = PANEL_W / 2.f;

        for (auto const& note : it->second) {
            float d0 = note.start - playerX; // distance to the press
            float d1 = note.end   - playerX; // distance to the release

            if (d1 < -5.f) continue;      // already finished
            if (d0 > LOOKAHEAD) continue; // too far ahead

            // bar goes from the press up to the release
            float yBottom = yOf(d0);
            float yTop    = yOf(d1);

            CCPoint a = {cx, yBottom + NOTE_R};
            CCPoint b = {cx, std::max(yTop + NOTE_R, yBottom + NOTE_R + 1.f)};

            bool active = (d0 < HIT_WINDOW);
            draw->drawSegment(a, b, NOTE_R, active ? COL_ACTIVE : COL_NOTE);
        }
    }
};
