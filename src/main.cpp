// Wave Progress Bar
// Turns the in-level progress bar into a wave that zigzags along the bar,
// drawn with the player's equipped wave icon and colors.
//
// How it works: the wave measures where the progress bar fill really is on
// screen every frame (world space) and draws next to it, so it doesn't matter
// how GD scales the bar internally. The original bar (track + fill) is faded
// out, and the trail is drawn in three passes like the vanilla wave trail:
// dark outline, colored body, thin white core.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <typeinfo>
#include <vector>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Tweakable constants
// ---------------------------------------------------------------------------

// Fraction of the bar height the zigzag spans at "Zigzag Height" 1.0.
static constexpr float BAR_USABLE_HEIGHT = 0.8f;
// Wave icon is roughly this many points tall at scale 1.0.
static constexpr float ICON_NATIVE_SIZE = 30.f;
// Icon height compared to bar height at "Wave Icon Scale" 1.0.
static constexpr float ICON_BAR_RATIO = 1.1f;
// Trail width compared to bar height at "Trail Thickness" 1.0.
static constexpr float TRAIL_BAR_RATIO = 0.18f;
// Outline width (each side) compared to bar height at "Outline Thickness" 1.0.
static constexpr float OUTLINE_BAR_RATIO = 0.06f;
// Width of the white core compared to the colored trail.
static constexpr float CORE_RATIO = 0.4f;

// ---------------------------------------------------------------------------
// Settings snapshot (read once per level so the update loop stays cheap)
// ---------------------------------------------------------------------------

struct WaveBarSettings {
    bool hideOriginal = true;
    float iconScale = 1.f;
    float zigzagHeight = 1.f;
    bool chaotic = false;
    float trailThickness = 2.f;
    float trailLength = 1.f; // 0..1 of the bar
    bool useSecondaryColor = false;
    bool outline = true;
    float outlineThickness = 1.f;
    bool whiteCore = true;

    static WaveBarSettings load() {
        auto mod = Mod::get();
        WaveBarSettings s;
        s.hideOriginal = mod->getSettingValue<bool>("hide-fill");
        s.iconScale = static_cast<float>(mod->getSettingValue<double>("icon-scale"));
        s.zigzagHeight = static_cast<float>(mod->getSettingValue<double>("zigzag-height"));
        s.chaotic = mod->getSettingValue<std::string>("zigzag-style") == "Chaotic";
        s.trailThickness = static_cast<float>(mod->getSettingValue<double>("trail-thickness"));
        s.trailLength = static_cast<float>(mod->getSettingValue<double>("trail-length")) / 100.f;
        s.useSecondaryColor = mod->getSettingValue<std::string>("trail-color") == "Secondary";
        s.outline = mod->getSettingValue<bool>("trail-outline");
        s.outlineThickness = static_cast<float>(mod->getSettingValue<double>("outline-thickness"));
        s.whiteCore = mod->getSettingValue<bool>("white-core");
        return s;
    }
};

// ---------------------------------------------------------------------------
// WaveBar: sits next to the progress bar and draws the wave on top of it
// ---------------------------------------------------------------------------

class WaveBar : public CCNode {
protected:
    enum class Mode { Unknown, Fill, Track };

    // Once we have worked out how the bar behaves, remember it for later levels
    // so the wave shows up instantly.
    inline static Mode s_knownMode = Mode::Unknown;

    CCNode* m_ref = nullptr; // the node GD resizes with progress (m_progressBar)

    CCDrawNode* m_trail = nullptr;
    SimplePlayer* m_icon = nullptr;

    WaveBarSettings m_settings;
    ccColor4F m_trailColor = {1.f, 1.f, 1.f, 1.f};

    // Is m_ref the growing fill (Fill), or the full bar (Track)? Figured out at
    // runtime by watching whether its width changes with progress.
    Mode m_mode = Mode::Unknown;
    bool m_haveSample = false;
    float m_sampleP = 0.f;
    float m_sampleW = 0.f;
    bool m_originalHidden = false;

    // Zigzag turning points in screen units: x = distance along the bar,
    // y = offset from the bar's center line. Every segment has slope 1 (45 deg).
    std::vector<CCPoint> m_path;
    float m_range = 0.f; // total height of the zigzag
    uint32_t m_seed = 0;
    uint32_t m_state = 0;
    int m_dir = 1;
    float m_curY = 0.f;

