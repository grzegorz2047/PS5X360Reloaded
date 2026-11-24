// SPDX-License-Identifier: MIT
// Fix 113 (avatar milestone 1): the generated avatar skeleton (include/xbox360ps5/avatar_skeleton.hpp)
// that XamAvatarGetAssets puts in the asset set, with the profile's body shape.
//
//   check-avatar-skeleton [<lang folder>]      run from an assembled tree's root
//
// Checks the joint table against the public hierarchy, the scales a shape gives, the shape's
// file, the set's layout as Kinect Sports reads it (0x82524E38, modelled here: which joints get a
// scale applied to its rig), and in the tree: the stub's wiring, the launcher's avatar sheet and
// its texts in the 16 language files.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/avatar_assets.hpp"
#include "xbox360ps5/avatar_skeleton.hpp"

namespace a = xbox360ps5::avatar_assets;
namespace sk = xbox360ps5::avatar_skeleton;

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

std::string ReadText(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}
uint32_t Word(const uint8_t* at) { return a::Word(at); }
float Float(const uint8_t* at) {
  const uint32_t bits = Word(at);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}
bool Near(float x, float y) { return std::fabs(x - y) < 1e-5f; }

// The published parent of each of the 71 joints (the XNA / ATG order; parents before children).
constexpr uint8_t kPublicParents[sk::kJoints] = {
    0xFF, 0,  0,  0,  0,  1,  2,  2,  3,  3,  1,  6,  5,  6,  5,  8,  5,  8,  5,  14, 12, 11, 16, 15,
    14,   20, 20, 20, 22, 22, 22, 25, 25, 25, 28, 28, 28, 33, 33, 33, 33, 33, 33, 33, 36, 36, 36, 36,
    36,   36, 36, 37, 38, 39, 40, 43, 44, 45, 46, 47, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60};

// Kinect Sports' 0x82524E38 per joint: the scale it gives its rig is the set's x and z with the
// reference's y, or for the arm's scale joints (0x1A 0x1B 0x1D 0x1E 0x20 0x23) the reference's x
// with the set's y and z; it applies it unless it is (1, 1, 1). The reference here is neutral.
bool KinectApplies(const uint8_t* joint, uint32_t index) {
  const bool arm = index == 0x1A || index == 0x1B || index == 0x1D || index == 0x1E || index == 0x20 || index == 0x23;
  const float x = arm ? 1.0f : Float(joint + 0x50), y = arm ? Float(joint + 0x54) : 1.0f, z = Float(joint + 0x58);
  return !(x == 1.0f && y == 1.0f && z == 1.0f);
}
// The set at `address`: how many joints Kinect Sports would apply a scale to; -1 when its read of
// the skeleton (pointer at +0, count, joints) leaves the buffer.
int KinectAppliedJoints(const std::vector<uint8_t>& set, uint32_t address) {
  const uint32_t skeleton = Word(set.data());
  if (skeleton < address || skeleton + 8 > address + set.size()) return -1;
  const uint32_t count = Word(set.data() + (skeleton - address)), joints = Word(set.data() + (skeleton - address) + 4);
  if (!count) return 0;
  if (count != sk::kJoints || joints < address || joints + count * sk::kJointBytes > address + set.size()) return -1;
  int applied = 0;
  for (uint32_t n = 0; n < count; ++n)
    if (KinectApplies(set.data() + (joints - address) + n * sk::kJointBytes, n)) ++applied;
  return applied;
}
std::vector<uint8_t> MakeSet(uint32_t address, const sk::Shape& shape) {
  std::vector<uint8_t> set(sk::kResultBytes, 0);  // the title clears what it asked for
  a::PrepareEmptyAssets(set.data(), address);      // fix 99
  sk::WriteSkeleton(set.data(), address, shape);   // this fix
  return set;
}
std::string Escaped(const std::string& text) {  // a language file's key: "\s" for spaces
  std::string out;
  for (char ch : text) out += ch == ' ' ? std::string("\\s") : std::string(1, ch);
  return out;
}

