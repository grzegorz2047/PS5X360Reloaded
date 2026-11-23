// SPDX-License-Identifier: MIT
// Fix 147c (tools/source-1.0.2/fixes/147c-harmonix-keys-upload.patch): the Harmonix music keys
// that Dance Central 3's encrypted loading music needs are never part of PS5X360Reloaded; the
// player uploads them on the settings server's /keys page, and only SHA-256 fingerprints are built
// in.
//  (a) harmonix_keys.hpp: SHA-256 (FIPS 180-4 vectors); four fingerprints, each a title key's and its
//      key's, 64 lowercase hex digits, all different;
//  (b) ParseKeys: the HvKeys array of a keys.h-shaped text (made-up bytes here), else every run of
//      exactly 32 hex digits ("hv1=..." lines too), nothing from longer runs or words;
//  (c) keys no fingerprint knows are refused (Accept counts them), a key file line with a wrong key is
//      skipped, Serialize writes "hv<n>=" lines; no unknown title key is taken for a Harmonix one;
//  (d) no key in the tree or the repository: every 16 bytes of every hex-byte list and hex string in
//      the sources, patches, docs and language files are hashed against the fingerprints;
//  (e) the assembled tree: XeKeysSetKey takes the uploaded key, the /keys page and /api/keys, the
//      overlay, and the texts in every language file.
// Optional, by hand: check-harmonix-keys <repo> --keys <keys.h> accepts four of its HvKeys.
// Built against the assembled 1.0.2 tree and run from its root with the repository as argument.
#undef NDEBUG
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/harmonix_keys.hpp"

