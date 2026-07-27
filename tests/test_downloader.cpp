#include <gtest/gtest.h>
#include "package_downloader_lib.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

// ─── Mock fetcher: serve a canned catalog so the resolver can be tested
//     without a network. Answers the default repo's logos-repo.json and the
//     index.json it points at; everything else 404s.
namespace {
constexpr const char* kIndexUrl = "https://test.local/index.json";

class MockFetcher : public lgpd::Fetcher {
public:
    std::string repoJson;   // served for the default repo URL
    std::string indexJson;  // served for kIndexUrl
    bool get(const std::string& url, std::string& out) override {
        if (url == lgpd::kDefaultRepositoryUrl) { out = repoJson;  return true; }
        if (url == kIndexUrl)                    { out = indexJson; return true; }
        return false;
    }
    bool getToFile(const std::string&, const std::string&) override { return false; }
};

json makeVersion(const char* ver, const char* hash, const json& deps) {
    // These tests order versions by SemVer precedence, so releasedAt only
    // tie-breaks equal versions (none here) — a single valid date suffices.
    return json{
        {"releasedAt", "2026-01-01T00:00:00Z"},
        {"url", std::string("https://test.local/") + ver + ".lgx"},
        {"rootHash", hash},
        {"manifest", {
            {"manifestVersion", "0.1.0"}, {"name", "x"}, {"version", ver},
            {"type", "core"}, {"dependencies", deps},
            {"main", {{"linux-amd64", "lib/x.so"}}},
        }},
    };
}

// A two-package catalog: blockchain_module @ {0.1.0, 0.2.0} and blockchain_ui
// @ 0.1.0 which depends on blockchain_module (range configurable).
std::shared_ptr<MockFetcher> catalogFetcher(const json& uiDepRange) {
    auto f = std::make_shared<MockFetcher>();
    f->repoJson = json{{"schemaVersion", 1}, {"name", "test"}, {"displayName", "Test"},
                       {"indexUrl", kIndexUrl}, {"trustedSigners", json::array()}}.dump();
    json bmV1 = makeVersion("0.1.0", "h_bm_010", json::array());
    bmV1["manifest"]["name"] = "blockchain_module";
    json bmV2 = makeVersion("0.2.0", "h_bm_020", json::array());
    bmV2["manifest"]["name"] = "blockchain_module";
    json ui  = makeVersion("0.1.0", "h_ui_010", json::array({ uiDepRange }));
    ui["manifest"]["name"] = "blockchain_ui";
    ui["manifest"]["type"] = "ui_qml";
    f->indexJson = json{
        {"schemaVersion", 2}, {"repositoryName", "test"},
        {"packages", json::array({
            json{{"name", "blockchain_module"}, {"versions", json::array({bmV2, bmV1})}},
            json{{"name", "blockchain_ui"},     {"versions", json::array({ui})}},
        })},
    }.dump();
    return f;
}

// Collect resolved entries by name -> list of versions.
std::map<std::string, std::vector<std::string>> resolvedVersions(const std::string& resolvedJson) {
    std::map<std::string, std::vector<std::string>> byName;
    for (const auto& e : json::parse(resolvedJson)) {
        if (e.contains("error") || !e.contains("name")) continue;
        byName[e.value("name", "")].push_back(e.value("version", ""));
    }
    return byName;
}
}  // namespace

// ─── Semver matcher ───────────────────────────────────────────────────────────
// These are pure tests — no network involved.

TEST(Semver, ExactAndComparator) {
    using lgpd::PackageDownloaderLib;
    EXPECT_TRUE (PackageDownloaderLib::semverMatches("1.2.3",     "1.2.3"));
    EXPECT_FALSE(PackageDownloaderLib::semverMatches("1.2.3",     "1.2.4"));
    EXPECT_TRUE (PackageDownloaderLib::semverMatches(">=1.0.0",   "1.2.3"));
    EXPECT_TRUE (PackageDownloaderLib::semverMatches(">=1.0.0",   "9.0.0"));
    EXPECT_FALSE(PackageDownloaderLib::semverMatches(">=2.0.0",   "1.99.0"));
    EXPECT_TRUE (PackageDownloaderLib::semverMatches("<2.0.0",    "1.99.0"));
}

