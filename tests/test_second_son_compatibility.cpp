// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/second_son_compatibility.h"
#include <cstdio>
#include <cstdlib>

namespace {
void Require(bool value, const char* reason) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", reason);
        std::exit(1);
    }
}
} // namespace
int main() {
    using namespace Core::SecondSon;
    for (const auto id : TitleIds) {
        Require(IsTitle(id), "all four Second Son regions accepted");
    }
    for (const auto id :
         {"CUSA0309", "CUSA00004x", "CUSA00004/../", "CUSA00004 ", "CUSA00068", ""}) {
        Require(!IsTitle(id), "other titles and malformed identifiers rejected");
    }
    Require(IsVersion("01.00") && IsVersion("01.07"), "multiple update versions accepted");
    for (const auto version : {"1.07", "01.7", "01.07x", "aa.bb", ""}) {
        Require(!IsVersion(version), "malformed APP_VER rejected");
    }
    Build profile{{"CUSA00004", "CUSA00309"}, "01.07", std::string(64, 'a'), "CUSA00309.xml"};
    Require(profile.IsValid(), "valid fingerprint-bound profile accepted");
    Require(profile.Matches("CUSA00309", "01.07", std::string(64, 'a')),
            "matching original region accepted");
    Require(profile.Matches("CUSA00004", "01.07", std::string(64, 'a')),
            "same exact executable reusable in another declared region");
    Require(!profile.Matches("CUSA00309", "01.00", std::string(64, 'a')),
            "another update never inherits fixed patch offsets");
    Require(!profile.Matches("CUSA00309", "01.07", std::string(64, 'b')),
            "same region/version with different program never inherits offsets");
    Require(!profile.Matches("CUSA00223", "01.07", std::string(64, 'a')),
            "undeclared region cannot use the profile");
    Require(EffectiveMode("pro4k", false) == "1080p", "unknown builds have safe 1080p fallback");
    Require(EffectiveMode("1080p", true) == "1080p", "verified profile respects 1080p selection");
    Require(EffectiveMode("pro4k", true) == "pro4k", "verified profile enables Pro 4K");
    for (const auto& resolution : Resolutions) {
        Require(EffectiveMode(resolution.key, true) == resolution.key,
                "verified builds preserve every menu resolution");
        Require(EffectiveMode(resolution.key, false) == "1080p",
                "every unverified resolution falls back safely");
        Require(FindResolution(resolution.key).width >= 1280, "menu dimensions are valid");
        Require((resolution.height >= 1440) == resolution.pro, "Pro policy follows resolution");
    }
    profile.legacy_cache_title = "CUSA00223";
    Require(!profile.IsValid(), "legacy cache cannot refer to another undeclared region");
    profile.legacy_cache_title.clear();
    profile.patch_file = "../CUSA00309.xml";
    Require(!profile.IsValid(), "patch path cannot leave profiles");
    profile.patch_file = "CUSA00309.xml";
    profile.title_ids.push_back("CUSA00068");
    Require(!profile.IsValid(), "registry cannot widen the scope to unrelated games");
    std::puts("Second Son compatibility policy checks passed.");
}
