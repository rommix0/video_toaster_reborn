// loadEffect: crouton file -> Effect.
#include <array>
#include <cctype>
#include <exception>

#include "effect_detail.h"
#include "log_detail.h"
#include "vt/control.h"
#include "vt/crouton.h"
#include "vt/dve.h"
#include "vt/framestore.h"
#include "vt/iff.h"

namespace vt {
namespace detail {

std::optional<Sound> loadSound(const Crouton& c, const std::string& effectName) {
    try {
        return readSound(c.bytes);
    } catch (const std::exception& ex) {  // compressed or malformed 8SVX: the effect plays without sound
        warn(effectName + ": sound not loaded (" + ex.what() + ")");
        return std::nullopt;
    }
}

}  // namespace detail

namespace {

constexpr std::array<char, 4> kAnimForm{'A', 'N', 'I', 'M'};
constexpr std::array<char, 4> kIlbmForm{'I', 'L', 'B', 'M'};
constexpr uint32_t kTagMode = 0x80000022;
constexpr uint32_t kTagColour = 0x80000023;        // Effect Color preset (vt/background.h)
constexpr uint32_t kTagCustomColour = 0x80000024;  // custom generator word
constexpr uint32_t kTagSourceWords = 0x1000001B;   // per palette index hardware words (vt/mix.h)
constexpr uint32_t kTagDescriptor = 0x80000007;    // routing descriptor (ALGO.md 7)
constexpr uint32_t kTagBusMask = 0x80000076;       // MAIN croutons: the source for Preview

// Rulings 27, 32: the crouton's own folder is an Overlays drawer (4.2/Effects/2000FX/OverLays,
// 4.2/Effects/4000FX/Overlays), any case. No crouton tag separates these effects from the rest:
// tag 0x80000011 is missing from seven of them, and "Wedding Bells !" sits byte for byte in both
// Overlays and Weddings.
bool inOverlaysDrawer(const std::filesystem::path& p) {
    std::error_code ec;
    const auto abs = std::filesystem::absolute(p, ec);
    std::string s = detail::utf8((ec ? p : abs).parent_path().filename());
    for (auto& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s == "overlays";
}

const Form* firstForm(const Crouton& c, std::array<char, 4> type) {
    for (const auto& f : c.forms)
        if (f.type == type) return &f;
    return nullptr;
}

// Ruling 9: the original reads a crouton's CrUD from its icon file. Five files hold no CrUD
// themselves: the Smooth Fade pair in 2000FX and 4000FX (ALGO) and the ChromaFX BluePoster, Gamma
// and Zebra Stripe (CRFX, LUT and all); theirs is in the sibling <name>.i / .I.
std::optional<Crouton> iconSibling(const std::filesystem::path& p) {
    for (const char* ext : {".i", ".I"}) {
        auto sib = p;
        sib += ext;
        if (auto c = loadCrouton(sib); c && (c->type == CroutonType::Algo || c->type == CroutonType::Crfx)) return c;
    }
    return std::nullopt;
}

constexpr const char* kFramestoreRefusal = "a framestore (load it into DV1/DV2), not an effect";

std::unique_ptr<Effect> fail(std::string* why, std::string reason) {
    if (why) *why = std::move(reason);
    return nullptr;
}

}  // namespace

std::unique_ptr<Effect> loadEffect(const std::filesystem::path& path, std::string* why) {
    try {
        auto c = loadCrouton(path);
        if (c && c->type == CroutonType::None) {  // no CrUD of its own: the sibling icon's (Ruling 9)
            if (isFramestore(path)) return fail(why, kFramestoreRefusal);  // FRAM icon, or a bare DV picture
            if (auto icon = iconSibling(path)) c = std::move(icon);
        }
        if (!c) return fail(why, "not a crouton file: " + detail::utf8(path));
        const std::string name = detail::utf8(path.filename());

        // Name, timing, sound, colour, drawer; the sound is read only for effects that get built.
        // A ChromaFX crouton is never an overlay (none is in an Overlays drawer).
        auto basics = [&](Timing t) {
            return detail::CroutonBasics{name, t, detail::loadSound(*c, name),
                                         effectColourFromTags(c->tag(kTagColour), c->tag(kTagCustomColour)),
                                         c->type != CroutonType::Crfx && inOverlaysDrawer(path),
                                         uint32_t(c->tag(kTagDescriptor).value_or(1))};
        };

        switch (c->type) {
            case CroutonType::Algo: return detail::makeAlgoEffect(basics(algoTiming(*c)), *c, why);
            case CroutonType::Main: return fail(why, "a control crouton (puts its source on Preview), not an effect");
            case CroutonType::Ctrl: return fail(why, "a project control (Blank, Stop), not an effect");
            case CroutonType::Crfx: return detail::makeChromaEffect(basics(fixedTiming(*c)), *c, why);
            case CroutonType::Fram: return fail(why, kFramestoreRefusal);
            default: break;
        }

        // The content decides, not the TYPE (Ruling 3).
        if (const Form* af = firstForm(*c, kAnimForm)) {
            Anim an = readAnim(c->bytes, *af);
            if (!an.planes.empty() && isDve(an.planes[0])) {
                // DVE files are ANIM content; the one 9-plane DVE has no index frames (Ruling 6)
                const auto tag = c->blobs.find(kTagSourceWords);
                return detail::makeDveEffect(basics(animTiming(*c, int(an.planes.size()))), an,
                                             tag != c->blobs.end() ? tag->second : std::vector<uint8_t>{},
                                             c->tag(kTagDescriptor).value_or(0), why);
            }
            AnimPlan plan = planAnim(an, c->tag(kTagMode));
            if (plan.kind == AnimKind::Unsupported) return fail(why, plan.reason);
            if (auto it = c->blobs.find(kTagSourceWords); it != c->blobs.end())
                plan.routes = paletteRoutes(it->second, an.bm.planes);
            auto b = basics(animTiming(*c, int(an.frames.size())));  // before `an` is moved from
            return detail::makeAnimEffect(std::move(b), std::move(an), std::move(plan), why);
        }

        for (const auto& f : c->forms) {
            if (f.type != kIlbmForm) continue;
            const Bitmap head = readBitmapHeader(c->bytes, f);
            if (head.w == 80 && head.h == 50) continue;  // the icon
            if (const char* refusal = detail::gradientRefusal(head)) return fail(why, refusal);
            auto [bm, img] = readIlbm(c->bytes, f);
            return detail::makeGradient(basics(fixedTiming(*c)), std::move(bm), std::move(img), why);
        }
        return fail(why, "no image data (parameter-only effect)");
    } catch (const std::exception& ex) {
        return fail(why, std::string("cannot read effect: ") + ex.what());
    }
}

std::optional<ControlMove> loadControl(const std::filesystem::path& path) {
    const auto c = loadCrouton(path);
    if (!c || (c->type != CroutonType::Main && c->type != CroutonType::Ctrl)) return std::nullopt;
    ControlMove m;
    if (c->type == CroutonType::Ctrl) return m;
    // Bus mask bits (FREEZE.md 0); bit 15 on inputs 3/4 toggles an input routing vtoast has no
    // counterpart for. A zero mask leaves Preview alone, as the handler does.
    switch (c->tag(kTagBusMask).value_or(0) & 0x7f) {
        case 0x01: m.preview = Source::In1; break;
        case 0x02: m.preview = Source::In2; break;
        case 0x04: m.preview = Source::In3; break;
        case 0x08: m.preview = Source::In4; break;
        case 0x10: m.preview = Source::DV1; break;
        case 0x20: m.preview = Source::DV2; break;
        case 0x40: m.preview = Source::Bkg; break;
        default: break;
    }
    if (const auto colour = c->tag(kTagColour); colour && *colour >= 1 && *colour <= 9)
        m.background = BkgColour(*colour);  // the Effect Color presets, in tag order
    return m;
}

}  // namespace vt