TEST(Semver, CaretAndTilde) {
    using lgpd::PackageDownloaderLib;
    EXPECT_TRUE (PackageDownloaderLib::semverMatches("^1.2.3",    "1.9.9"));
    EXPECT_FALSE(PackageDownloaderLib::semverMatches("^1.2.3",    "2.0.0"));
    EXPECT_TRUE (PackageDownloaderLib::semverMatches("^0.2.3",    "0.2.9"));
    EXPECT_FALSE(PackageDownloaderLib::semverMatches("^0.2.3",    "0.3.0"));
    EXPECT_TRUE (PackageDownloaderLib::semverMatches("~1.2.3",    "1.2.9"));
    EXPECT_FALSE(PackageDownloaderLib::semverMatches("~1.2.3",    "1.3.0"));
}

TEST(Semver, WildcardAndConjunction) {
    using lgpd::PackageDownloaderLib;
    EXPECT_TRUE (PackageDownloaderLib::semverMatches("*",          "9.9.9"));
    EXPECT_TRUE (PackageDownloaderLib::semverMatches("1.x",        "1.5.0"));
    EXPECT_FALSE(PackageDownloaderLib::semverMatches("1.x",        "2.0.0"));
    EXPECT_TRUE (PackageDownloaderLib::semverMatches(">=1.0 <2.0", "1.5.0"));
    EXPECT_FALSE(PackageDownloaderLib::semverMatches(">=1.0 <2.0", "2.0.1"));
    // Alternation
    EXPECT_TRUE (PackageDownloaderLib::semverMatches("1.x || 2.x", "2.3.4"));
    EXPECT_FALSE(PackageDownloaderLib::semverMatches("1.x || 2.x", "3.0.0"));
}

// REGRESSION. Ranges must not pull in pre-releases nobody asked for.
//
// The old hand-rolled matcher had no pre-release rule, so `^1.0.0` matched
// `2.0.0-alpha`: an unreleased alpha of the *next major* satisfied a caret range
// on 1.x and could be resolved as a dependency. Matching now goes through the
// shared implementation in logos-package, which enforces the npm rule — a range
// only sees pre-releases if it names one at the same major.minor.patch.
TEST(Semver, RangesDoNotMatchUnrequestedPreReleases) {
    using lgpd::PackageDownloaderLib;
    EXPECT_FALSE(PackageDownloaderLib::semverMatches("^1.0.0",  "2.0.0-alpha"));
    EXPECT_FALSE(PackageDownloaderLib::semverMatches("^1.0.0",  "1.5.0-beta.1"));
    EXPECT_FALSE(PackageDownloaderLib::semverMatches(">=1.0.0", "1.0.0-rc.1"));
    EXPECT_FALSE(PackageDownloaderLib::semverMatches("*",       "1.0.0-alpha"));

    // ...but an opted-in range still resolves them.
    EXPECT_TRUE (PackageDownloaderLib::semverMatches("^1.0.0-rc.1", "1.0.0-rc.2"));
    EXPECT_TRUE (PackageDownloaderLib::semverMatches("^1.0.0-rc.1", "1.0.0"));
}

// ─── Resolver ranking ────────────────────────────────────────────────────────

// REGRESSION. The resolver picked the matching candidate with the newest
// `releasedAt`, which is not the highest version. A 1.2.1 hotfix backported
// after 2.0.0 shipped has a newer timestamp but is an older version — and it won.
TEST(Ranking, HighestVersionBeatsLaterPublishedLowerVersion) {
    using lgpd::PackageDownloaderLib;
    // 2.0.0 released Jan; 1.2.1 backported in March. 2.0.0 must still win.
    EXPECT_FALSE(PackageDownloaderLib::outranks("1.2.1", "2026-03-01T00:00:00Z",
                                                "2.0.0", "2026-01-01T00:00:00Z"));
    EXPECT_TRUE (PackageDownloaderLib::outranks("2.0.0", "2026-01-01T00:00:00Z",
                                                "1.2.1", "2026-03-01T00:00:00Z"));
}

