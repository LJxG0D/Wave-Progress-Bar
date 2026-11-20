// Wave Progress Bar
// Turns the in-level progress bar into a wave that zigzags along the bar,
// drawn with the player's equipped wave icon and colors.
//
// How it works: the wave measures where the progress bar fill really is on
// screen every frame (world space) and draws next to it, so it doesn't matter
// how GD scales the bar internally. The original bar (track + fill) is faded
// out. The whole zigzag path is drawn ahead of time as an empty, transparent
// track with a black / white / black outline, and it fills up with the wave
// trail as the level progresses. The wave icon is drawn on top of everything.
//
// The path is drawn with exact polygons (a rectangle per segment and a mitered
// wedge at every turn) instead of GD's rounded line segments, so the corners
// stay clean at any slope, including the steeper mini wave.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <typeinfo>
#include <vector>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Tweakable constants (all sizes are relative to the height of the bar)
// ---------------------------------------------------------------------------

// Fraction of the bar height the zigzag spans at "Zigzag Height" 1.0.
static constexpr float BAR_USABLE_HEIGHT = 0.8f;
// Wave icon is roughly this many points tall at scale 1.0.
static constexpr float ICON_NATIVE_SIZE = 30.f;
// Icon height compared to bar height at "Wave Icon Scale" 1.0.
static constexpr float ICON_BAR_RATIO = 1.1f;
// Trail width compared to bar height at "Trail Thickness" 1.0.
static constexpr float TRAIL_BAR_RATIO = 0.18f;
// Width of the white core compared to the colored trail.
static constexpr float CORE_RATIO = 0.4f;
// Outline rings around the path, from the trail outwards: black, white, black.
static constexpr float RING_BLACK = 0.04f;
static constexpr float RING_WHITE = 0.05f;
// Normal wave moves at 45 degrees (slope 1), the mini wave twice as steeply.
static constexpr float NORMAL_SLOPE = 1.f;
static constexpr float MINI_SLOPE = 2.f;
// The mini wave and its trail are smaller.
static constexpr float MINI_SCALE = 0.6f;
// The wave is moved down so it never goes past the top of the screen.
static constexpr float SCREEN_MARGIN = 1.f;
// Progress after which we trust the fill width to measure the full bar width.
static constexpr float WIDTH_LOCK_PROGRESS = 0.3f;
// Corners sharper than this (ratio of miter length to line half-width) are
// cut flat instead of pointed. The mini wave is about 2.2, so this keeps it pointed.
static constexpr float MITER_LIMIT = 4.f;
// Paths are drawn this many times larger and the node is scaled back down.
// This keeps the anti-aliased edges of CCDrawNode polygons thin and crisp.
static constexpr float DRAW_SCALE = 4.f;

static constexpr float PI_F = 3.14159265358979f;

// ---------------------------------------------------------------------------
// Polygon helpers
// ---------------------------------------------------------------------------

// Fill a convex polygon (points in local units).
static void fillPolygon(CCDrawNode* node, std::vector<CCPoint> poly, ccColor4F const& color) {
    if (poly.size() < 3) return;

    // Make the winding counter-clockwise, skip degenerate shapes.
    float area = 0.f;
    for (size_t i = 0; i < poly.size(); ++i) {
        auto const& a = poly[i];
        auto const& b = poly[(i + 1) % poly.size()];
        area += a.x * b.y - b.x * a.y;
    }
    if (std::abs(area) < 1e-6f) return;
    if (area < 0.f) std::reverse(poly.begin(), poly.end());

    for (auto& p : poly) {
        p.x *= DRAW_SCALE;
        p.y *= DRAW_SCALE;
    }
    node->drawPolygon(poly.data(), static_cast<unsigned int>(poly.size()), color, 0.f, ccc4f(0.f, 0.f, 0.f, 0.f));
}