namespace {
namespace fs = std::filesystem;
namespace hk = xbox360ps5::harmonix_keys;

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}
std::string Read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}
std::string Sha(const std::string& text) {
  const auto d = hk::Sha256(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  return hk::Hex(d.data(), d.size());
}

void Hashing() {
  Expect(Sha("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "(a) SHA-256 of abc");
  Expect(Sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "(a) SHA-256 of nothing");
  Expect(Sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
             "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
         "(a) SHA-256 of two blocks");
  std::set<std::string> seen;
  for (const auto& f : hk::kFingerprints) {
    for (const char* h : {f.title_sha256, f.key_sha256}) {
      const std::string s = h;
      bool hex = s.size() == 64;
      for (char c : s) hex = hex && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
      Expect(hex, "(a) a fingerprint is 64 lowercase hex digits");
      seen.insert(s);
    }
  }
  Expect(hk::kKeys == 4 && seen.size() == 8, "(a) four pairs, eight different fingerprints");
}

void Parsing() {
  const std::string header =
      "const unsigned char ctrKey0B[16] = {0x10, 0x11};\n"
      "const unsigned char HvKeys[48] = {\n"
      " 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF,\n"
      " 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF,\n"
      " 0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF\n"
      "};\nconst unsigned char hiddenKeys[32] = {0x7F, 0x95};\n";
  const auto keys = hk::ParseKeys(header);
  Expect(keys.size() == 3 && keys[0][0] == 0xA0 && keys[0][15] == 0xAF && keys[2][0] == 0xC0 && keys[2][15] == 0xCF,
         "(b) the HvKeys array of a keys.h, in groups of 16");
  const auto lines = hk::ParseKeys(
      "hv1=000102030405060708090a0b0c0d0e0f\n"
      "FFEEDDCCBBAA99887766554433221100\n"
      "too short 000102030405060708090a0b0c0d0e\n"
      "too long 000102030405060708090a0b0c0d0e0f00\n"
      "word x000102030405060708090a0b0c0d0e0f\n");
  Expect(lines.size() == 2 && lines[0][1] == 0x01 && lines[1][0] == 0xFF,
         "(b) runs of exactly 32 hex digits, nothing else");
}

void Refusing() {
  hk::Set set{};
  hk::Key fake{};
  for (int i = 0; i < 16; ++i) fake[size_t(i)] = uint8_t(0x30 + i);
  size_t refused = 0;
  Expect(hk::Accept(set, {fake, fake}, &refused) == 0 && refused == 2 && hk::Count(set) == 0,
         "(c) keys no fingerprint knows are refused");
  Expect(hk::HvKeyIndex(fake) == -1 && hk::TitleKeyIndex(fake.data()) == -1 && hk::TitleKeyIndex(nullptr) == -1,
         "(c) an unknown key is no Harmonix key");
  set[1] = fake;
  const std::string file = hk::Serialize(set);
  Expect(file.find("hv1=303132333435363738393a3b3c3d3e3f") != std::string::npos, "(c) Serialize writes hv<n>= lines");
  Expect(hk::Count(hk::ParseFile(file)) == 0, "(c) a key file line with a wrong key is skipped");
  Expect(hk::KeyFile("/data/homebrew/PPSA83600") == "/data/homebrew/PPSA83600/keys/harmonix-mogg.txt", "(c) the file");
  Expect(hk::PageUrl("http://10.0.0.2:8360/", "abc123") == "http://10.0.0.2:8360/keys?k=abc123" &&
             hk::PageUrl("", "abc").empty(),
         "(c) the page's link");
}

// Every 16-byte window of every hex-byte list ("0x12, 0x34") and hex string in a text.
void ScanText(const std::string& text, const std::set<std::string>& prints, const fs::path& path, size_t* found) {
  std::vector<std::vector<uint8_t>> runs(1);
  const auto end_run = [&] { if (!runs.back().empty()) runs.emplace_back(); };
  for (size_t i = 0; i < text.size();) {
    if (text[i] == '0' && i + 3 < text.size() && (text[i + 1] == 'x' || text[i + 1] == 'X') &&
        hk::Nibble(text[i + 2]) >= 0 && hk::Nibble(text[i + 3]) >= 0 &&
        (i + 4 >= text.size() || hk::Nibble(text[i + 4]) < 0)) {
      runs.back().push_back(uint8_t(hk::Nibble(text[i + 2]) << 4 | hk::Nibble(text[i + 3])));
      i += 4;
      while (i < text.size() && (text[i] == ',' || text[i] == ' ' || text[i] == '\n' || text[i] == '\r' || text[i] == '\t')) ++i;
      continue;
    }
    size_t run = 0;
    while (i + run < text.size() && hk::Nibble(text[i + run]) >= 0) ++run;
    if (run >= 32) {
      end_run();
      for (size_t b = 0; b + 1 < run; b += 2)
        runs.back().push_back(uint8_t(hk::Nibble(text[i + b]) << 4 | hk::Nibble(text[i + b + 1])));
      end_run();
      i += run;
      continue;
    }
    end_run();
    i += run ? run : 1;
  }
  for (const auto& bytes : runs) {
    for (size_t at = 0; at + 16 <= bytes.size(); ++at) {
      const auto d = hk::Sha256(&bytes[at], 16);
      if (prints.count(hk::Hex(d.data(), d.size()))) {
        std::printf("FAIL: (d) a Harmonix key in %s\n", path.string().c_str());
        ++*found;
      }
    }
  }
}
void NoKeys(const std::vector<fs::path>& roots) {
  std::set<std::string> prints;
  for (const auto& f : hk::kFingerprints) { prints.insert(f.title_sha256); prints.insert(f.key_sha256); }
  const std::set<std::string> text = {".cc", ".cpp", ".h", ".hpp", ".c", ".patch", ".md", ".txt", ".py", ".toml",
                                      ".json", ".html", ".js", ".mjs", ".sh", ".yml", ".inc"};
  size_t files = 0, found = 0;
  for (const auto& root : roots) {
    std::error_code error;
    if (!fs::exists(root, error)) continue;
    for (fs::recursive_directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
      const auto& path = it->path();
      if (it->is_directory(error) && (path.filename() == ".git" || path.filename() == "third_party" ||
                                      path.filename() == "build" || path.filename() == ".deps")) {
        it.disable_recursion_pending();
        continue;
      }
      if (!it->is_regular_file(error) || !text.count(path.extension().string())) continue;
      if (it->file_size(error) > (8u << 20)) continue;
      ScanText(Read(path), prints, path, &found);
      ++files;
    }
  }
  std::printf("(d) %zu files scanned for Harmonix keys\n", files);
  Expect(files > 100, "(d) the scan saw the sources");
  Expect(found == 0, "(d) no Harmonix key anywhere");
}

void Tree(const fs::path& repo) {
  const std::string crypt = Read(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_crypt.cc");
  const size_t set = crypt.find("dword_result_t XeKeysSetKey_entry(");
  Expect(set != std::string::npos && crypt.find("hk::TitleKeyIndex(given)", set) != std::string::npos &&
             crypt.find("hk::Load(hk::KeyFile(XBOX360PS5_DATA_ROOT))", set) != std::string::npos &&
             crypt.find("hk::needed.store(", set) != std::string::npos &&
             crypt.find("uploaded ? uploaded->data() : given", set) != std::string::npos,
         "(e) XeKeysSetKey takes the uploaded key of a Harmonix title key and asks for missing ones");
  const std::string web = Read("platform/ps5/web_settings.cpp");
  const size_t key_check = web.find("web_key::Accept(given, key, official_key");
  const size_t page = web.find("target == \"/keys\" && method == \"GET\"");
  const size_t api = web.find("target == \"/api/keys\" && method == \"POST\"");
  Expect(key_check != std::string::npos && page > key_check && api > key_check,
         "(e) /keys and /api/keys need the pairing code");
  Expect(api != std::string::npos && web.find("hk::Accept(set, hk::ParseKeys(", api) != std::string::npos &&
             web.find("hk::Serialize(set)", api) != std::string::npos,
         "(e) /api/keys keeps only fingerprinted keys");
  Expect(web.find("hk::kWhere") != std::string::npos && web.find("__KEYS_BOOT__") != std::string::npos,
         "(e) the page names where the keys are public");
  const std::string game = Read("src/canary_game_main.cpp");
  Expect(game.find("xbox360ps5::harmonix_keys::needed.load()") != std::string::npos &&
             game.find("void DrawKeysPanel(") != std::string::npos &&
             game.find("harmonix_keys::PageUrl(") != std::string::npos &&
             game.find("harmonix_keys::kWhere") != std::string::npos,
         "(e) the overlay with the page's QR code and where the keys are");
  const char* texts[] = {"Music\\skeys\\sneeded", "Where\\sto\\sget\\sthem:", "Harmonix\\smusic\\skeys",
                         "Keys\\ssaved:\\s%u\\sof\\s%u.\\sRestart\\sthe\\sgame\\sto\\suse\\sthem.",
                         "Keys\\srefused\\s(no\\sfingerprint\\smatches):\\s%u"};
  for (const char* language : {"pl", "de", "fr", "es", "es-419", "it", "ja", "ko", "nl", "pt", "pt-BR", "ru", "tr",
                               "zh-Hans", "zh-Hant"}) {
    const std::string file = Read(repo / "tools/source-1.0.2/english/lang" / (std::string(language) + ".txt"));
    for (const char* t : texts)
      Expect(file.find(std::string("\n") + t + " = ") != std::string::npos,
             std::string("(e) ") + language + " translates " + t);
  }
}

}  // namespace

int main(int argc, char** argv) {
  const fs::path repo = argc > 1 ? fs::path(argv[1]) : fs::path("..");
  if (argc > 3 && !std::strcmp(argv[2], "--keys")) {
    hk::Set set{};
    size_t refused = 0;
    const size_t accepted = hk::Accept(set, hk::ParseKeys(Read(argv[3])), &refused);
    std::printf("keys.h: %zu accepted, %zu refused, %zu of %zu indexes filled\n", accepted, refused, hk::Count(set),
                hk::kKeys);
    return accepted == 4 && hk::Count(set) == 4 ? 0 : 1;
  }
  Hashing();
  Parsing();
  Refusing();
  NoKeys({repo / "tools", repo / "docs", repo / "include", repo / "src", repo / "assets", fs::path("include"),
          fs::path("src"), fs::path("platform"), fs::path("port"), fs::path(".deps/xenia-canary/src/xenia/kernel")});
  if (fs::exists(".deps/xenia-canary")) Tree(repo);
  else std::printf("note: no assembled tree here, tree checks skipped\n");
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("harmonix keys: all checks passed\n");
  return 0;
}