TEST(Ranking, ReleasedAtOnlyBreaksTiesBetweenEqualVersions) {
    using lgpd::PackageDownloaderLib;
    // Same version republished (e.g. a different rootHash, or a second repo):
    // the newer publish wins.
    EXPECT_TRUE (PackageDownloaderLib::outranks("1.0.0", "2026-02-01T00:00:00Z",
                                                "1.0.0", "2026-01-01T00:00:00Z"));
    EXPECT_FALSE(PackageDownloaderLib::outranks("1.0.0", "2026-01-01T00:00:00Z",
                                                "1.0.0", "2026-02-01T00:00:00Z"));
}

TEST(Ranking, PreReleaseRanksBelowItsRelease) {
    using lgpd::PackageDownloaderLib;
    EXPECT_FALSE(PackageDownloaderLib::outranks("1.0.0-rc.1", "2026-03-01T00:00:00Z",
                                                "1.0.0",      "2026-01-01T00:00:00Z"));
    // ...and rc.11 outranks rc.2, which a string compare gets backwards.
    EXPECT_TRUE (PackageDownloaderLib::outranks("1.0.0-rc.11", "2026-01-01T00:00:00Z",
                                                "1.0.0-rc.2",  "2026-01-01T00:00:00Z"));
}

// ─── Resolver: top-level pin wins over a transitive re-resolve (issue 2) ──────

// REGRESSION. A module that is BOTH explicitly pinned at top level AND a
// dependency of another pinned package must resolve to ONE entry, at the pin —
// not a second entry at the newest catalog version. The duplicate newest-copy
// is what made the app-manager version dropdown snap back to the latest release.
TEST(Resolver, TopLevelPinNotReResolvedAsTransitiveDep) {
    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(catalogFetcher(json{{"name", "blockchain_module"}, {"version", "*"}}));

    // The app-manager pins BOTH the app and the module (both top-level inputs).
    const std::string input = json::array({
        json{{"name", "blockchain_ui"},     {"version", "0.1.0"}},
        json{{"name", "blockchain_module"}, {"version", "0.1.0"}},
    }).dump();

    const std::string raw = lib.resolveDependenciesJson(input);
    const auto byName = resolvedVersions(raw);

    ASSERT_EQ(byName.count("blockchain_module"), 1u) << "raw resolver output: " << raw;
    // Exactly one entry, and it's the pinned 0.1.0 — NOT a duplicate 0.2.0.
    EXPECT_EQ(byName.at("blockchain_module"), (std::vector<std::string>{"0.1.0"}))
        << "pinned module was re-resolved to newest as a transitive dep; raw: " << raw;
}

// Control: with the module NOT pinned at top level, it still resolves
// transitively to the newest matching version (normal behaviour preserved).
TEST(Resolver, UnpinnedTransitiveDepStillResolvesToNewest) {
    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(catalogFetcher(json{{"name", "blockchain_module"}, {"version", "*"}}));

    const std::string input = json::array({
        json{{"name", "blockchain_ui"}, {"version", "0.1.0"}},
    }).dump();

    const auto byName = resolvedVersions(lib.resolveDependenciesJson(input));
    ASSERT_EQ(byName.count("blockchain_module"), 1u);
    EXPECT_EQ(byName.at("blockchain_module"), (std::vector<std::string>{"0.2.0"}));
}

// ─── Repository registry (in-memory) ─────────────────────────────────────────

TEST(Registry, DefaultIsPresentWhenEnabled) {
    lgpd::PackageDownloaderLib lib;
    auto repos = lib.registry().list();
    ASSERT_FALSE(repos.empty());
    EXPECT_TRUE(repos.front().isDefault);
    EXPECT_TRUE(repos.front().enabled);
}

TEST(Registry, MutationsRequireConfig) {
    lgpd::PackageDownloaderLib lib;
    auto err = lib.registry().addRepository("https://example.com/logos-repo.json");
    EXPECT_FALSE(err.empty());
    // remove of the default also requires a config-backed registry
    err = lib.registry().removeRepository(lgpd::kDefaultRepositoryUrl);
    EXPECT_FALSE(err.empty());
}