// Fill a circle (approximated with a many-sided polygon).
static void fillCircle(CCDrawNode* node, CCPoint const& center, float radius, ccColor4F const& color) {
    if (radius <= 0.f) return;
    constexpr int SIDES = 40;
    std::vector<CCPoint> poly;
    poly.reserve(SIDES);
    for (int i = 0; i < SIDES; ++i) {
        float const a = 2.f * PI_F * static_cast<float>(i) / static_cast<float>(SIDES);
        poly.push_back(ccp(center.x + std::cos(a) * radius, center.y + std::sin(a) * radius));
    }
    fillPolygon(node, poly, color);
}

// Draw a thick line along a polyline: a rectangle for every segment, plus a
// mitered wedge on the outside of every turn. Overlaps are harmless because
// everything is one solid color. The ends can be rounded off with a circle.
static void strokePolyline(
    CCDrawNode* node, std::vector<CCPoint> const& pts, float radius, ccColor4F const& color,
    bool capStart = false, bool capEnd = false
) {
    if (pts.size() < 2 || radius <= 0.f) return;

    if (capStart) fillCircle(node, pts.front(), radius, color);
    if (capEnd) fillCircle(node, pts.back(), radius, color);

    size_t const n = pts.size();
    std::vector<CCPoint> dir(n - 1);
    std::vector<bool> valid(n - 1, false);

    for (size_t i = 0; i + 1 < n; ++i) {
        float dx = pts[i + 1].x - pts[i].x;
        float dy = pts[i + 1].y - pts[i].y;
        float len = std::hypot(dx, dy);
        if (len < 1e-5f) continue;
        dir[i] = ccp(dx / len, dy / len);
        valid[i] = true;

        CCPoint nrm = ccp(-dir[i].y, dir[i].x);
        fillPolygon(node, {
            ccp(pts[i].x + nrm.x * radius, pts[i].y + nrm.y * radius),
            ccp(pts[i + 1].x + nrm.x * radius, pts[i + 1].y + nrm.y * radius),
            ccp(pts[i + 1].x - nrm.x * radius, pts[i + 1].y - nrm.y * radius),
            ccp(pts[i].x - nrm.x * radius, pts[i].y - nrm.y * radius),
        }, color);
    }

    // Joints
    for (size_t k = 1; k + 1 < n; ++k) {
        if (!valid[k - 1] || !valid[k]) continue;

        CCPoint const& d0 = dir[k - 1];
        CCPoint const& d1 = dir[k];
        float const cross = d0.x * d1.y - d0.y * d1.x;
        if (std::abs(cross) < 1e-4f) continue; // straight, nothing to fill

        // The gap opens on the outside of the turn.
        float const side = cross > 0.f ? -1.f : 1.f;
        CCPoint const n0 = ccp(-d0.y, d0.x);
        CCPoint const n1 = ccp(-d1.y, d1.x);
        CCPoint const p = pts[k];

        CCPoint const a = ccp(p.x + side * n0.x * radius, p.y + side * n0.y * radius);
        CCPoint const b = ccp(p.x + side * n1.x * radius, p.y + side * n1.y * radius);

        float const denom = 1.f + (n0.x * n1.x + n0.y * n1.y);
        if (denom > 0.05f) {
            CCPoint const m = ccp((n0.x + n1.x) / denom, (n0.y + n1.y) / denom);
            float const ratio = std::hypot(m.x, m.y);
            if (ratio <= MITER_LIMIT) {
                CCPoint const tip = ccp(p.x + side * m.x * radius, p.y + side * m.y * radius);
                fillPolygon(node, {p, a, tip, b}, color);
                continue;
            }
        }
        fillPolygon(node, {p, a, b}, color); // too sharp: cut the corner flat
    }
}

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
    bool pathOutline = true;
    bool solidTrail = false;
    bool miniWave = false;
    float verticalOffset = 0.f;

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
        s.pathOutline = mod->getSettingValue<bool>("trail-outline");
        s.solidTrail = mod->getSettingValue<bool>("solid-trail");
        s.miniWave = mod->getSettingValue<bool>("mini-wave");
        s.verticalOffset = static_cast<float>(mod->getSettingValue<double>("vertical-offset"));
        return s;
    }
};