int main(int argc, char** argv) {
  // The joints: 71, unique names, the published parents, parents before children, scale joints
  // are leaves; the arm's scale joints are the ones Kinect Sports treats as running along x.
  {
    std::set<std::string> names;
    for (uint32_t n = 0; n < sk::kJoints; ++n) {
      names.insert(sk::kSkeleton[n].name);
      CHECK(sk::kSkeleton[n].parent == kPublicParents[n]);
      CHECK(n == 0 ? sk::kSkeleton[n].parent == sk::kNone : sk::kSkeleton[n].parent < n);
      if (sk::IsScaleJoint(n)) CHECK(sk::FirstChild(n) == sk::kNone);
    }
    CHECK(names.size() == sk::kJoints);
    const std::set<uint32_t> scale_joints = {4, 7, 9, 10, 13, 17, 18, 24, 26, 27, 29, 30, 32, 35};
    for (uint32_t n = 0; n < sk::kJoints; ++n) CHECK(sk::IsScaleJoint(n) == (scale_joints.count(n) == 1));
    for (uint32_t n = 0; n < sk::kJoints; ++n)
      CHECK(sk::RunsAlongX(n) == (n == 0x1A || n == 0x1B || n == 0x1D || n == 0x1E || n == 0x20 || n == 0x23));
    CHECK(!std::strcmp(sk::kSkeleton[19].name, "HEAD") && !std::strcmp(sk::kSkeleton[14].name, "NECK"));
    // A standing mannequin: the head above the hips, the feet near the floor, arms out to the sides.
    float head_y = 0, ankle_y = 0;
    for (uint32_t n = 19; n != sk::kNone; n = sk::kSkeleton[n].parent) head_y += sk::kSkeleton[n].y;
    for (uint32_t n = 11; n != sk::kNone; n = sk::kSkeleton[n].parent) ankle_y += sk::kSkeleton[n].y;
    CHECK(head_y > 0.8f && head_y < 1.2f && ankle_y > 0.0f && ankle_y < 0.1f);
    CHECK(sk::kSkeleton[33].x > 0 && sk::kSkeleton[36].x < 0);
    // First child and next sibling follow from the parents.
    CHECK(sk::FirstChild(0) == 1 && sk::NextSibling(1) == 2 && sk::NextSibling(4) == sk::kNone);
    CHECK(sk::FirstChild(33) == 37 && sk::NextSibling(43) == sk::kNone && sk::FirstChild(70) == sk::kNone);
  }
  // Scales: the default shape changes nothing; weight sets the girth, height the length.
  {
    const sk::Shape neutral;
    for (uint32_t n = 0; n < sk::kJoints; ++n) {
      const sk::Scale s = sk::ScaleOf(n, neutral);
      CHECK(s.x == 1.0f && s.y == 1.0f && s.z == 1.0f);
    }
    sk::Shape heavy, thin, tall;
    heavy.weight = 100, thin.weight = -100, tall.height = 100;
    CHECK(Near(sk::ScaleOf(4, heavy).x, 1.25f) && Near(sk::ScaleOf(4, heavy).z, 1.25f) && sk::ScaleOf(4, heavy).y == 1.0f);
    CHECK(Near(sk::ScaleOf(26, heavy).y, 1.25f) && sk::ScaleOf(26, heavy).x == 1.0f);  // an arm: girth in y and z
    CHECK(Near(sk::ScaleOf(4, thin).x, 0.75f));
    CHECK(Near(sk::ScaleOf(13, tall).y, 1.12f) && Near(sk::ScaleOf(32, tall).x, 1.12f) && sk::ScaleOf(13, tall).x == 1.0f);
    for (uint32_t n = 0; n < sk::kJoints; ++n)
      if (!sk::IsScaleJoint(n)) CHECK(sk::ScaleOf(n, heavy).x == 1.0f && sk::ScaleOf(n, tall).y == 1.0f);
    sk::Shape female;
    female.body = 2;
    CHECK(Near(sk::ScaleOf(7, female).x, 1.06f) && Near(sk::ScaleOf(27, female).z, 0.94f) && sk::ScaleOf(13, female).x == 1.0f);
    // Clamped to the sheet's range.
    sk::Shape far;
    far.weight = 1000, far.height = -1000, far.body = 7;
    CHECK(sk::Clamped(far).weight == 100 && sk::Clamped(far).height == -100 && sk::Clamped(far).body == 1);
    CHECK(Near(sk::ScaleOf(4, far).x, 1.25f));
  }
  // The shape's file.
  {
    sk::Shape shape;
    shape.body = 2, shape.height = -30, shape.weight = 70;
    CHECK(sk::Format(shape) == "body=2\nheight=-30\nweight=70\n");
    const sk::Shape back = sk::Parse(sk::Format(shape));
    CHECK(back.body == 2 && back.height == -30 && back.weight == 70);
    const sk::Shape odd = sk::Parse("# comment\nweight=250\nnonsense\nheight=abc\r\nbody=2");
    CHECK(odd.weight == 100 && odd.height == 0 && odd.body == 2);
    CHECK(sk::Parse("").body == 1 && sk::Parse("").weight == 0);
    CHECK(sk::FileName(0xE0000123456789ABull) == "E0000123456789AB.txt");
    // Fix 165: the user's data folder (user_storage.hpp; before 165 the internal storage).
    CHECK(std::string(sk::kFolder) == "/data/homebrew/PPSA83600/avatars" ||
          std::string(sk::kFolder) == "/download0/xbox360ps5/avatars");
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "check-avatar-skeleton";
    std::filesystem::remove_all(folder);
    sk::Shape loaded;
    loaded.weight = 5;
    CHECK(!sk::Load(folder.string(), 0x0009000000000001ull, &loaded) && loaded.weight == 0);
    CHECK(sk::Save(folder.string(), 0x0009000000000001ull, shape));  // makes the folder
    CHECK(sk::Load(folder.string(), 0x0009000000000001ull, &loaded) && loaded.body == 2 && loaded.weight == 70);
    shape.weight = -10;
    CHECK(sk::Save(folder.string(), 0x0009000000000001ull, shape));  // over the old one
    CHECK(sk::Load(folder.string(), 0x0009000000000001ull, &loaded) && loaded.weight == -10);
    CHECK(!std::filesystem::exists(folder / "0009000000000001.txt.new"));
    std::filesystem::remove_all(folder);
  }
  // The set: fix 99's empty set, then the skeleton after it, inside what the title allocates.
  {
    constexpr uint32_t address = 0x40A40000;
    CHECK(sk::kResultBytes >= a::kResultBytes && sk::kSkeletonAt >= a::kClearedBytes);
    CHECK(sk::kJointsAt % 16 == 0 && sk::kJointsAt >= sk::kSkeletonAt + 8);
    sk::Shape heavy;
    heavy.weight = 60;
    std::vector<uint8_t> set = MakeSet(address, heavy);
    CHECK(Word(set.data()) == address + sk::kSkeletonAt);
    CHECK(Word(set.data() + sk::kSkeletonAt) == sk::kJoints && Word(set.data() + sk::kSkeletonAt + 4) == address + sk::kJointsAt);
    CHECK(a::ComponentCount(set.data()) == 0 && a::ComponentArray(set.data()) == 0);  // still no components
    for (uint32_t i = 4; i < sk::kSkeletonAt; ++i) CHECK(set[i] == 0);
    for (uint32_t i = sk::kJointsAt + sk::kJoints * sk::kJointBytes; i < sk::kResultBytes; ++i) CHECK(set[i] == 0);
    for (uint32_t n = 0; n < sk::kJoints; ++n) {
      const uint8_t* joint = set.data() + sk::kJointsAt + n * sk::kJointBytes;
      CHECK(Word(joint) == sk::kSkeleton[n].parent && Word(joint + 4) == sk::FirstChild(n) && Word(joint + 8) == sk::NextSibling(n));
      CHECK(Float(joint + 0x10) == sk::kSkeleton[n].x && Float(joint + 0x14) == sk::kSkeleton[n].y && Float(joint + 0x18) == sk::kSkeleton[n].z);
      CHECK(Float(joint + 0x2C) == 1.0f && Float(joint + 0x4C) == 1.0f && Float(joint + 0x5C) == 1.0f);  // no rotation
      CHECK(!std::memcmp(joint + 0x10, joint + 0x30, 0x20));  // the pose is the bind pose
      const sk::Scale s = sk::ScaleOf(n, heavy);
      CHECK(Float(joint + 0x50) == s.x && Float(joint + 0x54) == s.y && Float(joint + 0x58) == s.z);
    }
    // Kinect Sports: fix 99c's empty skeleton gives its rig nothing, the default shape nothing,
    // weight every scale joint, height alone nothing (it keeps its own lengths), a female body
    // the hips, chest and arms.
    std::vector<uint8_t> empty(sk::kResultBytes, 0);
    a::PrepareEmptyAssets(empty.data(), address);
    CHECK(KinectAppliedJoints(empty, address) == 0);
    CHECK(KinectAppliedJoints(MakeSet(address, sk::Shape()), address) == 0);
    CHECK(KinectAppliedJoints(set, address) == 14);
    sk::Shape tall, female;
    tall.height = 100, female.body = 2;
    CHECK(KinectAppliedJoints(MakeSet(address, tall), address) == 0);
    CHECK(KinectAppliedJoints(MakeSet(address, female), address) == 3 + 1 + 6);
    std::vector<uint8_t> cleared(sk::kResultBytes, 0);  // before fix 99c: no skeleton at all
    CHECK(KinectAppliedJoints(cleared, address) == -1);
  }

  // The tree: the stub's wiring, the launcher's sheet, the texts.
  const std::string xam = ReadText(".deps/xenia-canary/src/xenia/kernel/xam/xam_avatar.cc");
  const std::string launcher = ReadText("src/launcher.cpp"), header = ReadText("include/xbox360ps5/launcher.hpp");
  if (xam.empty() || launcher.empty()) {
    std::printf("no assembled tree here; the wiring not checked\n");
  } else {
    CHECK(xam.find("#include \"xbox360ps5/avatar_skeleton.hpp\"  // fix 113") != std::string::npos);
    const size_t size = xam.find("dword_result_t XamAvatarGetAssetsResultSize_entry(");
    const size_t helper = xam.find("static void WriteGeneratedSkeleton(");
    const size_t get = xam.find("dword_result_t XamAvatarGetAssets_entry(");
    CHECK(size != std::string::npos && helper != std::string::npos && get != std::string::npos && size < helper && helper < get);
    const std::string size_body = xam.substr(size, helper - size);
    const size_t fix99 = size_body.find("*result_buffer_size_ptr = xbox360ps5::avatar_assets::kResultBytes;");
    const size_t ours = size_body.find("*result_buffer_size_ptr = xbox360ps5::avatar_skeleton::kResultBytes;");
    CHECK(fix99 != std::string::npos && ours != std::string::npos && fix99 < ours);  // ours wins
    const std::string helper_body = xam.substr(helper, get - helper);
    CHECK(helper_body.find("load_and_swap<uint64_t>(") != std::string::npos && helper_body.find("+ 0x380") != std::string::npos);
    CHECK(helper_body.find("GetUserProfile(uint32_t(0))") != std::string::npos);
    // 1.0.5-work-fix113b: an owner that is no profile here (Kinect Sports' built-in manifests,
    // E000...48242C3E on the console) does not count; then player 1's profile.
    const size_t owner = helper_body.find("if (xuid && !kernel_state()->xam_state()->GetUserProfile(xuid)) xuid = 0;");
    CHECK(owner != std::string::npos && owner > helper_body.find("+ 0x380") &&
          owner < helper_body.find("GetUserProfile(uint32_t(0))"));
    CHECK(helper_body.find("sk::Load(sk::kFolder, xuid, &shape)") != std::string::npos);
    CHECK(helper_body.find("sk::WriteSkeleton(assets, address, shape);") != std::string::npos);
    CHECK(helper_body.find("\"Avatars: a generated skeleton of {} joints") != std::string::npos);
    const std::string get_body = xam.substr(get, 1600);
    const size_t prepare = get_body.find("PrepareEmptyAssets(");
    const size_t call = get_body.find("WriteGeneratedSkeleton(avatar_metadata_ptr, result_buffer_ptr);");
    const size_t fail = get_body.find("X_ERROR_FUNCTION_FAILED");
    CHECK(prepare != std::string::npos && call != std::string::npos && fail != std::string::npos && prepare < call && call < fail);
    // The launcher: Triangle on a profile opens its avatar, Circle saves it.
    CHECK(header.find("saves, avatar };") != std::string::npos && header.find("void DrawAvatarSheet(Canvas& c);") != std::string::npos);
    CHECK(launcher.find("#include \"xbox360ps5/avatar_skeleton.hpp\"") != std::string::npos);
    const size_t triangle = launcher.find("if (key == Key::triangle && profile_row_ < int(profiles_.size())) {");
    CHECK(triangle != std::string::npos &&
          launcher.find("avatar_skeleton::Load(avatar_skeleton::kFolder, profile.xuid, &shape);", triangle) != std::string::npos);
    const size_t sheet = launcher.find("case Mode::avatar: {");
    CHECK(sheet != std::string::npos &&
          launcher.find("avatar_skeleton::Save(avatar_skeleton::kFolder, avatar_xuid_, shape)", sheet) != std::string::npos);
    CHECK(launcher.find("else if (mode_ == Mode::avatar) DrawAvatarSheet(c);") != std::string::npos);
    CHECK(launcher.find("hint = c.Hint('t', Tr(\"Avatar\"), hint, 1004);") != std::string::npos);
    std::printf("tree: the stub writes the generated skeleton; the launcher has the avatar sheet\n");
  }
  // The sheet's texts in every language file (the argument: tools/source-1.0.2/english/lang).
  const char* const keys[] = {
      "Avatar", "Avatar saved. Games that show avatars use it from their next start.", "Could not save the avatar.",
      "The body shape of this profile's avatar, for games that show the player as an avatar (Kinect Sports). Only the shape for now: the avatar has no clothes or face yet.",
      "Body type", "Female", "Male", "The avatar's build: a female body has slightly wider hips and narrower shoulders.",
      "Height", "Shorter or taller limbs and body.", "Weight", "A thinner or heavier build."};
  if (argc < 2) {
    std::printf("no language folder given; the texts not checked\n");
  } else {
    const char* const codes[] = {"en", "pt-BR", "pt", "es", "es-419", "de", "fr", "it",
                                 "ja", "ko", "nl", "pl", "ru", "tr", "zh-Hans", "zh-Hant"};
    for (const char* code : codes) {
      const std::string lang = ReadText(std::string(argv[1]) + "/" + code + ".txt");
      CHECK(!lang.empty());
      for (const char* key : keys) {
        const bool found = lang.find("\n" + Escaped(key) + " = ") != std::string::npos;
        if (!found) std::printf("  %s.txt lacks \"%s\"\n", code, key);
        CHECK(found);
      }
      if (launcher.size())
        for (const char* key : keys) CHECK(launcher.find(std::string("Tr(\"") + key + "\")") != std::string::npos);
    }
  }
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-avatar-skeleton: all passed\n");
  return 0;
}