// The disabled default stays in list() marked enabled=false so Settings can
// render it as a toggled-off row rather than silently dropping it. The
// defaultDisabled config flag remains authoritative for catalog participation.
TEST(Registry, DisableDefaultKeepsInListMarkedDisabled) {
    fs::path cfg = fs::temp_directory_path() / ("lgpd_test_cfg_" + std::to_string(std::rand()) + ".json");
    {
        lgpd::PackageDownloaderLib lib(cfg.string());
        auto err = lib.registry().setEnabled(lgpd::kDefaultRepositoryUrl, false);
        EXPECT_TRUE(err.empty()) << err;
        auto repos = lib.registry().list();
        ASSERT_EQ(repos.size(), 1u);
        EXPECT_TRUE(repos.front().isDefault);
        EXPECT_FALSE(repos.front().enabled);
    }
    ASSERT_TRUE(fs::exists(cfg));
    {
        // Config on disk carries defaultDisabled: true.
        std::ifstream in(cfg);
        json j; in >> j;
        EXPECT_TRUE(j.value("defaultDisabled", false));

        lgpd::PackageDownloaderLib lib2(cfg.string());
        auto repos = lib2.registry().list();
        ASSERT_EQ(repos.size(), 1u);
        EXPECT_TRUE(repos.front().isDefault);
        EXPECT_FALSE(repos.front().enabled);
    }
    std::error_code ec; fs::remove(cfg, ec);
}

// removeRepository on the default URL is a hard delete — the row leaves
// list() entirely, and the caller must re-add by URL to restore it.
// Disable (setEnabled) is a separate state — see
// DisableDefaultKeepsInListMarkedDisabled above.
TEST(Registry, RemoveDefaultOmitsFromListAndReAddRestoresIt) {
    fs::path cfg = fs::temp_directory_path() / ("lgpd_test_rm_" + std::to_string(std::rand()) + ".json");
    auto mock = std::make_shared<MockFetcher>();
    mock->repoJson = json{{"schemaVersion", 1}, {"name", "test"},
                          {"displayName", "Test"}, {"indexUrl", kIndexUrl},
                          {"trustedSigners", json::array()}}.dump();
    mock->indexJson = json{{"schemaVersion", 2}, {"repositoryName", "test"},
                           {"packages", json::array()}}.dump();
    {
        lgpd::PackageDownloaderLib lib(cfg.string());
        lib.setFetcher(mock);
        {
            auto repos = lib.registry().list();
            ASSERT_EQ(repos.size(), 1u);
            EXPECT_TRUE(repos.front().enabled);
        }

        auto err = lib.registry().removeRepository(lgpd::kDefaultRepositoryUrl);
        EXPECT_TRUE(err.empty()) << err;
        EXPECT_TRUE(lib.registry().list().empty());

        // Re-removing an already-removed default returns "not registered"
        // (parity with a user repo that isn't present).
        err = lib.registry().removeRepository(lgpd::kDefaultRepositoryUrl);
        EXPECT_FALSE(err.empty());
        EXPECT_NE(err.find("not registered"), std::string::npos);

        // Toggling a removed default is also an error — the caller must
        // re-add first.
        err = lib.registry().setEnabled(lgpd::kDefaultRepositoryUrl, false);
        EXPECT_FALSE(err.empty());
        EXPECT_NE(err.find("not registered"), std::string::npos);

        // Re-add by the hardcoded default URL restores present + enabled.
        err = lib.registry().addRepository(lgpd::kDefaultRepositoryUrl);
        EXPECT_TRUE(err.empty()) << err;
        {
            auto repos = lib.registry().list();
            ASSERT_EQ(repos.size(), 1u);
            EXPECT_TRUE(repos.front().isDefault);
            EXPECT_TRUE(repos.front().enabled);
            EXPECT_EQ(repos.front().url, lgpd::kDefaultRepositoryUrl);
        }

        // Adding again while present-and-enabled is rejected.
        err = lib.registry().addRepository(lgpd::kDefaultRepositoryUrl);
        EXPECT_FALSE(err.empty());
        EXPECT_NE(err.find("already registered"), std::string::npos);
    }
    // Persistence: remove-then-reload — the fresh client sees an empty list.
    {
        lgpd::PackageDownloaderLib lib(cfg.string());
        lib.setFetcher(mock);
        {
            auto repos = lib.registry().list();
            ASSERT_EQ(repos.size(), 1u);
            EXPECT_TRUE(repos.front().enabled);
        }
        auto err = lib.registry().removeRepository(lgpd::kDefaultRepositoryUrl);
        EXPECT_TRUE(err.empty()) << err;
    }
    {
        lgpd::PackageDownloaderLib lib2(cfg.string());
        lib2.setFetcher(mock);
        EXPECT_TRUE(lib2.registry().list().empty());

        auto err = lib2.registry().addRepository(lgpd::kDefaultRepositoryUrl);
        EXPECT_TRUE(err.empty()) << err;
        auto after = lib2.registry().list();
        ASSERT_EQ(after.size(), 1u);
        EXPECT_TRUE(after.front().isDefault);
        EXPECT_TRUE(after.front().enabled);
    }
    std::error_code ec; fs::remove(cfg, ec);
}