// ---------------------------------------------------------------------------
// WaveBar: sits next to the progress bar and draws the wave on top of it
// ---------------------------------------------------------------------------

class WaveBar : public CCNode {
protected:
    enum class Mode { Unknown, Fill, Track };

    // Things we learn about the bar are remembered for later levels, so the
    // wave shows up instantly and the path doesn't change size mid-level.
    inline static Mode s_knownMode = Mode::Unknown;
    inline static float s_widthRatio = 0.f; // full bar width / bar height

    CCNode* m_ref = nullptr; // the node GD resizes with progress (m_progressBar)

    // Layers, bottom to top:
    //  m_clip    cuts the inside of the path out of the outline (transparent middle)
    //   m_track  black / white / black outline of the whole path
    //  m_trail   the filled part of the path
    //  m_icon    the wave icon
    CCClippingNode* m_clip = nullptr;
    CCDrawNode* m_stencil = nullptr;
    CCDrawNode* m_track = nullptr;
    CCDrawNode* m_trail = nullptr;
    SimplePlayer* m_icon = nullptr;

    WaveBarSettings m_settings;
    ccColor4F m_trailColor = {1.f, 1.f, 1.f, 1.f};
    float m_slope = NORMAL_SLOPE;

    // Is m_ref the growing fill (Fill), or the full bar (Track)? Figured out at
    // runtime by watching whether its width changes with progress.
    Mode m_mode = Mode::Unknown;
    bool m_haveSample = false;
    float m_sampleP = 0.f;
    float m_sampleW = 0.f;
    float m_fallbackWidth = 0.f;
    float m_grooveWidth = 0.f;
    bool m_originalScanned = false;

    // Zigzag turning points in screen units: x = distance along the bar,
    // y = offset from the bar's center line.
    std::vector<CCPoint> m_path;
    float m_range = 0.f; // total height of the zigzag
    uint32_t m_seed = 0;
    uint32_t m_state = 0;
    int m_dir = 1;
    float m_curY = 0.f;

    std::array<float, 11> m_trackKey = {};
    bool m_loggedFrame = false;

