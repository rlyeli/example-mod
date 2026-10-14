#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>

using namespace geode::prelude;

// ---- Settings you can tweak ----
const float LOOKAHEAD  = 150.f; // how far ahead bars appear (bigger = more warning)
const float PANEL_W    = 50.f;  // width of the little highway
const float PANEL_H    = 130.f; // height of the little highway
const float HIT_LINE_Y = 20.f;  // where the "click now" line sits (from panel bottom)

// Saved clicks from your last completed run
static std::vector<float> g_savedClicks;
// Clicks from the current attempt
static std::vector<float> g_currentClicks;

// PART 1: record where you click
class $modify(RecordLayer, GJBaseGameLayer) {
    void pushButton(int button, bool isPlayer1) {
        GJBaseGameLayer::pushButton(button, isPlayer1);
        if (m_player1) g_currentClicks.push_back(m_player1->getPositionX());
    }
};

// PART 2: draw the highway
class $modify(HighwayLayer, PlayLayer) {
    struct Fields {
        CCDrawNode* draw = nullptr;
    };

    void resetLevel() {
        PlayLayer::resetLevel();
        g_currentClicks.clear();
    }

    // Finishing the level saves this run as the "chart"
    void levelComplete() {
        PlayLayer::levelComplete();
        g_savedClicks = g_currentClicks;
    }

    // helper: draw a filled rectangle
    void rect(float x, float y, float w, float h, ccColor4F color) {
        CCPoint pts[4] = { {x, y}, {x + w, y}, {x + w, y + h}, {x, y + h} };
        m_fields->draw->drawPolygon(pts, 4, color, 0.f, color);
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

        // erase last frame's drawing
        m_fields->draw->clear();

        // dark background panel
        rect(0, 0, PANEL_W, PANEL_H, {0.f, 0.f, 0.f, 0.5f});

        // the hit line
        rect(0, HIT_LINE_Y, PANEL_W, 3.f, {1.f, 1.f, 1.f, 0.9f});

        // draw a bar for every upcoming saved click
        float playerX = m_player1->getPositionX();
        for (float clickX : g_savedClicks) {
            float distance = clickX - playerX;
            if (distance < -10.f || distance > LOOKAHEAD) continue;

            // far away = top of panel, about to click = on the hit line
            float t = distance / LOOKAHEAD;
            float y = HIT_LINE_Y + t * (PANEL_H - HIT_LINE_Y - 10.f);

            // bars turn green right when you should click
            ccColor4F color = (distance < 15.f)
                ? ccColor4F{0.2f, 1.f, 0.3f, 1.f}
                : ccColor4F{1.f, 0.8f, 0.2f, 1.f};

            rect(5.f, y, PANEL_W - 10.f, 6.f, color);
        }
    }
};
