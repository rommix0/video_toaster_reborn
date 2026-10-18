// loadEffect: crouton file -> Effect.
#include <array>
#include <cstdio>
#include <exception>

#include "effect_detail.h"
#include "vt/crouton.h"
#include "vt/dve.h"
#include "vt/iff.h"

namespace vt {
namespace detail {

std::optional<Sound> loadSound(const Crouton& c, const std::string& effectName) {
    try {
        return readSound(c.bytes);
    } catch (const std::exception& ex) {  // compressed or malformed 8SVX: the effect plays without sound
        std::fprintf(stderr, "vtoast: %s: sound not loaded (%s)\n", effectName.c_str(), ex.what());
        return std::nullopt;
    }
}

}  // namespace detail

namespace {

constexpr std::array<char, 4> kAnimForm{'A', 'N', 'I', 'M'};
constexpr std::array<char, 4> kIlbmForm{'I', 'L', 'B', 'M'};
constexpr uint32_t kTagMode = 0x80000022;

const Form* firstForm(const Crouton& c, std::array<char, 4> type) {
    for (const auto& f : c.forms)
        if (f.type == type) return &f;
    return nullptr;
}

std::unique_ptr<Effect> fail(std::string* why, std::string reason) {
    if (why) *why = std::move(reason);
    return nullptr;
}

}  // namespace

std::unique_ptr<Effect> loadEffect(const std::filesystem::path& path, std::string* why) {
    try {
        auto c = loadCrouton(path);
        if (!c) return fail(why, "not a crouton file: " + path.string());
        const auto u8 = path.filename().u8string();
        const std::string name(u8.begin(), u8.end());

        switch (c->type) {
            case CroutonType::Algo: return fail(why, "Switcher move (ALGO) not reverse-engineered yet");
            case CroutonType::Main: return fail(why, "Switcher move (MAIN) not reverse-engineered yet");
            case CroutonType::Ctrl: return fail(why, "Switcher move (CTRL) not reverse-engineered yet");
            case CroutonType::Crfx: return fail(why, "ChromaFX support not built yet");
            default: break;
        }

        // The content decides, not the TYPE (Ruling 3).
        if (const Form* af = firstForm(*c, kAnimForm)) {
            Anim an = readAnim(c->bytes, *af);
            if (!an.planes.empty() && isDve(an.planes[0])) {
                // DVE files are ANIM content; the one 9-plane DVE has no index frames (Ruling 6)
                const Timing timing = animTiming(*c, int(an.planes.size()));
                return detail::makeDveEffect(name, timing, detail::loadSound(*c, name), an, why);
            }
            AnimPlan plan = planAnim(an, c->tag(kTagMode));
            if (plan.kind == AnimKind::Unsupported) return fail(why, plan.reason);
            const Timing timing = animTiming(*c, int(an.frames.size()));
            return detail::makeAnimEffect(name, timing, detail::loadSound(*c, name), std::move(an),
                                           std::move(plan), why);
        }

        for (const auto& f : c->forms) {
            if (f.type != kIlbmForm) continue;
            const Bitmap head = readBitmapHeader(c->bytes, f);
            if (head.w == 80 && head.h == 50) continue;  // the icon
            if (const char* refusal = detail::gradientRefusal(head)) return fail(why, refusal);
            auto [bm, img] = readIlbm(c->bytes, f);
            return detail::makeGradient(name, fixedTiming(*c), detail::loadSound(*c, name), std::move(bm),
                                        std::move(img), why);
        }
        return fail(why, "no image data (parameter-only effect)");
    } catch (const std::exception& ex) {
        return fail(why, std::string("cannot read effect: ") + ex.what());
    }
}

}  // namespace vt
