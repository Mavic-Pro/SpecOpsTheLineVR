// Test offline delle funzioni pure (R-T2): nessun gioco, nessun visore.
//   cmake --build build --config Release --target unit_tests && build\Release\unit_tests.exe
#include "pure_utils.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_failed = 0, g_passed = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) {                                                              \
            ++g_passed;                                                          \
        } else {                                                                 \
            ++g_failed;                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                        \
    } while (0)

using namespace vr::pure;

static void TestParseVersion() {
    CHECK(ParseVersion("v0.9.9") == (std::vector<int>{0, 9, 9, 1}));
    CHECK(ParseVersion("0.9.9-beta1") == (std::vector<int>{0, 9, 9, 0}));
    CHECK(ParseVersion("v0.9.9") > ParseVersion("v0.9.9-beta1"));
    CHECK(ParseVersion("v0.9.9-beta1") > ParseVersion("v0.9.8"));
    CHECK(ParseVersion("v0.10.0") > ParseVersion("v0.9.9"));
    CHECK(ParseVersion("1.0") == (std::vector<int>{1, 0, 0, 1}));
    CHECK(ParseVersion("") == (std::vector<int>{0, 0, 0, 1}));
    CHECK(ParseVersion("garbage") == (std::vector<int>{0, 0, 0, 1}));
}

static void TestJson() {
    std::string json = "{\"tag_name\": \"v0.9.9\", \"assets\": [{\"name\":\"d3d9.dll\",\"size\":942592,"
                       "\"digest\":\"sha256:abc\",\"browser_download_url\":\"https://x/y/d3d9.dll\"}]}";
    size_t at = 0;
    CHECK(JsonString(json, "tag_name", 0, &at) == "v0.9.9");
    CHECK(at > 0 && json[at] == '"');
    CHECK(JsonString(json, "missing") == "");
    CHECK(JsonString("{\"a\":1}", "a") == "");  // non e' una stringa
    size_t urlAt = 0;
    std::string url = JsonString(json, "browser_download_url", 0, &urlAt);
    CHECK(url == "https://x/y/d3d9.dll");
    CHECK(JsonNumberBefore(json, "size", 0, urlAt) == 942592);
    CHECK(JsonNumberBefore(json, "size", urlAt, json.size()) == -1);
    size_t d = json.rfind("\"digest\"", urlAt);
    CHECK(d != std::string::npos && JsonString(json, "digest", d) == "sha256:abc");
}

static void TestRevoked() {
    std::string m = "{\"revoked\": [\"0.9.1\", \"v0.9.3\"], \"recommended\": \"v0.9.9\"}";
    CHECK(IsRevoked(m, "0.9.1"));
    CHECK(IsRevoked(m, "0.9.3"));
    CHECK(!IsRevoked(m, "0.9.2"));
    CHECK(!IsRevoked("{}", "0.9.1"));
    CHECK(!IsRevoked("{\"revoked\": [", "0.9.1"));
    CHECK(JsonString(m, "recommended") == "v0.9.9");
}

static void TestCompanions() {
    std::string m = "{\"version\": \"0.9.10\", \"files\": ["
                    "{\"name\": \"openvr_api.dll\", \"size\": 431616, \"sha256\": \"aa\"},"
                    "{\"name\": \"specopsvr.ini\", \"size\": 10, \"sha256\": \"bb\"},"
                    "{\"name\": \"..\\\\evil.bat\", \"size\": 10, \"sha256\": \"cc\"},"
                    "{\"name\": \"sub/x.bat\", \"size\": 10, \"sha256\": \"cc\"},"
                    "{\"name\": \"D3D9.dll\", \"size\": 10, \"sha256\": \"dd\"},"
                    "{\"name\": \"nosha.bat\", \"size\": 10},"
                    "{\"name\": \"rollback.bat\", \"size\": 120, \"sha256\": \"ee\"}]}";
    std::vector<Companion> c = ParseCompanions(m);
    CHECK(c.size() == 2);
    CHECK(c.size() == 2 && c[0].name == "openvr_api.dll" && c[0].size == 431616 && c[0].sha256 == "aa");
    CHECK(c.size() == 2 && c[1].name == "rollback.bat" && c[1].size == 120 && c[1].sha256 == "ee");
    CHECK(ParseCompanions("{}").empty());
    CHECK(ParseCompanions("").empty());
    // Ordine dei campi diverso: sha256 prima di size.
    std::vector<Companion> d = ParseCompanions("{\"files\":[{\"sha256\":\"ff\",\"name\":\"a.ps1\",\"size\":5}]}");
    CHECK(d.size() == 1 && d[0].sha256 == "ff" && d[0].size == 5);
}