    bool init(CCNode* ref, uint32_t seed, WaveBarSettings const& settings) {
        if (!CCNode::init()) return false;

        m_ref = ref;
        m_settings = settings;
        m_seed = seed;
        m_mode = s_knownMode;
        m_slope = settings.miniWave ? MINI_SLOPE : NORMAL_SLOPE;

        auto gm = GameManager::get();

        // Trail color follows the player's icon colors.
        auto c = gm->colorForIdx(settings.useSecondaryColor ? gm->getPlayerColor2() : gm->getPlayerColor());
        m_trailColor = {c.r / 255.f, c.g / 255.f, c.b / 255.f, 1.f};

        // Outline layer: everything inside the path is cut out (inverted clip).
        m_stencil = CCDrawNode::create();
        m_stencil->setScale(1.f / DRAW_SCALE);
        m_clip = CCClippingNode::create(m_stencil);
        m_clip->setInverted(true);
        this->addChild(m_clip, 0);

        m_track = CCDrawNode::create();
        m_track->setScale(1.f / DRAW_SCALE);
        m_clip->addChild(m_track);

        // Filled part of the path.
        m_trail = CCDrawNode::create();
        m_trail->setScale(1.f / DRAW_SCALE);
        this->addChild(m_trail, 1);

        // The wave icon the player has equipped. Always above the path.
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

    // ----- finding / hiding the original bar -------------------------------

    bool isAncestorOfThis(CCNode* n) {
        for (CCNode* p = this->getParent(); p; p = p->getParent()) {
            if (p == n) return true;
        }
        return false;
    }

    // Is this node shaped and placed like the progress bar (long, thin, and
    // sitting on top of the fill)? Optionally reports its width in world units.
    bool looksLikeBar(CCNode* n, CCPoint const& fillOrigin, float fillHeight, float* outWidth) const {
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

        if (outWidth) *outWidth = w;
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

    // Finds the original bar's pieces, measures its track, and (optionally)
    // fades them out.
    void scanOriginalBar(CCPoint const& fillOrigin, float fillHeight, bool hide) {
        std::vector<CCNode*> candidates;
        candidates.push_back(m_ref);

        // The fill's ancestors (the track is often its parent), and the
        // siblings of the fill and of its parent (the track can also sit next to it).
        CCNode* a = m_ref->getParent();
        for (int i = 0; a && i < 3; ++i, a = a->getParent()) {
            candidates.push_back(a);
            if (i <= 1) {
                if (auto kids = a->getChildren()) {
                    for (auto k : CCArrayExt<CCNode*>(kids)) candidates.push_back(k);
                }
            }
        }

        int count = 0;
        for (auto n : candidates) {
            if (n == this || this->isAncestorOfThis(n)) continue;

            if (n != m_ref) {
                float w = 0.f;
                if (!this->looksLikeBar(n, fillOrigin, fillHeight, &w)) continue;
                m_grooveWidth = std::max(m_grooveWidth, w);
            }
            if (hide) this->fadeOut(n);
            ++count;
        }
        log::info(
            "[WaveBar] found {} node(s) of the original bar (hidden={}), track width={}",
            count, hide, m_grooveWidth
        );
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
            // How far up/down can we go before hitting the edge?
            float const maxRise = m_dir > 0 ? (yMax - m_curY) : (m_curY - yMin);
            float rise = maxRise;

            if (m_settings.chaotic) {
                float const lo = std::min(minSegment, maxRise);
                rise = lo + this->rnd() * (maxRise - lo);
            }
            rise = std::max(rise, 0.25f);

            // slope 1 = 45 degrees, slope 2 = twice as steep (mini wave)
            float const run = rise / m_slope;
            float const x = m_path.back().x + run;
            m_curY = std::clamp(m_curY + m_dir * rise, yMin, yMax);
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

    // The path between two distances along the bar: start, every turn, end.
    std::vector<CCPoint> pathBetween(float from, float to) const {
        std::vector<CCPoint> pts;
        pts.push_back(ccp(from, this->yAt(from)));
        for (auto const& v : m_path) {
            if (v.x > from && v.x < to) pts.push_back(v);
        }
        pts.push_back(ccp(to, this->yAt(to)));
        return pts;
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
        if (m_haveSample && m_sampleP > 0.f) {
            m_fallbackWidth = m_sampleW / m_sampleP;
        }

        // Measure the original bar and fade it out, once we know where it is.
        if (!m_originalScanned) {
            m_originalScanned = true;
            this->scanOriginalBar(origin, barHeight, m_settings.hideOriginal);
        }

        // ----- how wide is the whole bar? --------------------------------
        float totalWidth = fillWidth;
        if (m_mode == Mode::Fill) {
            // Learn the real full width once the fill is long enough to measure it
            // well, and remember it for the next levels.
            if (s_widthRatio <= 0.f && progress >= WIDTH_LOCK_PROGRESS) {
                s_widthRatio = (fillWidth / progress) / barHeight;
                log::info("[WaveBar] measured full bar width = {} x bar height", s_widthRatio);
            }
            if (s_widthRatio > 0.f) {
                totalWidth = s_widthRatio * barHeight;
            } else if (m_grooveWidth > 0.f) {
                totalWidth = m_grooveWidth;
            } else if (m_fallbackWidth > 0.f) {
                totalWidth = m_fallbackWidth;
            }
        }
        totalWidth = std::max(totalWidth, 1.f);

        // Where is the head of the wave, measured along the bar from its left edge?
        float head = (m_mode == Mode::Fill) ? fillWidth : progress * fillWidth;
        head = std::clamp(head, 0.f, totalWidth);

        float const startD = m_settings.trailLength >= 0.999f
            ? 0.f
            : std::max(0.f, head - m_settings.trailLength * totalWidth);

        // ----- sizes (world units) ---------------------------------------
        float const mini = m_settings.miniWave ? MINI_SCALE : 1.f;
        float const trailR = barHeight * TRAIL_BAR_RATIO * m_settings.trailThickness * mini / 2.f;
        float const ringInner = trailR + barHeight * RING_BLACK;
        float const ringWhite = ringInner + barHeight * RING_WHITE;
        float const ringOuter = ringWhite + barHeight * RING_BLACK;
        float const iconSize = barHeight * ICON_BAR_RATIO * m_settings.iconScale * mini;

        if (m_range <= 0.f) {
            this->startPath(barHeight * BAR_USABLE_HEIGHT * m_settings.zigzagHeight);
        }
        this->extendTo(totalWidth);

        // ----- vertical position -----------------------------------------
        float centerY = origin.y + barHeight / 2.f - m_settings.verticalOffset * barHeight;

        // Keep everything (outline corners and icon) inside the top of the screen.
        // The pointed corners of the path stick out by radius * sqrt(1 + slope^2).
        float const cornerReach = std::sqrt(1.f + m_slope * m_slope);
        float const pathReach = (m_settings.pathOutline ? ringOuter : trailR) * cornerReach;
        float const winHeight = CCDirector::get()->getWinSize().height;
        float const topExtent = m_range / 2.f + std::max(pathReach, iconSize * 0.6f);
        float const overflow = centerY + topExtent + SCREEN_MARGIN - winHeight;
        if (overflow > 0.f) centerY -= overflow;

        auto toLocal = [&](CCPoint const& p) {
            return this->convertToNodeSpace(ccp(origin.x + p.x, centerY + p.y));
        };
        auto localPath = [&](float from, float to) {
            std::vector<CCPoint> out;
            for (auto const& p : this->pathBetween(from, to)) out.push_back(toLocal(p));
            return out;
        };

        // ----- the empty path with its outline (only redrawn when needed) ---
        std::array<float, 11> const key = {
            totalWidth, origin.x, centerY, w0.x, w0.y, unit, trailR, ringOuter,
            m_settings.pathOutline ? 1.f : 0.f, m_slope, barHeight
        };
        bool dirty = false;
        for (size_t i = 0; i < key.size(); ++i) {
            if (std::abs(key[i] - m_trackKey[i]) > 0.01f) dirty = true;
        }
        if (dirty) {
            m_trackKey = key;
            m_stencil->clear();
            m_track->clear();
            if (m_settings.pathOutline) {
                auto full = localPath(0.f, totalWidth);
                float const r = 1.f / unit;
                ccColor4F const black = ccc4f(0.f, 0.f, 0.f, 1.f);
                ccColor4F const white = ccc4f(1.f, 1.f, 1.f, 1.f);

                // The inside of the path is cut out of the outline, so it stays transparent.
                // The ends are rounded, like the original progress bar.
                strokePolyline(m_stencil, full, trailR * r, white, true, true);
                // outside to inside: black, white, black
                strokePolyline(m_track, full, ringOuter * r, black, true, true);
                strokePolyline(m_track, full, ringWhite * r, white, true, true);
                strokePolyline(m_track, full, ringInner * r, black, true, true);
            }
        }

        // ----- the filled part: colored trail with an optional white line ----
        auto filled = localPath(startD, head);
        m_trail->clear();
        // The start of the trail is rounded to fit the rounded start of the path
        // outline. With the outline turned off, it stays flat.
        bool const roundStart = m_settings.pathOutline && startD <= 0.001f;
        strokePolyline(m_trail, filled, trailR / unit, m_trailColor, roundStart, false);
        if (!m_settings.solidTrail) {
            strokePolyline(m_trail, filled, trailR * CORE_RATIO / unit, ccc4f(1.f, 1.f, 1.f, 1.f), roundStart, false);
        }

        // ----- wave icon: on top of everything, pointing along the path -----
        float const iconScale = (iconSize / ICON_NATIVE_SIZE) / unit;
        size_t const seg = this->segmentAt(head);
        bool const goingUp = m_path[seg + 1].y > m_path[seg].y;
        float const angle = std::atan(m_slope) * 180.f / PI_F;

        m_icon->setScale(iconScale);
        m_icon->setPosition(filled.back());
        m_icon->setRotation(goingUp ? -angle : angle);
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
