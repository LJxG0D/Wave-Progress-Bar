// Wave Progress Bar
// Turns the in-level progress bar into a wave that zigzags along the bar,
// drawn with the player's equipped wave icon and colors.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Tweakable constants (adjust these if the wave does not line up with the bar
// on your GD version)
// ---------------------------------------------------------------------------

// Frame / file name of the progress bar texture, used to measure the bar.
static constexpr char const* BAR_TEXTURE_NAME = "GJ_progressBar_001.png";
// Fallback bar size, in points, if the texture can't be measured.
static constexpr float BAR_FALLBACK_WIDTH = 356.f;
static constexpr float BAR_FALLBACK_HEIGHT = 10.f;
// Horizontal padding inside the bar, in points, applied to both ends.
static constexpr float BAR_PADDING_X = 2.f;
// Fraction of the bar height the classic zigzag uses at "Zigzag Height" 1.0.
static constexpr float BAR_USABLE_HEIGHT = 0.8f;

// ---------------------------------------------------------------------------
// Settings snapshot (read once per level so the update loop stays cheap)
// ---------------------------------------------------------------------------

struct WaveBarSettings {
    bool hideFill = true;
    float iconScale = 0.3f;
    float zigzagHeight = 1.f;
    bool chaotic = false;
    float trailThickness = 1.6f;
    float trailLength = 1.f; // 0..1
    bool useSecondaryColor = false;

    static WaveBarSettings load() {
        auto mod = Mod::get();
        WaveBarSettings s;
        s.hideFill = mod->getSettingValue<bool>("hide-fill");
        s.iconScale = static_cast<float>(mod->getSettingValue<double>("icon-scale"));
        s.zigzagHeight = static_cast<float>(mod->getSettingValue<double>("zigzag-height"));
        s.chaotic = mod->getSettingValue<std::string>("zigzag-style") == "Chaotic";
        s.trailThickness = static_cast<float>(mod->getSettingValue<double>("trail-thickness"));
        s.trailLength = static_cast<float>(mod->getSettingValue<double>("trail-length")) / 100.f;
        s.useSecondaryColor = mod->getSettingValue<std::string>("trail-color") == "Secondary";
        return s;
    }
};

// ---------------------------------------------------------------------------
// WaveBar: a node that lives inside the progress bar and draws the wave
// ---------------------------------------------------------------------------

class WaveBar : public CCNode {
protected:
    CCDrawNode* m_trail = nullptr;
    SimplePlayer* m_icon = nullptr;

    WaveBarSettings m_settings;
    ccColor4F m_trailColor = {1.f, 1.f, 1.f, 1.f};

    float m_width = 0.f;  // usable width of the bar (local space)
    float m_height = 0.f; // height of the bar (local space)
    float m_startX = 0.f; // left edge of the usable area
    float m_yMin = 0.f;
    float m_yMax = 0.f;

    // Turning points of the zigzag. Every segment has a slope of exactly 1
    // (45 degrees), like a real wave. x is relative to m_startX.
    std::vector<CCPoint> m_path;

    bool init(float width, float height, uint32_t seed, WaveBarSettings const& settings) {
        if (!CCNode::init()) return false;

        m_settings = settings;
        m_height = height;
        m_startX = BAR_PADDING_X;
        m_width = std::max(1.f, width - BAR_PADDING_X * 2.f);

        float range = std::max(1.f, height * BAR_USABLE_HEIGHT * settings.zigzagHeight);
        m_yMin = height / 2.f - range / 2.f;
        m_yMax = height / 2.f + range / 2.f;

        this->buildPath(seed);

        auto gm = GameManager::get();

        // Trail color follows the player's icon colors.
        auto c = gm->colorForIdx(settings.useSecondaryColor ? gm->getPlayerColor2() : gm->getPlayerColor());
        m_trailColor = {c.r / 255.f, c.g / 255.f, c.b / 255.f, 1.f};

        m_trail = CCDrawNode::create();
        this->addChild(m_trail, 1);

        // The wave icon the player has equipped.
        m_icon = SimplePlayer::create(gm->getPlayerDart());
        m_icon->updatePlayerFrame(gm->getPlayerDart(), IconType::Wave);
        m_icon->setColor(gm->colorForIdx(gm->getPlayerColor()));
        m_icon->setSecondColor(gm->colorForIdx(gm->getPlayerColor2()));
        if (gm->getPlayerGlow()) {
            m_icon->setGlowOutline(gm->colorForIdx(gm->getPlayerGlowColor()));
        } else {
            m_icon->disableGlowOutline();
        }
        m_icon->setScale(settings.iconScale);
        this->addChild(m_icon, 2);

        this->setProgress(0.f);
        return true;
    }

