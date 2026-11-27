// SPDX-License-Identifier: MIT
// Fix 187 (assets/patches/57520826 - Harry Potter for Kinect.patch.toml; issue 76): Harry Potter
// for Kinect's Charms lessons that wait forever for a spoken spell time out like the others.
//
// Charms class runs through modes (table 82D0388C): 0 PlayerEntering, 1 VerbalInstructing,
// 2 ListeningForVerbal, 3 ReactingToVerbal, 4 SkippingVerbal, 5 PosingInstructing, 6 LookingForPose,
// 7 ReactingToPose. The listening update (82390138) counts down +0xD74 (22 s, then 25 s) with two
// re-prompts (+0xDDC), then sets +0xB78 = 0, and the next update switches to SkippingVerbal and on to
// the pose. The lesson set-up (8238AD28) sets +0xDB1 for lessons 66F00010..66F00013 and +0xDC1 for
// 6790000A; while either is set, two branches (82390224, 82390230) go past the countdown (82390300),
// so those lessons wait for the spoken spell forever. Fix 178's virtual microphone hears silence, so
// a player without a microphone stays there. "Spoken spells time out without a microphone" turns the
// two branches into nops.
//  (a) the patch file: title 57520826, the one executable hash it was read from, one patch, on, two
//      be32 writes (82390224 and 82390230 = 60000000) and nothing else;
//  (b) a model of the listening update: lessons with both flags clear take exactly the same steps
//      with and without the patch (the branches were never taken there); flagged lessons now reach
//      SkippingVerbal after two re-prompts instead of waiting forever; a spell heard first is still
//      a spell heard (ReactingToVerbal);
//  (c) the title's patch loader (src/game_patches.cpp, from the repository or an assembled tree)
//      applies a file only to an executable whose hash it lists, so no other title or version is
//      touched;
//  (d) with XBOX360PS5_HP_IMAGE naming an unpacked default.xex image (base 82000000; the game is
//      never in the repository): the code shape the two writes replace (lbz +0xDB1 / +0xDC1, cmplwi,
//      bne to 82390300, then the +0xD74 countdown), the lesson set-up that sets the flags for those
//      five lessons only, and the mode table's ListeningForVerbal entry.
// Run from the repository root (run-fix-checks.sh passes the repository).
#undef NDEBUG
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;

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
size_t Count(const std::string& text, const std::string& what) {
  size_t n = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) ++n;
  return n;
}

void PatchFile(const fs::path& repo) {
  const std::string toml = Read(repo / "assets/patches/57520826 - Harry Potter for Kinect.patch.toml");
  Expect(!toml.empty(), "(a) the patch file is there");
  Expect(toml.find("title_id = \"57520826\"") != std::string::npos &&
             Count(toml, "hash = ") == 1 && toml.find("hash = \"BA2E557F754ED9C1\"") != std::string::npos,
         "(a) title 57520826, one executable hash (BA2E557F754ED9C1)");
  Expect(Count(toml, "[[patch]]") == 1 && Count(toml, "is_enabled = true") == 1 &&
             Count(toml, "is_enabled") == 1 &&
             toml.find("name = \"Spoken spells time out without a microphone\"") != std::string::npos,
         "(a) one patch, on, \"Spoken spells time out without a microphone\"");
  Expect(Count(toml, "[[patch.be32]]") == 2 && Count(toml, "address = ") == 2 &&
             Count(toml, "value = 0x60000000") == 2,
         "(a) two be32 writes, both nop");
  const size_t first = toml.find("address = 0x82390224");
  const size_t second = toml.find("address = 0x82390230");
  Expect(first != std::string::npos && second != std::string::npos && first < second,
         "(a) the writes are 82390224 and 82390230, nothing else");
}

// ---- (b) the listening update, modelled -------------------------------------------------------
enum class Mode { Listening, Reacting, Skipping };
struct Lesson {
  bool db1 = false, dc1 = false;  // +0xDB1, +0xDC1 (lesson set-up)
};
// Runs the listening step for `seconds` of silence (or until a spell is heard at `heard_at`) and
// returns the mode it ends in. One update per 1/30 s. `patched`: the two branches are nops.
Mode Listen(const Lesson& lesson, bool patched, double seconds, double heard_at = -1) {
  double countdown = 22.0;  // +0xD74 when the step starts
  int retries = 0;          // +0xDDC
  float b78 = 1.0f;         // +0xB78 (> 0 while listening)
  const double dt = 1.0 / 30;
  for (double t = 0; t < seconds; t += dt) {
    if (heard_at >= 0 && t >= heard_at) return Mode::Reacting;  // +0xD60 < 1: a result came
    if (b78 <= 0) return Mode::Skipping;                         // 823901A0..823901D4
    const bool bypass = !patched && (lesson.db1 || lesson.dc1);  // 82390224 / 82390230
    if (bypass) continue;                                        // to 82390300: no countdown
    countdown -= dt;                                             // 82390238..82390254
    if (countdown > 0) continue;
    if (retries == 0) { retries = 1; countdown = 22.0; }         // re-prompt (mode 2 again)
    else if (retries == 1) { retries = 2; countdown = 25.0; }    // re-prompt
    else b78 = 0;                                                // 823902E8
  }
  return Mode::Listening;
}