// User (external) repo lifecycle: add → disable → enable → remove → re-add.
// Covers the symmetry with the default's flags — a disable keeps the row
// in list() with enabled=false; a remove takes it out; a re-add restores
// it with enabled=true. Duplicate add is rejected.
TEST(Registry, UserRepoLifecycle) {
    fs::path cfg = fs::temp_directory_path() / ("lgpd_test_user_" + std::to_string(std::rand()) + ".json");
    const std::string url = "https://example.com/ext/logos-repo.json";

    // A URL-agnostic fetcher: any logos-repo.json request gets the same
    // repo manifest; any request to that manifest's indexUrl gets an empty
    // package list. This keeps the mock small and covers both the default
    // URL and the user URL added below.
    class AllUrlsFetcher : public lgpd::Fetcher {
    public:
        std::string repoJson;
        std::string indexJson;
        bool get(const std::string& u, std::string& out) override {
            if (u == kIndexUrl) { out = indexJson; return true; }
            // Any logos-repo.json variant, including the default.
            out = repoJson; return true;
        }
        bool getToFile(const std::string&, const std::string&) override { return false; }
    };
    auto mock = std::make_shared<AllUrlsFetcher>();
    mock->repoJson = json{{"schemaVersion", 1}, {"name", "ext"},
                          {"displayName", "External"}, {"indexUrl", kIndexUrl},
                          {"trustedSigners", json::array()}}.dump();
    mock->indexJson = json{{"schemaVersion", 2}, {"repositoryName", "ext"},
                           {"packages", json::array()}}.dump();
    lgpd::PackageDownloaderLib lib(cfg.string());
    lib.setFetcher(mock);

    // Add.
    auto err = lib.registry().addRepository(url);
    ASSERT_TRUE(err.empty()) << err;
    {
        auto repos = lib.registry().list();
        ASSERT_EQ(repos.size(), 2u);   // default + user
        // User row is the second one (default is first).
        EXPECT_EQ(repos[1].url, url);
        EXPECT_TRUE(repos[1].enabled);
        EXPECT_FALSE(repos[1].isDefault);
    }

    // Add-duplicate.
    err = lib.registry().addRepository(url);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("already registered"), std::string::npos);

    // Disable keeps in list, flips enabled=false.
    err = lib.registry().setEnabled(url, false);
    ASSERT_TRUE(err.empty()) << err;
    {
        auto repos = lib.registry().list();
        ASSERT_EQ(repos.size(), 2u);
        EXPECT_EQ(repos[1].url, url);
        EXPECT_FALSE(repos[1].enabled);
    }

    // Enable again.
    err = lib.registry().setEnabled(url, true);
    ASSERT_TRUE(err.empty()) << err;
    EXPECT_TRUE(lib.registry().list().at(1).enabled);

    // Remove drops it from the list.
    err = lib.registry().removeRepository(url);
    ASSERT_TRUE(err.empty()) << err;
    ASSERT_EQ(lib.registry().list().size(), 1u);
    EXPECT_TRUE(lib.registry().list().front().isDefault);

    // Removing again reports not-registered.
    err = lib.registry().removeRepository(url);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("not registered"), std::string::npos);

    // setEnabled on an absent user repo also errors.
    err = lib.registry().setEnabled(url, true);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("not registered"), std::string::npos);

    // Re-adding the same URL after remove works (enabled=true again).
    err = lib.registry().addRepository(url);
    ASSERT_TRUE(err.empty()) << err;
    {
        auto repos = lib.registry().list();
        ASSERT_EQ(repos.size(), 2u);
        EXPECT_EQ(repos[1].url, url);
        EXPECT_TRUE(repos[1].enabled);
    }

    // Add rejects malformed URLs.
    err = lib.registry().addRepository("");
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("empty"), std::string::npos);
    err = lib.registry().addRepository("http://example.com/insecure/logos-repo.json");
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("https"), std::string::npos);

    std::error_code ec; fs::remove(cfg, ec);
}

