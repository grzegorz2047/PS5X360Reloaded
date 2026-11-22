// Fix 90: NtQueryInformationFile(XFileXctdCompressionInformation) answers whether a file is XCTD
// compressed (include/xbox360ps5/xctd.hpp).
//
//   check-xctd-query        run from an assembled tree's root
//
// Synthetic headers only (no game data): the XCTD magic gives 1, anything else 0, a short file
// 0; and the tree's query reads the file's first word at offset 0 (the file's position stays),
// answers success with that value, and keeps the old INVALID_PARAMETER for a file it cannot read.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "xbox360ps5/xctd.hpp"

namespace x = xbox360ps5::xctd;

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

std::string ReadText(const char* path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

int main() {
  // An XCTD file: 0x0FF512ED big-endian, then its header (made-up values after the magic).
  const uint8_t xctd[] = {0x0F, 0xF5, 0x12, 0xED, 0x00, 0x00, 0x00, 0x01};
  CHECK(x::IsCompressedHeader(xctd, sizeof(xctd)));
  CHECK(x::Answer(xctd, sizeof(xctd)) == x::kCompressed);
  CHECK(x::Answer(xctd, 4) == x::kCompressed);
  // Plain files: other magics seen on Kinect Sports' disc and a little-endian magic.
  const uint8_t caff[] = {'C', 'A', 'F', 'F', '0', '8'};
  const uint8_t xmv[] = {0x30, 0x26, 0xB2, 0x75};
  const uint8_t swapped[] = {0xED, 0x12, 0xF5, 0x0F};
  const uint8_t near[] = {0x0F, 0xF5, 0x12, 0xEC};
  for (const uint8_t* plain : {caff, xmv, swapped, near}) CHECK(x::Answer(plain, 4) == x::kNotCompressed);
  // Shorter than the magic (an empty or 3-byte file): not compressed.
  CHECK(x::Answer(xctd, 3) == x::kNotCompressed);
  CHECK(x::Answer(xctd, 0) == x::kNotCompressed);
  CHECK(x::Answer(nullptr, 4) == x::kNotCompressed);

  // 1.0.5_candidate_9: the trace of compressed files (Tracer), bounded per handle and in all.
  {
    x::Tracer t;
    CHECK(!t.Take(0xF8000010));  // not traced
    t.Add(0xF8000010, "\\bundle\\4F45161E");
    std::string path;
    CHECK(t.Traced(0xF8000010) && t.Take(0xF8000010, &path) && path == "\\bundle\\4F45161E");
    int lines = 1;
    while (t.Take(0xF8000010)) ++lines;
    CHECK(lines == x::Tracer::kLinesPerHandle);
    t.Add(0xF8000010, "\\bundle\\4F3D9648");  // the handle reused by a later open: a new budget
    CHECK(t.Take(0xF8000010, &path) && path == "\\bundle\\4F3D9648");
    t.Remove(0xF8000010);
    CHECK(!t.Traced(0xF8000010) && !t.Take(0xF8000010));
    // the total: many handles stop at kLinesInAll
    int all = lines + 1;
    for (uint32_t h = 0; h < 40; ++h) {
      t.Add(0xF9000000 + h, "f");
      while (t.Take(0xF9000000 + h)) ++all;
    }
    CHECK(all == x::Tracer::kLinesInAll);
    // at most kHandles followed: the oldest goes
    x::Tracer u;
    for (uint32_t h = 0; h < x::Tracer::kHandles + 1; ++h) u.Add(h + 1, "f");
    CHECK(!u.Traced(1) && u.Traced(2) && u.Traced(uint32_t(x::Tracer::kHandles + 1)));
    CHECK(x::Hex(xctd, sizeof(xctd)) == "0ff512ed00000001");
    CHECK(x::Hex(xctd, sizeof(xctd), 4) == "0ff512ed" && x::Hex(nullptr, 4).empty() && x::Hex(xctd, 0).empty());
  }

  const std::string io = ReadText(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_io_info.cc");
  if (io.empty()) {
    std::printf("no assembled tree here; the query not checked\n");
  } else {
    const size_t at = io.find("case XFileXctdCompressionInformation: {");
    const size_t end = io.find("case XFileNetworkOpenInformation:", at);
    CHECK(at != std::string::npos && end != std::string::npos);
    const std::string body = io.substr(at, end - at);
    // Read at offset 0 through the vfs file (positional: the XFile's position is not moved).
    CHECK(body.find("ReadSync(std::span<uint8_t>(head, sizeof(head)), 0, &head_read)") != std::string::npos);
    CHECK(body.find("xbox360ps5::xctd::Answer(head, head_read)") != std::string::npos);
    CHECK(body.find("out_length = sizeof(uint32_t);") != std::string::npos);
    // An unreadable file keeps the old answer; the old "unimplemented" for every file is gone.
    CHECK(body.find("status = X_STATUS_INVALID_PARAMETER;") != std::string::npos);
    CHECK(body.find("if (!readable) {") < body.find("status = X_STATUS_INVALID_PARAMETER;"));
    CHECK(io.find("\"unimplemented\");\n      // Files that are XCTD compressed") == std::string::npos);
    CHECK(body.find("XCTD: title {:08X} asks whether its files are compressed") != std::string::npos);
    // the trace: compressed handles followed through queries, sets, reads and LDI calls
    CHECK(body.find("xbox360ps5::xctd::tracer.Add(uint32_t(file_handle), path);") != std::string::npos);
    CHECK(io.find("info_class != XFileXctdCompressionInformation &&\n      xbox360ps5::xctd::tracer.Take(") !=
          std::string::npos);
    CHECK(io.find("XCTD trace: handle {:08X} set class") != std::string::npos);
    const std::string reads = ReadText(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc");
    const size_t read_at = reads.find("dword_result_t NtReadFile_entry(");
    const size_t scatter_at = reads.find("dword_result_t NtReadFileScatter_entry(");
    const size_t traced = reads.find("XCTD trace: handle {:08X} read at");
    CHECK(read_at < traced && traced < scatter_at);
    CHECK(reads.find("#endif\n#include \"xbox360ps5/xctd.hpp\"") != std::string::npos);  // on every platform
    const std::string ldi = ReadText(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_ldi.cc");
    CHECK(ldi.find("LDI trace: create, block") != std::string::npos);
    CHECK(ldi.find("LDI trace: handle {} frame") != std::string::npos);
    std::printf("tree: the XCTD query answers from the file's first word; compressed files are traced\n");
  }
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-xctd-query: all passed\n");
  return 0;
}
