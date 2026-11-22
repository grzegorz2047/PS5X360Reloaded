// SPDX-License-Identifier: MIT
// Fix 80 (tools/source-1.0.2/fixes/80-enumerator-release.patch): Battlefield 3 filled the
// guest's 4 KiB heap in about 14 minutes and then read through a null pointer. Fix 79's line
// on 1.0.5_candidate_55 named the holders: enumerators (about 15 a second, created 8,175, freed
// 9) and the game's XamAlloc buffers, one each. The game makes an enumerator with
// XamCreateEnumeratorHandle, takes it with XamGetPrivateEnumStructureFromHandle (a handle
// reference) and gives it back with ObDereferenceObject, which finds the object by its address
// (XObject::GetNativeObject). An enumerator has no dispatch header: SetNativePointer stamps its
// handle where a dispatch header's wait list is (offsets 8 and 12), and XEnumerator::Initialize
// then writes close_message and user_index there. Unstamped, the address was taken for a
// dispatch object of another kind, that one's reference was given back instead, and the
// enumerator kept one for ever. Its enumerators are now found by their address first.
// Checked in the assembled tree (the kernel is not built on the host):
//  (a) the overlap: X_DISPATCH_HEADER's wait list and X_KENUMERATOR's close_message and
//      user_index are at offsets 8 and 12;
//  (b) Initialize records the enumerator by its guest object after writing those fields, the
//      destructor forgets it, and only one with a handle is given out;
//  (c) GetNativeObject asks for an enumerator at the address before the stamp resolver, and
//      gives the lock back when it returns one; ObDereferenceObject and
//      XamGetPrivateEnumStructureFromHandle still pair a handle reference through it.
// Run from the assembled tree's root (run-fix-checks.sh).
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}
std::string Read(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  std::string s = text.str();
  std::string out;
  for (char c : s)
    if (c != '\r') out += c;
  return out;
}
const std::string K = ".deps/xenia-canary/src/xenia/kernel/";

// The byte offsets of a struct's be<uint32_t> fields, in order, from the line after "struct NAME {".
std::vector<std::string> Fields(const std::string& text, const std::string& name) {
  std::vector<std::string> fields;
  const size_t at = text.find("struct " + name + " {");
  if (at == std::string::npos) return fields;
  const size_t end = text.find("};", at);
  std::istringstream body(text.substr(at, end - at));
  std::string line;
  while (std::getline(body, line)) {
    const size_t be = line.find("be<uint32_t> ");
    if (be == std::string::npos) continue;
    std::string field = line.substr(be + 13);
    field = field.substr(0, field.find(';'));
    fields.push_back(field);
  }
  return fields;
}

void CheckOverlap() {
  const std::string object_h = Read(K + "xobject.h");
  const size_t header = object_h.find("// https://www.nirsoft.net/kernel_struct/vista/DISPATCHER_HEADER.html");
  const size_t type = object_h.find("    X_OBJECT_TYPES type;", header);
  size_t bytes = 0, at = type;
  for (int n = 0; n < 3; ++n) {  // three one-byte unions after the type byte
    at = object_h.find("    union {", at + 1);
    if (at != std::string::npos) ++bytes;
  }
  const size_t signal = object_h.find("  xe::be<uint32_t> signal_state;\n  X_LIST_ENTRY wait_list;\n} X_DISPATCH_HEADER;", header);
  Expect(header != std::string::npos && type != std::string::npos && bytes == 3 && signal != std::string::npos &&
             object_h.find("static_assert_size(X_DISPATCH_HEADER, 0x10);") != std::string::npos,
         "X_DISPATCH_HEADER: 4 one-byte fields, signal_state, then the wait list at offset 8");
  Expect(object_h.find("    header->wait_list.flink_ptr = kXObjSignature;\n    header->wait_list.blink_ptr = handle;") !=
             std::string::npos,
         "the stamp is the wait list: the signature at offset 8, the handle at 12");
  const std::string enum_h = Read(K + "xenumerator.h");
  const auto fields = Fields(enum_h, "X_KENUMERATOR");
  Expect(fields.size() >= 4 && fields[0] == "app_id" && fields[1] == "open_message" && fields[2] == "close_message" &&
             fields[3] == "user_index",
         "X_KENUMERATOR: close_message at offset 8, user_index at 12, over the stamp");
}