    // Build the zigzag from x = 0 to a bit past the end of the bar.
    void buildPath(uint32_t seed) {
        float const range = m_yMax - m_yMin;
        float const minSegment = std::max(1.f, range * 0.25f);

        // Small deterministic generator, so a level always gets the same pattern.
        uint32_t state = seed * 2654435761u + 0x9E3779B9u;
        auto rnd = [&state]() {
            state = state * 1664525u + 1013904223u;
            return static_cast<float>(state >> 8) / 16777216.f;
        };

        float x = 0.f;
        float y = (m_yMin + m_yMax) / 2.f;
        int dir = rnd() < 0.5f ? 1 : -1;

        m_path.clear();
        m_path.push_back(ccp(x, y));

        while (x < m_width + range * 2.f) {
            float const maxLen = dir > 0 ? (m_yMax - y) : (y - m_yMin);
            float len = maxLen;

            if (m_settings.chaotic) {
                float const lo = std::min(minSegment, maxLen);
                len = lo + rnd() * (maxLen - lo);
            }
            len = std::max(len, 0.25f);

            x += len;
            y = std::clamp(y + dir * len, m_yMin, m_yMax);
            m_path.push_back(ccp(x, y));
            dir = -dir;
        }
    }

    // Index of the path segment containing x (segment i spans m_path[i]..m_path[i+1]).
    size_t segmentAt(float x) const {
        auto it = std::upper_bound(
            m_path.begin(), m_path.end(), x,
            [](float v, CCPoint const& p) { return v < p.x; }
        );
        size_t idx = (it == m_path.begin()) ? 0 : static_cast<size_t>(it - m_path.begin()) - 1;
        return std::min(idx, m_path.size() - 2);
    }

    float yAt(float x) const {
        size_t i = this->segmentAt(x);
        auto const& a = m_path[i];
        auto const& b = m_path[i + 1];
        float t = (b.x - a.x) > 0.0001f ? (x - a.x) / (b.x - a.x) : 0.f;
        return a.y + (b.y - a.y) * std::clamp(t, 0.f, 1.f);
    }

public:
    static WaveBar* create(float width, float height, uint32_t seed, WaveBarSettings const& settings) {
        auto ret = new WaveBar();
        if (ret && ret->init(width, height, seed, settings)) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    // progress: 0..1
    void setProgress(float progress) {
        progress = std::clamp(progress, 0.f, 1.f);

        float const x = progress * m_width;
        float const startX = m_settings.trailLength >= 0.999f
            ? 0.f
            : std::max(0.f, x - m_settings.trailLength * m_width);

        // Collect the trail polyline: start point, every turn in between, current point.
        std::vector<CCPoint> pts;
        pts.reserve(m_path.size());
        pts.push_back(ccp(startX, this->yAt(startX)));
        for (auto const& v : m_path) {
            if (v.x > startX && v.x < x) pts.push_back(v);
        }
        pts.push_back(ccp(x, this->yAt(x)));

        m_trail->clear();
        float const radius = m_settings.trailThickness / 2.f;
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            auto a = ccp(pts[i].x + m_startX, pts[i].y);
            auto b = ccp(pts[i + 1].x + m_startX, pts[i + 1].y);
            m_trail->drawSegment(a, b, radius, m_trailColor);
        }

        // The wave icon sits at the head of the trail and points along the current
        // 45 degree direction (up = rotate counter-clockwise, down = clockwise).
        auto const& head = pts.back();
        size_t seg = this->segmentAt(x);
        bool goingUp = m_path[seg + 1].y > m_path[seg].y;

        m_icon->setPosition(ccp(head.x + m_startX, head.y));
        m_icon->setRotation(goingUp ? -45.f : 45.f);
    }
};

// ---------------------------------------------------------------------------
// PlayLayer hook
// ---------------------------------------------------------------------------

class $modify(WaveProgressPlayLayer, PlayLayer) {
    struct Fields {
        WaveBar* m_wave = nullptr;
    };

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();

        // Platformer levels don't have a progress bar.
        if (!m_progressBar || (m_level && m_level->isPlatformer())) return;

        auto settings = WaveBarSettings::load();

        // Measure the bar from its texture.
        CCSize size = CCSize(BAR_FALLBACK_WIDTH, BAR_FALLBACK_HEIGHT);
        if (auto frame = CCSpriteFrameCache::get()->spriteFrameByName(BAR_TEXTURE_NAME)) {
            size = frame->getOriginalSize();
        } else if (auto tex = CCSprite::create(BAR_TEXTURE_NAME)) {
            size = tex->getContentSize();
        }
        if (size.width < 1.f || size.height < 1.f) {
            size = CCSize(BAR_FALLBACK_WIDTH, BAR_FALLBACK_HEIGHT);
        }

        uint32_t seed = m_level ? static_cast<uint32_t>(m_level->m_levelID.value()) : 0u;

        auto wave = WaveBar::create(size.width, size.height, seed, settings);
        if (!wave) return;

        wave->setID("wave-bar"_spr);
        m_progressBar->addChild(wave, 10);
        m_fields->m_wave = wave;

        if (settings.hideFill) {
            m_progressBar->setOpacity(0);
        }
    }

    void updateProgressbar() {
        PlayLayer::updateProgressbar();

        if (m_fields->m_wave) {
            m_fields->m_wave->setProgress(this->getCurrentPercent() / 100.f);
        }
    }
};