// Re-adding a disabled (but not removed) default clears the disabled flag —
// the "add" action means "present and enabled", regardless of prior state.
TEST(Registry, AddDefaultWhileDisabledClearsDisabledFlag) {
    fs::path cfg = fs::temp_directory_path() / ("lgpd_test_ad_" + std::to_string(std::rand()) + ".json");
    lgpd::PackageDownloaderLib lib(cfg.string());
    auto err = lib.registry().setEnabled(lgpd::kDefaultRepositoryUrl, false);
    ASSERT_TRUE(err.empty()) << err;
    ASSERT_EQ(lib.registry().list().size(), 1u);
    ASSERT_FALSE(lib.registry().list().front().enabled);

    err = lib.registry().addRepository(lgpd::kDefaultRepositoryUrl);
    EXPECT_TRUE(err.empty()) << err;
    auto after = lib.registry().list();
    ASSERT_EQ(after.size(), 1u);
    EXPECT_TRUE(after.front().enabled);
    std::error_code ec; fs::remove(cfg, ec);
}

// refresh() must not try to fetch the default when it is disabled OR
// removed. With disable, the row is still in list() but shouldn't consume
// network. With remove, the row is gone. Either way a missing/unreachable
// default must not surface as a refresh error the user can't clear.
TEST(Registry, RefreshSkipsDisabledAndRemovedDefault) {
    auto failing = std::make_shared<MockFetcher>();
    failing->repoJson.clear();
    failing->indexJson.clear();

    // Disabled default: still listed, still skipped by refresh.
    {
        fs::path cfg = fs::temp_directory_path() / ("lgpd_test_ref_d_" + std::to_string(std::rand()) + ".json");
        lgpd::PackageDownloaderLib lib(cfg.string());
        lib.setFetcher(failing);
        auto err = lib.registry().setEnabled(lgpd::kDefaultRepositoryUrl, false);
        ASSERT_TRUE(err.empty()) << err;
        err = lib.registry().refresh();
        EXPECT_TRUE(err.empty()) << "refresh reported errors for a disabled default: " << err;
        std::error_code ec; fs::remove(cfg, ec);
    }
    // Removed default: absent from list(), refresh has nothing to fetch.
    {
        fs::path cfg = fs::temp_directory_path() / ("lgpd_test_ref_r_" + std::to_string(std::rand()) + ".json");
        lgpd::PackageDownloaderLib lib(cfg.string());
        lib.setFetcher(failing);
        auto err = lib.registry().removeRepository(lgpd::kDefaultRepositoryUrl);
        ASSERT_TRUE(err.empty()) << err;
        err = lib.registry().refresh();
        EXPECT_TRUE(err.empty()) << "refresh reported errors for a removed default: " << err;
        std::error_code ec; fs::remove(cfg, ec);
    }
}

TEST(Catalog, ReturnsJsonArrayWhenNoNetwork) {
    // No real network is required because the lib lazy-fetches per repo
    // and degrades to empty results on failure.
    lgpd::PackageDownloaderLib lib;
    json catalog;
    ASSERT_NO_THROW(catalog = json::parse(lib.getCatalogJson()));
    EXPECT_TRUE(catalog.is_array());
}