static void TestDll32() {
    std::string pe(0x400, '\0');
    pe[0] = 'M';
    pe[1] = 'Z';
    *reinterpret_cast<uint32_t*>(&pe[0x3C]) = 0x100;
    pe[0x100] = 'P';
    pe[0x101] = 'E';
    *reinterpret_cast<uint16_t*>(&pe[0x104]) = 0x014C;
    CHECK(IsDll32(pe));
    *reinterpret_cast<uint16_t*>(&pe[0x104]) = 0x8664;  // x64
    CHECK(!IsDll32(pe));
    CHECK(!IsDll32("MZ"));
    CHECK(!IsDll32(std::string(0x400, 'x')));
}

static void TestNormalize() {
    CHECK(NormalizeConfigLine("Spread ON  # commento") == "spread on");
    CHECK(NormalizeConfigLine("hud 0,5 100") == "hud 0.5 100");
    CHECK(NormalizeConfigLine("marker\tFalse") == "marker off");
    CHECK(NormalizeConfigLine("gesture melee YES") == "gesture melee on");
    CHECK(NormalizeConfigLine("# solo commento") == "# solo commento");
    CHECK(NormalizeConfigLine("ucall pc SetCinematicMode 1 0 0 1 1 1") == "ucall pc SetCinematicMode 1 0 0 1 1 1");
    CHECK(NormalizeConfigLine("giveweapon YWeapon_M4Carbine") == "giveweapon YWeapon_M4Carbine");
    CHECK(NormalizeConfigLine("toast Hello World # keep") == "toast Hello World # keep");
    CHECK(NormalizeConfigLine("") == "");
    CHECK(ConfigKey("gesture grenade on") == "gesture grenade");
    CHECK(ConfigKey("dynres min 65") == "dynres min");
    CHECK(ConfigKey("dynres on") == "dynres");
    CHECK(ConfigKey("hud 0.4 100") == "hud");
}

static void TestMigrate() {
    static const ConfigChange kChanges[] = {{2, "submitpose on", "submitpose off"}, {5, "hud 0.6 150", "hud 0.5 100"}};
    std::vector<std::string> lines = {"submitpose on", "hud 0.6 150"};
    std::string done;
    CHECK(MigrateConfigLines(lines, 5, kChanges, 2, &done));
    CHECK(lines[0] == "submitpose off" && lines[1] == "hud 0.5 100");
    CHECK(lines.back() == "configversion 5");
    CHECK(done.find("submitpose") != std::string::npos);
    // Gia' alla 2: il cambio della 2 non si applica piu', quello della 5 si'.
    lines = {"configversion 2", "submitpose on", "hud 0.6 150"};
    CHECK(MigrateConfigLines(lines, 5, kChanges, 2, &done));
    CHECK(lines[1] == "submitpose on" && lines[2] == "hud 0.5 100" && lines[0] == "configversion 5");
    // Gia' aggiornato: niente da fare.
    lines = {"configversion 5", "hud 0.6 150"};
    CHECK(!MigrateConfigLines(lines, 5, kChanges, 2, &done));
    CHECK(lines[1] == "hud 0.6 150");
}

int main() {
    TestParseVersion();
    TestJson();
    TestRevoked();
    TestCompanions();
    TestDll32();
    TestNormalize();
    TestMigrate();
    std::printf("unit tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