    bool m_loggedFrame = false;

    bool init(CCNode* ref, uint32_t seed, WaveBarSettings const& settings) {
        if (!CCNode::init()) return false;

        m_ref = ref;
        m_settings = settings;
        m_seed = seed;
        m_mode = s_knownMode;

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
        this->addChild(m_icon, 2);

        // Nothing is shown until we know how the bar behaves.
        this->setVisible(m_mode != Mode::Unknown);

        auto size = ref->getContentSize();
        auto parent = ref->getParent();
        log::info(
            "[WaveBar] fill type={} content={}x{} scale=({}, {}) anchor=({}, {}) pos=({}, {}) parent={} children={} knownMode={}",
            typeid(*ref).name(), size.width, size.height, ref->getScaleX(), ref->getScaleY(),
            ref->getAnchorPoint().x, ref->getAnchorPoint().y, ref->getPositionX(), ref->getPositionY(),
            parent ? typeid(*parent).name() : "none", ref->getChildrenCount(), static_cast<int>(m_mode)
        );
        return true;
    }

    // ----- hiding the original bar -----------------------------------------

    bool isAncestorOfThis(CCNode* n) {
        for (CCNode* p = this->getParent(); p; p = p->getParent()) {
            if (p == n) return true;
        }
        return false;
    }

    // Is this node shaped and placed like the progress bar (long, thin, and
    // sitting on top of the fill)?
    bool looksLikeBar(CCNode* n, CCPoint const& fillOrigin, float fillHeight) const {
        auto const s = n->getContentSize();
        if (s.width < 1.f || s.height < 1.f) return false;

        CCPoint const o = n->convertToWorldSpace(ccp(0.f, 0.f));
        CCPoint const r = n->convertToWorldSpace(ccp(s.width, 0.f));
        CCPoint const t = n->convertToWorldSpace(ccp(0.f, s.height));
        float const w = r.x - o.x;
        float const h = t.y - o.y;
        if (w <= 0.f || h <= 0.f) return false;
        if (w < h * 6.f) return false;          // bars are much wider than tall
        if (h > fillHeight * 4.f) return false; // ...and not much taller than the fill

        float const centerY = fillOrigin.y + fillHeight / 2.f;
        if (centerY < o.y - 1.f || centerY > o.y + h + 1.f) return false;
        if (fillOrigin.x < o.x - 2.f || fillOrigin.x > o.x + w + 2.f) return false;
        return true;
    }

    // Make a node and everything inside it transparent. Doesn't touch labels
    // (like the percentage text), and never touches our own wave.
    void fadeOut(CCNode* n) {
        if (!n || n == this || this->isAncestorOfThis(n)) return;
        if (typeinfo_cast<CCLabelBMFont*>(n)) return;

        if (auto rgba = typeinfo_cast<CCNodeRGBA*>(n)) {
            rgba->setOpacity(0);
        }
        if (auto children = n->getChildren()) {
            for (auto child : CCArrayExt<CCNode*>(children)) {
                this->fadeOut(child);
            }
        }
    }

    void hideOriginalBar(CCPoint const& fillOrigin, float fillHeight) {
        std::vector<CCNode*> candidates;
        candidates.push_back(m_ref);

        // The fill's ancestors (the track is often its parent), and the
        // siblings of the fill and of its parent (the track can also sit next to it).
        CCNode* a = m_ref->getParent();
        for (int i = 0; a && i < 3; ++i, a = a->getParent()) {
            candidates.push_back(a);
            if (i == 0) {
                if (auto kids = a->getChildren()) {
                    for (auto k : CCArrayExt<CCNode*>(kids)) candidates.push_back(k);
                }
            }
            if (i == 1) {
                if (auto kids = a->getChildren()) {
                    for (auto k : CCArrayExt<CCNode*>(kids)) candidates.push_back(k);
                }
            }
        }

        int hidden = 0;
        for (auto n : candidates) {
            if (n == this || this->isAncestorOfThis(n)) continue;
            if (n != m_ref && !this->looksLikeBar(n, fillOrigin, fillHeight)) continue;
            this->fadeOut(n);
            ++hidden;
        }
        log::info("[WaveBar] faded out {} node(s) of the original bar", hidden);
    }

