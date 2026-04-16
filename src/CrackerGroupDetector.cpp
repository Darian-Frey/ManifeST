#include "manifest/CrackerGroupDetector.hpp"

#include "manifest/GameStringScanner.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace manifest {

namespace {

// Signature table. Each entry: (group_name, [patterns ...]).
//
// Rules of thumb when curating this:
//
// * Patterns are matched UPPERCASE with word-boundary semantics — see
//   GameStringScanner::matchKnownGames for the same matcher.
// * Each pattern must be ≥ 5 chars to avoid coincidental hits in
//   ordinary English text or in game data. Group names that are short
//   English words (BOSS, BBC, ICS, IMPACT) are intentionally omitted
//   here; if upstream wants them, longer signatures (intro greets,
//   distinctive scrolltext phrases) need to be supplied first.
// * Multiple patterns per group OR'd — any one hit attributes the group.
// * Avoid signatures that overlap two groups; otherwise we'll tag both.
//
// The engine already detects D-Bug, Pompey, Medway, Cynix, Elite, TRSI,
// Metallinos, CSM, Replicants, Neuter Booter, Copylock, Sleepwalker,
// MS-DOS via boot-sector hooks. We re-state those here so a deeper hit
// (cracktro screen, scrolltext) still attributes correctly when the
// boot sector itself was bypassed by a stub loader. ManifeST tags
// from BOTH sources and dedupes.
struct GroupSig {
    const char*              name;
    std::vector<const char*> patterns;
};

// clang-format off
static const std::vector<GroupSig> kSignatures = {
    // --- Engine-detected (re-stated for deep-hit coverage) ---------------
    { "D-Bug",            { "D-BUG", "DBUG", "DBUG.PRG" } },
    { "Pompey Pirates",   { "POMPEY PIRATES", "POMPEY PIRATE" } },
    { "Medway Boys",      { "MEDWAY BOYS", "MEDWAY 9", "MEDWAY 8", "MEDWAY 7",
                            "MEDWAY 6", "MEDWAY 5", "MEDWAY 4", "MEDWAY 3",
                            "MEDWAY 2", "MEDWAY 1" } },
    { "Cynix",            { "CYNIX LOADER", "CYNIX PRESENTS" } },
    { "Replicants",       { "REPLICANTS" } },
    { "TRSI",             { "TRSI", "TRISTAR & RED SECTOR", "TRISTAR" } },
    { "Elite",            { "ELITE PRESENTS", "ELITE\\TEKNIX" } },

    // --- Cracker / cracking groups not in the engine ---------------------
    { "Automation",       { "AUTOMATION ", "AUTO MATION", "BREAK FREE OR DIE" } },
    { "LSD",              { "LITTLE STAINED" } },
    { "Was (Not Was)",    { "WAS NOT WAS", "WAS (NOT WAS)" } },
    { "Empire",           { "THE EMPIRE", "EMPIRE PRESENTS" } },
    { "Fuzion",           { "FUZION " } },
    { "MCA",              { "M.C.A.", "THE MAGICIAN" } },
    { "Hotline",          { "HOTLINE PRESENTS", "HOTLINE CRACK" } },
    { "Blade Runners",    { "BLADERUNNERS", "BLADE RUNNERS" } },
    { "Delight",          { "DELIGHT PRESENT", "DELIGHT CRACK" } },
    { "Vectronix",        { "VECTRONIX" } },
    { "Superior",         { "SUPERIOR PRESENT", "SUPERIOR CRACK" } },
    { "Flame of Finland", { "FLAME OF FINLAND", "FOF PRESENTS" } },
    { "Mad Vision",       { "MAD VISION" } },
    { "Point of View",    { "POINT OF VIEW", "P.O.V." } },
    { "Bad Brew Crew",    { "BAD BREW CREW", "BAD BREW " } },
    { "Dream Weavers",    { "DREAM WEAVERS" } },
    { "TCB",              { "CAREBEARS", "CARE BEARS",
                            "THE CAREBEARS", "THE CARE BEARS" } },
    { "Sewer Software",   { "SEWER SOFTWARE", "SEWER SOFT" } },
    { "Euroswap",         { "EUROSWAP" } },
    { "FOFT",             { "FREE TRADERS", "FED OF FREE" } },
    { "Supremacy",        { "SUPREMACY" } },
    { "Scottish Crackin", { "SCOTTISH CRACKIN", "SCOTTISH CRACKING" } },
    { "Midland Boyz",     { "MIDLAND BOYZ", "MIDLANDS BOYZ" } },
    { "Delta Force",      { "DELTA FORCE" } },
    { "Pompey Krappy",    { "KRAPPY KOMPACT", "POMPEY KRAPPY" } },
    { "Spaced Out",       { "SPACED OUT" } },

    // --- Common cracktro greets often used as group signatures ----------
    // None — patterns above are already specific enough that adding
    // generic greets ("CRACKED BY", "MENU NUMBER") would inflate noise.
};
// clang-format on

bool containsAsWord(const std::string& run, const std::string& pat) {
    std::size_t pos = 0;
    while ((pos = run.find(pat, pos)) != std::string::npos) {
        const bool left_ok  = (pos == 0) ||
            !std::isalnum(static_cast<unsigned char>(run[pos - 1]));
        const std::size_t end = pos + pat.size();
        const bool right_ok = (end >= run.size()) ||
            !std::isalnum(static_cast<unsigned char>(run[end]));
        if (left_ok && right_ok) return true;
        ++pos;
    }
    return false;
}

} // namespace

std::vector<CrackerGroupDetector::Evidence>
CrackerGroupDetector::detectWithEvidence(const std::vector<uint8_t>& bytes) {
    const auto runs = GameStringScanner::extractRuns(
        bytes, GameStringScanner::defaultConfig());

    // group → (match_pattern, evidence_run). First hit wins as evidence.
    std::unordered_map<std::string, std::pair<std::string, std::string>> hits;

    for (const auto& sig : kSignatures) {
        if (hits.count(sig.name)) continue;
        for (const auto& pat_c : sig.patterns) {
            std::string pat = pat_c;
            for (auto& c : pat) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (pat.size() < 5) continue;
            for (const auto& run : runs) {
                if (containsAsWord(run, pat)) {
                    hits[sig.name] = {pat, run};
                    goto next_group;
                }
            }
        }
        next_group:;
    }

    std::vector<Evidence> out;
    out.reserve(hits.size());
    for (auto& [group, ev] : hits) {
        out.push_back({group, std::move(ev.first), std::move(ev.second)});
    }
    std::sort(out.begin(), out.end(),
              [](const Evidence& a, const Evidence& b){ return a.group < b.group; });
    return out;
}

std::vector<std::string>
CrackerGroupDetector::detect(const std::vector<uint8_t>& bytes) {
    auto ev = detectWithEvidence(bytes);
    std::vector<std::string> out;
    out.reserve(ev.size());
    for (auto& e : ev) out.push_back(std::move(e.group));
    return out;
}

} // namespace manifest