void CheckEnumerator() {
  const std::string cc = Read(K + "xenumerator.cc");
  const size_t init = cc.find("X_STATUS XEnumerator::Initialize(uint32_t user_index, uint32_t app_id,");
  const size_t create = cc.find("auto native_object = CreateNative(sizeof(X_KENUMERATOR) + extra_size);", init);
  const size_t close = cc.find("guest_object->close_message = close_message;", create);
  const size_t user = cc.find("guest_object->user_index = user_index;", create);
  const size_t record = cc.find("OwnedEnumerators()[this->guest_object()] = this;  // fix 80", create);
  const size_t ret = cc.find("return X_STATUS_SUCCESS;", create);
  Expect(init != std::string::npos && create != std::string::npos && close < record && user < record && record != std::string::npos &&
             record < ret,
         "Initialize records the enumerator by its guest object after writing over the stamp");
  const size_t dtor = cc.find("XEnumerator::~XEnumerator() {");
  const size_t erase = cc.find("OwnedEnumerators().erase(it);", dtor);
  Expect(dtor != std::string::npos && erase != std::string::npos && erase - dtor < 3000, "the destructor forgets it");
  const size_t find = cc.find("XObject* XEnumerator::RetainByGuestObject(uint32_t guest_ptr) {");
  const size_t handles = cc.find("it->second->handles().empty()", find);
  const size_t retain = cc.find("return retain_object<XObject>(it->second).release();", find);
  Expect(find != std::string::npos && handles != std::string::npos && retain != std::string::npos && handles < retain,
         "only an enumerator that still has a handle is given out, retained");
  Expect(cc.find("XEnumerator::~XEnumerator() = default;") == std::string::npos, "no default destructor left");
}

void CheckLookup() {
  const std::string cc = Read(K + "xobject.cc");
  const size_t fn = cc.find("object_ref<XObject> XObject::GetNativeObject(KernelState* kernel_state,");
  const size_t lock = cc.find("global_critical_region::mutex().lock();", fn);
  const size_t owned = cc.find("if (XObject* owned = XEnumerator::RetainByGuestObject(guest_ptr)) {", fn);
  const size_t unlock = cc.find("global_critical_region::mutex().unlock();", owned);
  const size_t give = cc.find("return object_ref<XObject>(owned);", owned);
  const size_t resolve = cc.find("xbox360ps5::native_object::Resolve(kernel, report);", fn);
  Expect(fn != std::string::npos && lock < owned && owned != std::string::npos && unlock < give && give < resolve,
         "GetNativeObject finds an enumerator by its address before the stamp resolver, and unlocks");
  const std::string ob = Read(K + "xboxkrnl/xboxkrnl_ob.cc");
  const size_t deref = ob.find("void xeObDereferenceObject(PPCContext* context, uint32_t native_ptr) {");
  const size_t native = ob.find("auto object = XObject::GetNativeObject<XObject>(", deref);
  const size_t release = ob.find("object->ReleaseHandle();", native);
  Expect(deref != std::string::npos && native != std::string::npos && release != std::string::npos,
         "ObDereferenceObject gives back a handle reference of the object at the address");
  const std::string xam = Read(K + "xam/xam_enum.cc");
  const size_t priv = xam.find("dword_result_t XamGetPrivateEnumStructureFromHandle_entry(");
  Expect(priv != std::string::npos && xam.find("e->RetainHandle();", priv) != std::string::npos,
         "XamGetPrivateEnumStructureFromHandle takes a handle reference");
}

}  // namespace

int main() {
  CheckOverlap();
  CheckEnumerator();
  CheckLookup();
  if (failures) {
    std::printf("check-enumerator-release: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-enumerator-release: ok\n");
  return 0;
}