    // ----- zigzag path ------------------------------------------------------

    float rnd() {
        m_state = m_state * 1664525u + 1013904223u;
        return static_cast<float>(m_state >> 8) / 16777216.f;
    }

    void startPath(float range) {
        m_range = std::max(range, 1.f);
        m_state = m_seed * 2654435761u + 0x9E3779B9u;
        m_curY = 0.f;
        m_dir = this->rnd() < 0.5f ? 1 : -1;
        m_path.clear();
        m_path.push_back(ccp(0.f, m_curY));
    }

    // Make sure the path reaches past distance d.
    void extendTo(float d) {
        float const yMax = m_range / 2.f;
        float const yMin = -m_range / 2.f;
        float const minSegment = std::max(1.f, m_range * 0.25f);

        while (m_path.back().x < d + m_range * 2.f) {
            float const maxLen = m_dir > 0 ? (yMax - m_curY) : (m_curY - yMin);
            float len = maxLen;

            if (m_settings.chaotic) {
                float const lo = std::min(minSegment, maxLen);
                len = lo + this->rnd() * (maxLen - lo);
            }
            len = std::max(len, 0.25f);

            float const x = m_path.back().x + len;
            m_curY = std::clamp(m_curY + m_dir * len, yMin, yMax);
            m_path.push_back(ccp(x, m_curY));
            m_dir = -m_dir;
        }
    }

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
    static WaveBar* create(CCNode* ref, uint32_t seed, WaveBarSettings const& settings) {
        auto ret = new WaveBar();
        if (ret && ret->init(ref, seed, settings)) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    // progress: 0..1
    void setProgress(float progress) {
        if (!m_ref || !this->getParent()) return;
        progress = std::clamp(progress, 0.f, 1.f);

        // Where is the fill on screen right now? (world space, so GD's own
        // scaling of the sprite doesn't matter)
        auto const size = m_ref->getContentSize();
        CCPoint const origin = m_ref->convertToWorldSpace(ccp(0.f, 0.f));
        CCPoint const right = m_ref->convertToWorldSpace(ccp(size.width, 0.f));
        CCPoint const top = m_ref->convertToWorldSpace(ccp(0.f, size.height));

        float const fillWidth = right.x - origin.x;
        float const barHeight = top.y - origin.y;
        if (barHeight < 0.01f) return; // not laid out yet

        // How many world units is one unit of our own coordinate space?
        CCPoint const w0 = this->convertToWorldSpace(ccp(0.f, 0.f));
        CCPoint const w1 = this->convertToWorldSpace(ccp(1.f, 0.f));
        float const unit = std::max(0.0001f, std::hypot(w1.x - w0.x, w1.y - w0.y));

        if (!m_loggedFrame) {
            m_loggedFrame = true;
            log::info(
                "[WaveBar] first frame: progress={} fillWidth(world)={} barHeight(world)={} unit={}",
                progress, fillWidth, barHeight, unit
            );
        }

        // Decide whether the node we watch is the growing fill or the whole bar.
        if (m_mode == Mode::Unknown) {
            if (progress < 0.02f) return;
            if (!m_haveSample) {
                m_haveSample = true;
                m_sampleP = progress;
                m_sampleW = fillWidth;
                return;
            }
            if (std::abs(progress - m_sampleP) < 0.03f) return;

            float const tolerance = std::max(0.5f, 0.03f * std::max(fillWidth, m_sampleW));
            m_mode = std::abs(fillWidth - m_sampleW) > tolerance ? Mode::Fill : Mode::Track;
            s_knownMode = m_mode;
            log::info(
                "[WaveBar] mode = {} (sample p={} w={}, now p={} w={})",
                m_mode == Mode::Fill ? "Fill" : "Track", m_sampleP, m_sampleW, progress, fillWidth
            );
            this->setVisible(true);
        }

        // Fade out the original bar (track and fill) once we know where it is.
        if (!m_originalHidden) {
            m_originalHidden = true;
            if (m_settings.hideOriginal) {
                this->hideOriginalBar(origin, barHeight);
            }
        }

        float const centerY = origin.y + barHeight / 2.f;
        if (m_range <= 0.f) {
            this->startPath(barHeight * BAR_USABLE_HEIGHT * m_settings.zigzagHeight);
        }

        // Where is the head of the wave, measured along the bar from its left edge?
        float head = 0.f;
        float totalWidth = 0.f;
        if (m_mode == Mode::Fill) {
            head = fillWidth;
            totalWidth = progress > 0.005f ? fillWidth / progress : fillWidth;
        } else {
            head = progress * fillWidth;
            totalWidth = fillWidth;
        }
        head = std::max(0.f, head);

        float const startD = m_settings.trailLength >= 0.999f
            ? 0.f
            : std::max(0.f, head - m_settings.trailLength * totalWidth);

        this->extendTo(head);

        // Collect the trail polyline in world units: start, every turn, head.
        std::vector<CCPoint> pts;
        pts.reserve(m_path.size());
        pts.push_back(ccp(startD, this->yAt(startD)));
        for (auto const& v : m_path) {
            if (v.x > startD && v.x < head) pts.push_back(v);
        }
        pts.push_back(ccp(head, this->yAt(head)));

        std::vector<CCPoint> local;
        local.reserve(pts.size());
        for (auto const& p : pts) {
            local.push_back(this->convertToNodeSpace(ccp(origin.x + p.x, centerY + p.y)));
        }

        // Three passes, like the vanilla wave trail: dark outline, colored body,
        // thin white core. Each pass is drawn over the previous one.
        float const trailRadius = (barHeight * TRAIL_BAR_RATIO * m_settings.trailThickness / 2.f) / unit;
        float const outlineRadius =
            trailRadius + (barHeight * OUTLINE_BAR_RATIO * m_settings.outlineThickness) / unit;
        float const coreRadius = trailRadius * CORE_RATIO;

        auto drawPass = [&](float radius, ccColor4F const& color) {
            for (size_t i = 0; i + 1 < local.size(); ++i) {
                m_trail->drawSegment(local[i], local[i + 1], radius, color);
            }
        };

        m_trail->clear();
        if (m_settings.outline) drawPass(outlineRadius, ccc4f(0.f, 0.f, 0.f, 1.f));
        drawPass(trailRadius, m_trailColor);
        if (m_settings.whiteCore) drawPass(coreRadius, ccc4f(1.f, 1.f, 1.f, 1.f));

        // Wave icon: sized relative to the bar, pointing along the current direction.
        float const iconScale =
            (barHeight * ICON_BAR_RATIO / ICON_NATIVE_SIZE) * m_settings.iconScale / unit;
        size_t const seg = this->segmentAt(head);
        bool const goingUp = m_path[seg + 1].y > m_path[seg].y;

        m_icon->setScale(iconScale);
        m_icon->setPosition(local.back());
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

        CCNode* fill = m_progressBar;
        CCNode* parent = fill->getParent();
        if (!parent) return;

        // "Show Progress Bar" is off in the GD settings: leave everything alone.
        if (!fill->isVisible() || !parent->isVisible()) return;

        // Put the wave next to the bar's track, not inside it, so that fading
        // the original bar out never affects the wave.
        CCNode* host = parent;
        if (typeinfo_cast<CCNodeRGBA*>(parent) && parent->getParent()) {
            host = parent->getParent();
        }

        // Draw above whatever branch of the host the original bar lives in.
        CCNode* branch = fill;
        while (branch->getParent() && branch->getParent() != host) {
            branch = branch->getParent();
        }

        uint32_t seed = m_level ? static_cast<uint32_t>(m_level->m_levelID.value()) : 0u;

        auto wave = WaveBar::create(fill, seed, WaveBarSettings::load());
        if (!wave) return;

        wave->setID("wave-bar"_spr);
        host->addChild(wave, branch->getZOrder() + 10);
        m_fields->m_wave = wave;
    }

    void updateProgressbar() {
        PlayLayer::updateProgressbar();

        if (m_fields->m_wave) {
            m_fields->m_wave->setProgress(this->getCurrentPercent() / 100.f);
        }
    }
};