void Model() {
  const Lesson plain, db1{true, false}, dc1{false, true}, both{true, true};
  // Plain lessons: the same with and without the patch, at every moment.
  for (double seconds : {10.0, 30.0, 60.0, 69.0, 70.0, 75.0, 120.0})
    Expect(Listen(plain, false, seconds) == Listen(plain, true, seconds),
           "(b) a lesson with both flags clear is unchanged at " + std::to_string(seconds) + " s");
  Expect(Listen(plain, false, 75) == Mode::Skipping, "(b) plain lessons skip after about 70 s");
  // Flagged lessons: forever before, SkippingVerbal after the same countdown with it.
  for (const Lesson& lesson : {db1, dc1, both}) {
    Expect(Listen(lesson, false, 600) == Mode::Listening, "(b) a flagged lesson waited forever");
    Expect(Listen(lesson, true, 75) == Mode::Skipping, "(b) a flagged lesson now skips after about 70 s");
    Expect(Listen(lesson, true, 60) == Mode::Listening, "(b) not before the countdown ends");
    Expect(Listen(lesson, true, 75, 5.0) == Mode::Reacting, "(b) a spell heard is still heard");
  }
}

// ---- (c) only that executable ------------------------------------------------------------------
void Loader(const fs::path& repo) {
  std::string loader = Read("src/game_patches.cpp");
  if (loader.empty()) loader = Read(repo / "src/game_patches.cpp");
  if (loader.empty()) {
    std::printf("note: no src/game_patches.cpp here, the loader check is skipped\n");
    return;
  }
  Expect(loader.find("if (hash && std::find(file.hashes.begin(), file.hashes.end(), hash) == file.hashes.end()) continue;") !=
                 std::string::npos &&
             loader.find("if (start != prefix) continue;") != std::string::npos,
         "(c) a patch file applies only to its title and to an executable hash it lists");
}

// ---- (d) the game's code -----------------------------------------------------------------------
uint32_t Word(const std::vector<uint8_t>& image, uint32_t address) {
  const size_t at = address - 0x82000000u;
  return uint32_t(image[at]) << 24 | uint32_t(image[at + 1]) << 16 | uint32_t(image[at + 2]) << 8 | image[at + 3];
}
std::string Text(const std::vector<uint8_t>& image, uint32_t address) {
  std::string out;
  for (size_t at = address - 0x82000000u; at < image.size() && image[at]; ++at) out += char(image[at]);
  return out;
}
void Image(const char* path) {
  const std::string bytes = Read(path);
  const std::vector<uint8_t> image(bytes.begin(), bytes.end());
  Expect(image.size() > 0xF00000, "(d) the image is a whole default.xex memory image");
  if (image.size() <= 0xF00000) return;
  // The two branches and what they skip.
  Expect(Word(image, 0x82390218) == 0x897F0DB1 && Word(image, 0x82390220) == 0x2B0B0000 &&
             Word(image, 0x82390224) == 0x409A00DC && Word(image, 0x82390228) == 0x897F0DC1 &&
             Word(image, 0x8239022C) == 0x2B0B0000 && Word(image, 0x82390230) == 0x409A00D0,
         "(d) lbz +0xDB1 / +0xDC1, cmplwi 0, bne: the words the patch replaces");
  Expect(0x82390224 + 0xDC == 0x82390300 && 0x82390230 + 0xD0 == 0x82390300 &&
             Word(image, 0x82390300) == 0x38800000,
         "(d) both go to 82390300 (li r4,0: no countdown)");
  Expect(Word(image, 0x82390238) == 0xC1BF0D74 && Word(image, 0x82390254) == 0xD15F0D74 &&
             Word(image, 0x823902E8) == 0xD3FF0B78,
         "(d) after them the +0xD74 countdown, and the +0xB78 = 0 of the last retry");
  // The lesson set-up: +0xDB1 = 1 only for lessons 66F00010..66F00013.
  Expect(Word(image, 0x8238ADD0) == 0x3D4066F0 && Word(image, 0x8238ADDC) == 0x61490010 &&
             Word(image, 0x8238ADF4) == 0x995F0DB1 && Word(image, 0x8238AE08) == 0x61490011 &&
             Word(image, 0x8238AE20) == 0x993F0DB1 && Word(image, 0x8238AE3C) == 0x61490012 &&
             Word(image, 0x8238AE54) == 0x993F0DB1 && Word(image, 0x8238AE70) == 0x61490013 &&
             Word(image, 0x8238AE88) == 0x997F0DB1,
         "(d) +0xDB1 is set for lessons 66F00010..66F00013");
  Expect(Text(image, 0x8202D458) == "XSportMode_CharmsListeningForVerbal" &&
             Word(image, 0x82D038D4) == 0x8202D458 && Text(image, 0x8202D414) == "XSportMode_CharmsSkippingVerbal",
         "(d) the Charms mode table names ListeningForVerbal and SkippingVerbal");
}

}  // namespace

int main(int argc, char** argv) {
  const fs::path repo = argc > 1 ? fs::path(argv[1]) : fs::current_path();
  PatchFile(repo);
  Model();
  Loader(repo);
  if (const char* image = std::getenv("XBOX360PS5_HP_IMAGE")) Image(image);
  else std::printf("note: XBOX360PS5_HP_IMAGE not set, the game's code shape is not checked here\n");
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("hp voice skip patch: all checks passed\n");
  return 0;
}
