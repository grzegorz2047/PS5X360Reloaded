// SPDX-License-Identifier: MIT
// Fix 148b (tools/source-1.0.2/fixes/148b-disc-name-tags.patch): No-Intro names put a tag after the
// disc number, "Grand Theft Auto V (World) (...) (Disc 1) (Install).iso" and "(Disc 2) (Play).iso".
// port/disc_catalog.cpp's DiscHint read only a number at the very end of the name, so these discs
// had no number: the in-game disc swap (fix 51) found no Disc 2 beside Disc 1, and the library
// (fix 89) could group them only by title ID.
//  (a) such names give their disc number and one set name (the family) for every disc of the set;
//  (b) names without a disc number, with two tags, or with a tag that holds a digit stay as before;
//      fix 89's names ("Disc 1", "(Disc 2)", "- Disk 3", "[Disco 2]", "Game_Disc1") are unchanged.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "../port/disc_catalog.cpp"

#include <cstdio>
#include <string>

namespace {
namespace md = xbox360ps5::multidisc;

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}

void Tagged() {
  const std::string gta = "Grand Theft Auto V (World) (En,Fr,De,Es,It,Pt,Zh,Ko,Pl,Ru)";
  Expect(ps5x360_disc::DiscNumberFromName(gta + " (Disc 1) (Install).iso") == 1, "(a) (Disc 1) (Install)");
  Expect(ps5x360_disc::DiscNumberFromName(gta + " (Disc 2) (Play).iso") == 2, "(a) (Disc 2) (Play)");
  Expect(md::NameWithoutDisc(gta + " (Disc 1) (Install).iso") ==
             md::NameWithoutDisc(gta + " (Disc 2) (Play).iso"),
         "(a) both discs have one set name");
  Expect(md::NameWithoutDisc(gta + " (Disc 2) (Play).iso") == gta, "(a) the set name drops disc and tag");
  Expect(ps5x360_disc::DiscNumberFromName("Forza Motorsport 4 (Disc 2) (Content)") == 2, "(a) (Content)");
  Expect(ps5x360_disc::DiscNumberFromName("Game [Disc 1] [Install]") == 1, "(a) square brackets");
  Expect(ps5x360_disc::DiscNumberFromName("Game (Disc 3) (Bonus Disc)") == 3, "(a) a tag of two words");
}

void Unchanged() {
  Expect(ps5x360_disc::DiscNumberFromName("Lost Odyssey (Install)") == 0, "(b) a tag without a disc number");
  Expect(ps5x360_disc::DiscNumberFromName("Halo 3 (Install)") == 0, "(b) a number that is no disc, then a tag");
  Expect(ps5x360_disc::DiscNumberFromName("Game (Disc 1) (Install) (Rev 1)") == 0, "(b) two tags");
  Expect(ps5x360_disc::DiscNumberFromName("Game (Disc 1) (v2)") == 0, "(b) a tag with a digit");
  Expect(ps5x360_disc::DiscNumberFromName("Battlefield 3 (Disc 1)") == 1, "(b) (Disc 1)");
  Expect(ps5x360_disc::DiscNumberFromName("Final Fantasy XIII - Disk 3") == 3, "(b) - Disk 3");
  Expect(ps5x360_disc::DiscNumberFromName("Juego [Disco 2]") == 2, "(b) [Disco 2]");
  Expect(ps5x360_disc::DiscNumberFromName("Game_Disc1") == 1, "(b) Game_Disc1");
  Expect(ps5x360_disc::DiscNumberFromName("Discovery 2") == 0, "(b) Discovery 2");
  Expect(md::NameWithoutDisc("Battlefield 3 (Disc 1)") == "Battlefield 3", "(b) set name unchanged");
}

}  // namespace

int main() {
  Tagged();
  Unchanged();
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("disc name tags: all checks passed\n");
  return 0;
}
