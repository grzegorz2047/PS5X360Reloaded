// SPDX-License-Identifier: MIT
// Host check of fix 44 (issue #8): the 1.0.x Kinect headers the assembled tree takes from the
// official package's KINECT-source-and-licenses.zip (virtual_skeleton.hpp,
// factory_calibration.hpp, virtual_depth_camera.hpp) and the reconstructed usb_kinect.hpp.
// Built and run from the assembled tree's root (tools/source-1.0.2/run-fix-checks.sh):
//   * KTCAL1 calibration blobs, valid and broken, and the missing calibration.bin;
//   * a zero-confidence joint stays NOT_TRACKED with a zeroed position (as the official
//     1.0.2's skeleton writer, va 0x7455c4);
//   * the camera's control command bytes and reply check, the VID/PID match;
//   * the reassembly of isochronous "RB" packets into depth and colour frames;
//   * the relay client against a fake kinect-tracker relay on 127.0.0.1:9301 (frames, partial
//     reads, bad magic, wrong size, any other stream byte as colour, reconnects, stop while
//     connected) and the camera's calibration switching while the relay is active;
//   * the direct USB session against a simulated /dev/ugen camera (control path, FS_INIT,
//     FS_OPEN, FS_START, completions, frames, the periodic report, an unplug, and the
//     failures of each step), with the official log lines;
//   * the tree's provenance: the package pinned in SOURCES.json and its licences.
#define XBOX360PS5_KINECT_USB_HOST_LOG
#include <arpa/inet.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <time.h>
#include <type_traits>
#include <unistd.h>
#include <vector>

// The core's XELOGW/XELOGI, recorded as "w <text>" / "i <text>".
namespace host_log {
inline std::mutex mutex;
inline std::vector<std::string> lines;
template <typename T>
std::string Text(const T& value) {
  std::ostringstream out;
  if constexpr (std::is_same_v<T, uint8_t> || std::is_same_v<T, int8_t>)
    out << int(value);
  else
    out << value;
  return out.str();
}
template <typename... Args>
void Add(char level, std::string_view format, const Args&... args) {
  const std::vector<std::string> values{Text(args)...};
  std::string line(1, level);
  line += ' ';
  size_t next = 0;
  for (size_t n = 0; n < format.size(); ++n) {
    if (format[n] == '{' && n + 1 < format.size() && format[n + 1] == '}') {
      line += next < values.size() ? values[next++] : std::string("{?}");
      ++n;
    } else {
      line += format[n];
    }
  }
  std::printf("  log: %s\n", line.c_str());
  std::lock_guard<std::mutex> lock(mutex);
  lines.push_back(line);
}
size_t Count(const std::string& line) {
  std::lock_guard<std::mutex> lock(mutex);
  size_t count = 0;
  for (const auto& seen : lines) count += seen == line;
  return count;
}
size_t CountPrefix(const std::string& prefix) {
  std::lock_guard<std::mutex> lock(mutex);
  size_t count = 0;
  for (const auto& seen : lines) count += seen.compare(0, prefix.size(), prefix) == 0;
  return count;
}
void Clear() {
  std::lock_guard<std::mutex> lock(mutex);
  lines.clear();
}
}  // namespace host_log
template <typename... Args>
void XELOGW(std::string_view format, const Args&... args) {
  host_log::Add('w', format, args...);
}
template <typename... Args>
void XELOGI(std::string_view format, const Args&... args) {
  host_log::Add('i', format, args...);
}

#include "xbox360ps5/virtual_depth_camera.hpp"

using namespace xbox360ps5;
using namespace std::chrono_literals;

static int failures = 0;
#define CHECK(condition)                                                    \
  do {                                                                      \
    if (!(condition)) {                                                     \
      std::printf("FAIL line %d: %s\n", __LINE__, #condition);              \
      ++failures;                                                           \
    }                                                                       \
  } while (0)

template <typename F>
static bool WaitFor(F condition, std::chrono::milliseconds limit) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (!condition()) {
    if (std::chrono::steady_clock::now() > end) return false;
    std::this_thread::sleep_for(5ms);
  }
  return true;
}
static bool WaitLog(const std::string& line, std::chrono::milliseconds limit, size_t times = 1) {
  return WaitFor([&] { return host_log::Count(line) >= times; }, limit);
}

// ---- factory_calibration.hpp --------------------------------------------------------------
static void Put32(uint8_t* at, uint32_t value) {
  for (int n = 0; n < 4; ++n) at[n] = uint8_t(value >> (8 * n));
}
static void PutFloat(uint8_t* at, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  Put32(at, bits);
}
struct Blob {
  uint32_t nr = 118, np = 8, nf = 322, ns = 4;
  std::vector<uint8_t> reg = std::vector<uint8_t>(118), pad = std::vector<uint8_t>(8),
                       fixed = std::vector<uint8_t>(334), shift = std::vector<uint8_t>(4);
  Blob() {
    // The values of the package's native/kinect/tests/test_factory.c.
    for (size_t n = 2; n < reg.size(); ++n) reg[n] = uint8_t(n * 3 + 1);
    PutFloat(&fixed[94], 7.5f);
    PutFloat(&fixed[98], 2.3f);
    PutFloat(&fixed[102], 120);
    PutFloat(&fixed[106], .105f);
    shift[2] = 200;
  }
  std::vector<uint8_t> Bytes() const {
    std::vector<uint8_t> out(24);
    std::memcpy(out.data(), "KTCAL1\0\0", 8);
    Put32(&out[8], nr);
    Put32(&out[12], np);
    Put32(&out[16], nf);
    Put32(&out[20], ns);
    out.insert(out.end(), reg.begin(), reg.begin() + std::min<size_t>(nr, reg.size()));
    out.insert(out.end(), pad.begin(), pad.begin() + std::min<size_t>(np, pad.size()));
    out.insert(out.end(), fixed.begin(), fixed.begin() + std::min<size_t>(nf, fixed.size()));
    out.insert(out.end(), shift.begin(), shift.begin() + std::min<size_t>(ns, shift.size()));
    return out;
  }
};
static bool Parse(const std::vector<uint8_t>& bytes, motion::VirtualDepthCalibration& out) {
  return motion::factory_calibration_detail::Parse(bytes.data(), bytes.size(), out);
}
static bool Parse(const Blob& blob) {
  motion::VirtualDepthCalibration out;
  const bool ok = Parse(blob.Bytes(), out);
  CHECK(out.valid == ok && out.physical);
  return ok;
}

static void CheckCalibration() {
  std::puts("calibration (KTCAL1)");
  motion::VirtualDepthCalibration builtin;
  CHECK(builtin.valid && !builtin.physical && builtin.const_shift == 200);
  Blob good;
  motion::VirtualDepthCalibration out;
  CHECK(Parse(good.Bytes(), out));
  CHECK(out.valid && out.physical);
  CHECK(out.dcmos_emitter_distance == 7.5f && out.dcmos_rcmos_distance == 2.3f);
  CHECK(out.reference_distance == 120 && out.reference_pixel_size == .105f);
  CHECK(out.const_shift == 200);
  CHECK(std::equal(out.registration.begin(), out.registration.end(), good.reg.begin() + 2));
  Blob long_fixed;
  long_fixed.nf = 334;
  CHECK(Parse(long_fixed));
  // Each broken field on its own.
  auto broken = [&](auto change) {
    Blob blob;
    change(blob);
    return !Parse(blob);
  };
  CHECK(broken([](Blob& b) { b.nr = 117; }));
  CHECK(broken([](Blob& b) { b.np = 7; }));
  CHECK(broken([](Blob& b) { b.nf = 323; }));
  CHECK(broken([](Blob& b) { b.ns = 5; }));
  CHECK(broken([](Blob& b) { b.reg[0] = 1; }));             // a device error, not data
  CHECK(broken([](Blob& b) { b.pad[1] = 1; }));
  CHECK(broken([](Blob& b) { b.fixed[0] = 1; }));
  CHECK(broken([](Blob& b) { b.shift[1] = 1; }));
  CHECK(broken([](Blob& b) { b.pad[2] = 65; }));            // crop beyond 64
  CHECK(!broken([](Blob& b) { b.pad[2] = 64; }));
  CHECK(broken([](Blob& b) { std::fill(b.reg.begin() + 2, b.reg.end(), 0); }));
  CHECK(broken([](Blob& b) { PutFloat(&b.fixed[106], NAN); }));
  CHECK(broken([](Blob& b) { PutFloat(&b.fixed[94], INFINITY); }));
  CHECK(broken([](Blob& b) { PutFloat(&b.fixed[94], 1.9f); }));
  CHECK(broken([](Blob& b) { PutFloat(&b.fixed[94], 20.5f); }));
  CHECK(broken([](Blob& b) { PutFloat(&b.fixed[98], .5f); }));
  CHECK(broken([](Blob& b) { PutFloat(&b.fixed[98], 10.5f); }));
  CHECK(broken([](Blob& b) { PutFloat(&b.fixed[102], 40); }));
  CHECK(broken([](Blob& b) { PutFloat(&b.fixed[102], 600); }));
  CHECK(broken([](Blob& b) { PutFloat(&b.fixed[106], 2); }));
  CHECK(broken([](Blob& b) { PutFloat(&b.fixed[106], .25f); }));  // focal 240
  CHECK(broken([](Blob& b) { PutFloat(&b.fixed[102], 500); PutFloat(&b.fixed[106], .2f); }));  // 1250
  CHECK(broken([](Blob& b) { b.shift[2] = 0; }));
  CHECK(broken([](Blob& b) { b.shift[2] = 0x01; b.shift[3] = 0x10; }));  // 4097
  CHECK(!broken([](Blob& b) { b.shift[2] = 0x00; b.shift[3] = 0x10; }));  // 4096
  auto bytes = good.Bytes();
  bytes[0] = 'X';
  CHECK(!Parse(bytes, out) && !out.valid && out.physical);
  bytes = good.Bytes();
  bytes.push_back(0);
  CHECK(!Parse(bytes, out));
  bytes = good.Bytes();
  bytes.pop_back();
  CHECK(!Parse(bytes, out));
  CHECK(!motion::factory_calibration_detail::Parse(nullptr, 0, out));
  CHECK(!motion::factory_calibration_detail::Parse(good.Bytes().data(), 23, out));
  // The console's file is absent on a host: physical, not valid.
  if (access("/data/kinect-tracker/calibration.bin", F_OK)) {
    const auto missing = motion::ReadPhysicalCalibration();
    CHECK(!missing.valid && missing.physical);
  }
}

// ---- virtual_skeleton.hpp -----------------------------------------------------------------
static uint32_t BigU32(const uint8_t* at) {
  return uint32_t(at[0]) << 24 | uint32_t(at[1]) << 16 | uint32_t(at[2]) << 8 | at[3];
}
static float BigFloat(const uint8_t* at) {
  const uint32_t bits = BigU32(at);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}
static void CheckSkeleton() {
  std::puts("skeleton: zero-confidence joints");
  motion::Frame pose;
  pose.tracked = true;
  for (size_t n = 0; n < pose.joints.size(); ++n)
    pose.joints[n] = {.1f * float(n), .05f * float(n), .2f, 1};
  pose.joints[7].confidence = 0;    // absent hand
  pose.joints[19].confidence = 0;   // absent foot
  pose.joints[9].confidence = .3f;  // seen badly
  std::vector<uint8_t> frame(motion::kSkeletonFrameBytes, 0xEE);
  motion::WriteSkeletonFrame(frame.data(), pose, 3, 1000, 5);
  const uint8_t* body = &frame[motion::kSkeletonDataOffset];
  CHECK(BigU32(body) == motion::kSkeletonTracked && BigU32(body + 4) == 5);
  for (uint32_t n = 0; n < motion::kJointCount; ++n) {
    const uint8_t* position = body + 0x20 + n * 16;
    const uint32_t state = BigU32(body + 0x160 + n * 4);
    if (n == 7 || n == 19) {
      for (int k = 0; k < 16; ++k) CHECK(position[k] == 0);  // XYZW zero
      CHECK(state == 0);                                      // NUI NOT_TRACKED
    } else {
      CHECK(BigFloat(position + 12) == 1.0f);
      CHECK(state == (n == 9 ? motion::kJointInferred : motion::kJointTracked));
    }
  }
}

// ---- usb_kinect.hpp: pure parts ------------------------------------------------------------
static std::vector<std::vector<uint8_t>> Packets(uint8_t flag, const std::vector<uint8_t>& frame,
                                                 size_t body = kinect_usb::kPacketBytes - 12) {
  std::vector<std::vector<uint8_t>> out;
  uint8_t sequence = 0x40;
  for (size_t at = 0; at < frame.size(); at += body) {
    const size_t size = std::min(body, frame.size() - at);
    const uint8_t kind = at == 0 ? 1 : at + size == frame.size() ? 5 : 2;
    std::vector<uint8_t> packet = {'R', 'B', 0, uint8_t(flag | kind), 0, sequence++, 0, 0, 1, 2, 3, 4};
    packet.insert(packet.end(), frame.begin() + at, frame.begin() + at + size);
    out.push_back(std::move(packet));
  }
  return out;
}
static std::vector<uint8_t> Pattern(size_t size, uint8_t seed) {
  std::vector<uint8_t> out(size);
  for (size_t n = 0; n < size; ++n) out[n] = uint8_t(n * 7 + seed + (n >> 11));
  return out;
}

static void CheckPure() {
  std::puts("usb_kinect: control command, VID/PID, packet reassembly");
  uint8_t command[12];
  kinect_usb::WriteRegisterCommand(0x1234, 0x105, 0x00AB, command);
  const uint8_t expected[12] = {0x47, 0x4D, 2, 0, 3, 0, 0x34, 0x12, 0x05, 0x01, 0xAB, 0};
  CHECK(!std::memcmp(command, expected, 12));
  const uint8_t good[8] = {'R', 'B', 2, 0, 3, 0, 0, 0}, bad[8] = {'R', 'C', 2, 0, 3, 0, 0, 0};
  CHECK(kinect_usb::ControlReplyValid(good, 8) && !kinect_usb::ControlReplyValid(good, 7));
  CHECK(!kinect_usb::ControlReplyValid(bad, 8));
  CHECK(kinect_usb::IsKinectCamera(0x045E, 0x02AE) && kinect_usb::IsKinectCamera(0x045E, 0x02BF));
  CHECK(!kinect_usb::IsKinectCamera(0x045E, 0x02B0));  // the motor
  CHECK(!kinect_usb::IsKinectCamera(0x045E, 0x02C2));  // Kinect for Windows
  CHECK(!kinect_usb::IsKinectCamera(0x045D, 0x02AE));
  CHECK(kinect_usb::kDepthFrameBytes == 0x67200 && kinect_usb::kColorFrameBytes == 0x4B000);

  const auto depth_frame = Pattern(kinect_usb::kDepthFrameBytes, 1);
  const auto depth_packets = Packets(0x70, depth_frame);
  CHECK(depth_packets.size() == 222);
  kinect_usb::StreamAssembly depth(0x70, kinect_usb::kDepthFrameBytes);
  size_t ready = 0;
  for (const auto& packet : depth_packets) ready += depth.Packet(packet.data(), uint32_t(packet.size()));
  CHECK(ready == 1 && depth.complete == 1 && depth.incomplete == 0 && depth.frame == depth_frame);
  // Colour packets, broken headers and packets before a start do not touch a depth frame.
  const auto colour_packets = Packets(0x80, Pattern(kinect_usb::kColorFrameBytes, 9));
  for (const auto& packet : colour_packets) CHECK(!depth.Packet(packet.data(), uint32_t(packet.size())));
  auto middle = depth_packets[3];
  CHECK(!depth.Packet(middle.data(), uint32_t(middle.size())) && !depth.in_frame);
  auto end = depth_packets.back();
  CHECK(!depth.Packet(end.data(), uint32_t(end.size())) && depth.incomplete == 0);
  auto start = depth_packets.front();
  CHECK(!depth.Packet(start.data(), 11) && !depth.in_frame);
  auto wrong = start;
  wrong[1] = 'C';
  CHECK(!depth.Packet(wrong.data(), uint32_t(wrong.size())) && !depth.in_frame);
  std::vector<uint8_t> oversize(kinect_usb::kPacketBytes + 4, 0);
  std::memcpy(oversize.data(), start.data(), 12);
  CHECK(!depth.Packet(oversize.data(), uint32_t(oversize.size())) && !depth.in_frame);
  // A frame that ends early counts as incomplete; the next start begins anew.
  CHECK(!depth.Packet(start.data(), uint32_t(start.size())) && depth.in_frame);
  CHECK(!depth.Packet(depth_packets[1].data(), uint32_t(depth_packets[1].size())));
  CHECK(!depth.Packet(end.data(), uint32_t(end.size())));
  CHECK(depth.incomplete == 1 && !depth.in_frame && depth.complete == 1);
  // More data than a frame: the surplus is dropped and the end still completes it (as the
  // official session does); a sequence number gap is not checked (nor in the official).
  ready = 0;
  for (size_t n = 0; n + 1 < depth_packets.size(); ++n)
    ready += depth.Packet(depth_packets[n].data(), uint32_t(depth_packets[n].size()));
  CHECK(depth.fill == depth_frame.size() - (end.size() - 12));
  CHECK(!depth.Packet(depth_packets[5].data(), uint32_t(depth_packets[5].size())));
  ready += depth.Packet(end.data(), uint32_t(end.size()));
  CHECK(ready == 1 && depth.complete == 2 && depth.frame == depth_frame);
  // A frame restarted in the middle of another.
  kinect_usb::StreamAssembly colour(0x80, kinect_usb::kColorFrameBytes);
  for (size_t n = 0; n < 10; ++n) colour.Packet(colour_packets[n].data(), uint32_t(colour_packets[n].size()));
  ready = 0;
  for (const auto& packet : colour_packets) ready += colour.Packet(packet.data(), uint32_t(packet.size()));
  CHECK(ready == 1 && colour.complete == 1 && colour.incomplete == 0);

  kinect_usb::Slot slot;
  std::vector<uint8_t> out;
  CHECK(slot.Copy(0, out) == 0 && out.empty());
  const uint8_t bytes[3] = {1, 2, 3};
  slot.Publish(bytes, 3);
  CHECK(slot.Copy(0, out) == 1 && out == std::vector<uint8_t>({1, 2, 3}));
  CHECK(slot.Copy(1, out) == 0);
  slot.sequence = 0xFFFFFFFEu;
  slot.Publish(bytes, 2);
  CHECK(slot.sequence == 1 && slot.Copy(0xFFFFFFFEu, out) == 1 && out.size() == 2);
}

// ---- the relay -------------------------------------------------------------------------------
struct FakeRelay {
  int server = -1, client = -1;
  bool Listen() {
    server = socket(AF_INET, SOCK_STREAM, 0);
    const int one = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kinect_usb::kRelayPort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(server, 1)) {
      std::printf("cannot listen on 127.0.0.1:%u (errno %d)\n", kinect_usb::kRelayPort, errno);
      close(server);
      server = -1;
      return false;
    }
    return true;
  }
  bool Accept(std::chrono::milliseconds limit) {
    pollfd waiting{server, POLLIN, 0};
    if (poll(&waiting, 1, int(limit.count())) <= 0) return false;
    client = accept(server, nullptr, nullptr);
    return client >= 0;
  }
  bool Send(const void* data, size_t size) {
    auto* at = static_cast<const uint8_t*>(data);
    while (size) {
      const ssize_t sent = send(client, at, size, MSG_NOSIGNAL);
      if (sent <= 0) return false;
      at += sent;
      size -= size_t(sent);
    }
    return true;
  }
  static std::vector<uint8_t> Header(uint8_t stream, uint32_t size, const char* magic = "KNCT") {
    std::vector<uint8_t> header = {uint8_t(magic[0]), uint8_t(magic[1]), uint8_t(magic[2]),
                                   uint8_t(magic[3]), stream, 0, 0, 0};
    for (int n = 0; n < 4; ++n) header.push_back(uint8_t(size >> (8 * n)));
    return header;
  }
  bool Frame(uint8_t stream, const std::vector<uint8_t>& payload) {
    const auto header = Header(stream, uint32_t(payload.size()));
    return Send(header.data(), header.size()) && Send(payload.data(), payload.size());
  }
  // The emulator closed its side: reading gives the end of the stream.
  bool PeerClosed(std::chrono::milliseconds limit) {
    pollfd waiting{client, POLLIN, 0};
    if (poll(&waiting, 1, int(limit.count())) <= 0) return false;
    uint8_t byte;
    return recv(client, &byte, 1, 0) == 0;
  }
  void CloseClient() {
    if (client >= 0) close(client);
    client = -1;
  }
  void Close() {
    CloseClient();
    if (server >= 0) close(server);
    server = -1;
  }
  ~FakeRelay() { Close(); }
};

// No camera on USB: the relay is the only source.
struct NoUsb {
  static std::string FindCamera() { return ""; }
  static int Open(const std::string&) { return -1; }
  static void Close(int) {}
  static int Ioctl(int, unsigned long, void*) { errno = ENODEV; return -1; }
  static void Wait(int, int) {}
};

static const std::string kNoSource =
    "i Kinect USB: no camera on USB and no relay; using pose-derived frames";
static const std::string kRelayOpen =
    "w Kinect USB: receiving camera frames from the relay on 127.0.0.1:9301";
static std::string RelayClosed(int frames) {
  return "w Kinect USB: relay closed after " + std::to_string(frames) + " frames";
}

static void CheckRelay() {
  std::puts("usb_kinect: relay client");
  host_log::Clear();
  FakeRelay relay;
  {
    kinect_usb::BasicSensor<NoUsb> sensor;
    CHECK(WaitLog(kNoSource, 1500ms));
    CHECK(!sensor.active());
    std::vector<uint8_t> out;
    CHECK(sensor.CopyDepth(0, out) == 0 && sensor.CopyColor(0, out) == 0);
    // The relay appears: the sensor finds it on its next attempt (every 2 s).
    if (!relay.Listen()) {
      ++failures;
      return;
    }
    CHECK(relay.Accept(3000ms));
    CHECK(WaitLog(kRelayOpen, 1000ms));
    CHECK(WaitFor([&] { return sensor.active(); }, 1000ms));
    const auto depth = Pattern(kinect_usb::kDepthFrameBytes, 3);
    const auto colour = Pattern(kinect_usb::kColorFrameBytes, 5);
    CHECK(relay.Frame(1, depth));
    CHECK(WaitFor([&] { return sensor.CopyDepth(0, out) == 1; }, 1000ms) && out == depth);
    CHECK(sensor.CopyDepth(1, out) == 0);
    CHECK(sensor.CopyColor(0, out) == 0);
    CHECK(relay.Frame(2, colour));
    CHECK(WaitFor([&] { return sensor.CopyColor(0, out) == 1; }, 1000ms) && out == colour);
    // A frame in pieces, with pauses longer than nothing but shorter than the 1 s timeout.
    const auto depth2 = Pattern(kinect_usb::kDepthFrameBytes, 11);
    const auto header = FakeRelay::Header(1, kinect_usb::kDepthFrameBytes);
    CHECK(relay.Send(header.data(), 5));
    std::this_thread::sleep_for(100ms);
    CHECK(relay.Send(header.data() + 5, 7));
    CHECK(relay.Send(depth2.data(), 1000));
    std::this_thread::sleep_for(300ms);
    CHECK(relay.Send(depth2.data() + 1000, depth2.size() - 1000));
    CHECK(WaitFor([&] { return sensor.CopyDepth(1, out) == 2; }, 1000ms) && out == depth2);
    // Any stream byte other than 1 is colour, with colour's size.
    CHECK(relay.Frame(7, colour));
    CHECK(WaitFor([&] { return sensor.CopyColor(1, out) == 2; }, 1000ms));
    // No data for longer than the receive timeout keeps the connection.
    std::this_thread::sleep_for(1300ms);
    CHECK(sensor.active() && host_log::Count(RelayClosed(4)) == 0);
    // Bad magic: the sensor closes the connection and connects again at once.
    const auto bad = FakeRelay::Header(1, kinect_usb::kDepthFrameBytes, "KNCX");
    CHECK(relay.Send(bad.data(), bad.size()));
    CHECK(relay.PeerClosed(2000ms));
    CHECK(WaitLog(RelayClosed(4), 1000ms));
    relay.CloseClient();
    CHECK(relay.Accept(1000ms));
    CHECK(WaitLog(kRelayOpen, 1000ms, 2));
    // A size that is not the stream's: closed again.
    const auto wrong = FakeRelay::Header(1, kinect_usb::kColorFrameBytes);
    CHECK(relay.Send(wrong.data(), wrong.size()));
    CHECK(relay.PeerClosed(2000ms));
    CHECK(WaitLog(RelayClosed(0), 1000ms));
    relay.CloseClient();
    // The relay goes away mid-frame: closed, no partial frame published.
    CHECK(relay.Accept(1000ms));
    CHECK(WaitLog(kRelayOpen, 1000ms, 3));
    CHECK(relay.Send(header.data(), header.size()) && relay.Send(depth.data(), 4096));
    relay.Close();
    CHECK(WaitLog(RelayClosed(0), 2000ms, 2));
    CHECK(WaitFor([&] { return !sensor.active(); }, 1000ms));
    CHECK(sensor.CopyDepth(2, out) == 0);
    // The frames stay available for the camera after the relay has gone.
    CHECK(sensor.CopyDepth(0, out) == 2 && out == depth2);
    // No second "no camera" line: it is said once per camera.
    std::this_thread::sleep_for(500ms);
    CHECK(host_log::Count(kNoSource) == 1);
  }
  // A stop while the relay is connected and silent ends within the receive timeout.
  {
    FakeRelay silent;
    CHECK(silent.Listen());
    std::chrono::steady_clock::time_point stop;
    {
      kinect_usb::BasicSensor<NoUsb> sensor;
      CHECK(silent.Accept(2000ms));
      CHECK(WaitFor([&] { return sensor.active(); }, 1000ms));
      // Waiting on a silent relay sleeps in recv (1 s timeouts), it does not spin.
      timespec cpu0{}, cpu1{};
      clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu0);
      std::this_thread::sleep_for(1500ms);
      clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu1);
      const double cpu = double(cpu1.tv_sec - cpu0.tv_sec) + double(cpu1.tv_nsec - cpu0.tv_nsec) / 1e9;
      std::printf("  1.5 s on a silent relay used %.3f s of CPU\n", cpu);
      CHECK(cpu < 0.3);
      CHECK(sensor.active() && host_log::Count(RelayClosed(0)) == 2);
      stop = std::chrono::steady_clock::now();
    }
    const auto took = std::chrono::steady_clock::now() - stop;
    CHECK(took < 1500ms);
    CHECK(host_log::Count(RelayClosed(0)) == 3);
    std::printf("  stop while connected took %lld us\n",
                (long long)std::chrono::duration_cast<std::chrono::microseconds>(took).count());
  }
}

// ---- the camera's calibration and frames through VirtualDepthCamera --------------------------
static void CheckDepthCamera() {
  std::puts("virtual_depth_camera: relay frames and calibration");
  host_log::Clear();
  CHECK(!motion::VirtualDepthCamera::Open(false, {}));
  motion::SetEnabled(true);
  auto camera = motion::VirtualDepthCamera::Open(false, {});
  CHECK(camera != nullptr);
  if (!camera) return;
  // No source: the built-in calibration and pose-derived frames.
  auto calibration = camera->calibration();
  CHECK(calibration.valid && !calibration.physical);
  std::vector<uint8_t> out;
  CHECK(camera->CopyDepthFrame(0, out) != 0 && out.size() == kinect_usb::kDepthFrameBytes);
#if __has_include("xbox360ps5/camera_figure.hpp")
  motion::camera_figure_wanted = false;  // fix 185: the grey of before, without the figure
#endif
  CHECK(camera->CopyColorFrame(0, out) != 0 && out == std::vector<uint8_t>(kinect_usb::kColorFrameBytes, 128));
#if __has_include("xbox360ps5/camera_figure.hpp")
  motion::camera_figure_wanted = true;
#endif
  FakeRelay relay;
  CHECK(relay.Listen());
  CHECK(relay.Accept(3000ms));
  CHECK(WaitFor([&] { return camera->calibration().physical; }, 1000ms));
  // The physical camera's calibration comes from calibration.bin; on a host there is none.
  if (access("/data/kinect-tracker/calibration.bin", F_OK)) CHECK(!camera->calibration().valid);
  const auto depth = Pattern(kinect_usb::kDepthFrameBytes, 21);
  CHECK(relay.Frame(1, depth));
  CHECK(WaitFor([&] { return camera->CopyDepthFrame(0, out) == 1; }, 1000ms) && out == depth);
  CHECK(camera->CopyDepthFrame(1, out) == 0);
  CHECK(camera->CopyColorFrame(0, out) == 0);  // nothing from the relay yet
  relay.Close();
  CHECK(WaitFor([&] { return !camera->calibration().physical; }, 2000ms));
  CHECK(camera->calibration().valid);
  camera.reset();
  motion::SetEnabled(false);
}

// ---- the direct USB session against a simulated ugen camera ----------------------------------
struct FakeUsb {
  static inline std::mutex mutex;
  static inline std::string path = "/dev/ugen0.4";
  static inline bool present = true;
  static inline int open_errno = 0, control_errno = 0, init_errno = 0, open_fail_index = -1,
                    complete_errno = 0;
  static inline int read_tries = 0, empty_reads = 0;
  static inline std::vector<std::pair<uint16_t, uint16_t>> registers;
  static inline std::vector<uint16_t> tags;
  static inline std::vector<std::string> calls;
  static inline std::vector<kinect_usb::ugen::FsOpen> opens;
  static inline kinect_usb::ugen::FsEndpoint* endpoints = nullptr;
  static inline bool pending[4] = {};
  static inline bool starts_ok = true;
  static inline int closed_fd = -1;
  static inline std::deque<std::pair<uint8_t, std::vector<std::vector<uint8_t>>>> completions;

  static void Reset() {
    std::lock_guard<std::mutex> lock(mutex);
    present = true;
    open_errno = control_errno = init_errno = complete_errno = 0;
    open_fail_index = -1;
    read_tries = empty_reads = 0;
    registers.clear();
    tags.clear();
    calls.clear();
    opens.clear();
    endpoints = nullptr;
    std::fill(std::begin(pending), std::end(pending), false);
    starts_ok = true;
    closed_fd = -1;
    completions.clear();
  }
  static std::string FindCamera() {
    std::lock_guard<std::mutex> lock(mutex);
    return present ? path : "";
  }
  static int Open(const std::string& at) {
    std::lock_guard<std::mutex> lock(mutex);
    calls.push_back("open " + at);
    if (open_errno) {
      errno = open_errno;
      return -1;
    }
    return 77;
  }
  static void Close(int fd) {
    std::lock_guard<std::mutex> lock(mutex);
    calls.push_back("close");
    closed_fd = fd;
  }
  static void Wait(int fd, int milliseconds) {
    if (fd != 77 || milliseconds != 50) starts_ok = false;
    std::this_thread::sleep_for(1ms);
  }
  static int Fail(int error) {
    errno = error;
    return -1;
  }
  static int Ioctl(int fd, unsigned long request, void* argument) {
    namespace ugen = kinect_usb::ugen;
    std::lock_guard<std::mutex> lock(mutex);
    if (fd != 77) return Fail(EBADF);
    if (request == ugen::kDoRequest) {
      auto* control = static_cast<ugen::CtlRequest*>(argument);
      if (control->ucr_flags != ugen::kShortTransferOk) return Fail(EINVAL);
      if (control->ucr_request.bmRequestType == 0x40) {
        if (control_errno) return Fail(control_errno);
        const auto* bytes = static_cast<const uint8_t*>(control->ucr_data);
        if (control->ucr_request.wLength[0] != 12 || bytes[0] != 'G' || bytes[1] != 'M')
          return Fail(EINVAL);
        tags.push_back(uint16_t(bytes[6] | bytes[7] << 8));
        registers.push_back({uint16_t(bytes[8] | bytes[9] << 8), uint16_t(bytes[10] | bytes[11] << 8)});
        return 0;
      }
      if (control->ucr_request.bmRequestType != 0xC0 || control->ucr_request.wLength[1] != 2)
        return Fail(EINVAL);
      ++read_tries;
      // The first answer of a command is often empty: the command is asked again.
      if (empty_reads > 0) {
        --empty_reads;
        control->ucr_actlen = 0;
        return 0;
      }
      auto* reply = static_cast<uint8_t*>(control->ucr_data);
      const uint8_t answer[12] = {'R', 'B', 2, 0, 3, 0, 0, 0, 0, 0, 0, 0};
      std::memcpy(reply, answer, 12);
      control->ucr_actlen = 12;
      return 0;
    }
    if (request == ugen::kFsInit) {
      calls.push_back("FS_INIT");
      if (init_errno) return Fail(init_errno);
      auto* init = static_cast<ugen::FsInit*>(argument);
      if (init->ep_index_max != 4) return Fail(EINVAL);
      endpoints = init->pEndpoints;
      return 0;
    }
    if (request == ugen::kFsOpen) {
      auto* open = static_cast<ugen::FsOpen*>(argument);
      calls.push_back("FS_OPEN " + std::to_string(open->ep_index));
      opens.push_back(*open);
      if (open->ep_index == open_fail_index) return Fail(EINVAL);
      return 0;
    }
    const uint8_t index = *static_cast<uint8_t*>(argument);
    if (request == ugen::kFsStart) {
      if (index >= 4 || !endpoints) return Fail(EINVAL);
      const auto& endpoint = endpoints[index];
      bool ok = endpoint.nFrames == 32 && endpoint.flags == 3 && endpoint.timeout == 1000 &&
                endpoint.ppBuffer && endpoint.pLength;
      for (uint32_t n = 0; ok && n < 32; ++n) ok = endpoint.pLength[n] == 1920;
      if (!ok) starts_ok = false;
      pending[index] = true;
      return 0;
    }
    if (request == ugen::kFsComplete) {
      if (complete_errno) return Fail(complete_errno);
      if (completions.empty() || !pending[completions.front().first]) return Fail(EBUSY);
      auto [at, packets] = std::move(completions.front());
      completions.pop_front();
      auto& endpoint = endpoints[at];
      for (size_t n = 0; n < packets.size(); ++n) {
        std::memcpy(endpoint.ppBuffer[n], packets[n].data(), packets[n].size());
        endpoint.pLength[n] = uint32_t(packets[n].size());
      }
      endpoint.aFrames = uint32_t(packets.size());
      endpoint.status = 0;
      pending[at] = false;
      *static_cast<uint8_t*>(argument) = at;
      return 0;
    }
    if (request == ugen::kFsStop) calls.push_back("FS_STOP " + std::to_string(index));
    else if (request == ugen::kFsClose) calls.push_back("FS_CLOSE " + std::to_string(index));
    else if (request == ugen::kFsUninit) calls.push_back("FS_UNINIT");
    else return Fail(EINVAL);
    return 0;
  }
  // A frame's packets as transfers of 32, alternating between two endpoints.
  static void Queue(uint8_t first, uint8_t second, const std::vector<std::vector<uint8_t>>& packets) {
    std::lock_guard<std::mutex> lock(mutex);
    for (size_t at = 0, n = 0; at < packets.size(); at += 32, ++n) {
      std::vector<std::vector<uint8_t>> transfer(packets.begin() + at,
                                                 packets.begin() + std::min(packets.size(), at + 32));
      completions.emplace_back(n % 2 ? second : first, std::move(transfer));
    }
  }
  static bool Drained() {
    std::lock_guard<std::mutex> lock(mutex);
    return completions.empty();
  }
  static size_t Calls(const std::string& call) {
    std::lock_guard<std::mutex> lock(mutex);
    return size_t(std::count(calls.begin(), calls.end(), call));
  }
};

static const std::string kOpened = "w Kinect USB: camera /dev/ugen0.4 opened directly";
static const std::string kStarted = "w Kinect USB: depth and colour streams started";
static const std::string kReleased = "w Kinect USB: streams stopped, camera released";

static void CheckUsbSession() {
  std::puts("usb_kinect: direct USB session (simulated camera)");
  host_log::Clear();
  FakeUsb::Reset();
  FakeUsb::empty_reads = 3;
  const auto depth = Pattern(kinect_usb::kDepthFrameBytes, 31);
  const auto colour = Pattern(kinect_usb::kColorFrameBytes, 37);
  {
    kinect_usb::BasicSensor<FakeUsb> sensor;
    CHECK(WaitLog(kOpened, 1500ms));
    CHECK(WaitLog(kStarted, 1500ms));
    CHECK(sensor.active());
    {
      std::lock_guard<std::mutex> lock(FakeUsb::mutex);
      const std::vector<std::pair<uint16_t, uint16_t>> expected = {
          {0x105, 0}, {0x06, 0}, {0x05, 0}, {0x12, 3}, {0x13, 1}, {0x14, 30}, {0x06, 2},
          {0x17, 0}, {0x0C, 0}, {0x0D, 1}, {0x0E, 30}, {0x05, 1}, {0x47, 0}};
      CHECK(FakeUsb::registers == expected);
      for (size_t n = 0; n < FakeUsb::tags.size(); ++n) CHECK(FakeUsb::tags[n] == n);
      CHECK(FakeUsb::read_tries == int(expected.size()) + 3);
      CHECK(FakeUsb::opens.size() == 4);
      for (size_t n = 0; n < FakeUsb::opens.size(); ++n) {
        const auto& open = FakeUsb::opens[n];
        CHECK(open.ep_index == n && open.ep_no == (n < 2 ? 0x82 : 0x81));
        CHECK(open.max_bufsize == 61440 && open.max_frames == 32);
      }
      CHECK(FakeUsb::pending[0] && FakeUsb::pending[1] && FakeUsb::pending[2] && FakeUsb::pending[3]);
    }
    std::vector<uint8_t> out;
    CHECK(sensor.CopyDepth(0, out) == 0);
    FakeUsb::Queue(0, 1, Packets(0x70, depth));
    CHECK(WaitFor([&] { return sensor.CopyDepth(0, out) == 1; }, 2000ms) && out == depth);
    FakeUsb::Queue(2, 3, Packets(0x80, colour));
    CHECK(WaitFor([&] { return sensor.CopyColor(0, out) == 1; }, 2000ms) && out == colour);
    // A depth frame cut short, and colour packets arriving on a depth endpoint: no new frame.
    auto cut = Packets(0x70, depth);
    cut.erase(cut.begin() + 10, cut.end() - 1);
    FakeUsb::Queue(0, 1, cut);
    FakeUsb::Queue(1, 1, Packets(0x80, colour));
    CHECK(WaitFor([] { return FakeUsb::Drained(); }, 2000ms));
    std::this_thread::sleep_for(50ms);
    CHECK(sensor.CopyDepth(1, out) == 0 && sensor.CopyColor(1, out) == 0);
    // The report, 10 s after the start (then every minute).
    CHECK(WaitLog("i Kinect USB: frames depth 1 colour 1 (incomplete 1/0)", 12000ms));
    // Unplugged: the next completion fails with another error than EBUSY.
    {
      std::lock_guard<std::mutex> lock(FakeUsb::mutex);
      FakeUsb::present = false;
      FakeUsb::complete_errno = ENXIO;
    }
    CHECK(WaitLog("w Kinect USB: stream stopped (errno " + std::to_string(ENXIO) +
                      "), sensor unplugged?",
                  1000ms));
    CHECK(WaitLog(kReleased, 1000ms));
    CHECK(WaitFor([&] { return !sensor.active(); }, 1000ms));
    CHECK(WaitFor([] { return FakeUsb::Calls("close") == 1; }, 1000ms));
    {
      std::lock_guard<std::mutex> lock(FakeUsb::mutex);
      CHECK(FakeUsb::closed_fd == 77);
      CHECK(FakeUsb::starts_ok);
      const auto at = [](const std::string& call) {
        return size_t(std::find(FakeUsb::calls.begin(), FakeUsb::calls.end(), call) - FakeUsb::calls.begin());
      };
      CHECK(at("FS_STOP 0") < at("FS_CLOSE 0") && at("FS_CLOSE 3") < at("FS_UNINIT"));
      CHECK(at("FS_UNINIT") < at("close"));
      const std::vector<std::pair<uint16_t, uint16_t>> last(FakeUsb::registers.end() - 2,
                                                            FakeUsb::registers.end());
      CHECK(last == (std::vector<std::pair<uint16_t, uint16_t>>{{0x06, 0}, {0x05, 0}}));
    }
    for (int n = 0; n < 4; ++n) {
      CHECK(FakeUsb::Calls("FS_STOP " + std::to_string(n)) == 1);
      CHECK(FakeUsb::Calls("FS_CLOSE " + std::to_string(n)) == 1);
    }
    // The frames already taken stay; no "no camera" line after a camera was there.
    CHECK(sensor.CopyDepth(0, out) == 1 && out == depth);
    std::this_thread::sleep_for(300ms);
    CHECK(host_log::Count(kNoSource) == 0);
  }

  // A stop while streaming: no "stream stopped" line, the camera is released.
  host_log::Clear();
  FakeUsb::Reset();
  {
    kinect_usb::BasicSensor<FakeUsb> sensor;
    CHECK(WaitLog(kStarted, 1500ms));
  }
  CHECK(host_log::CountPrefix("w Kinect USB: stream stopped") == 0);
  CHECK(host_log::Count(kReleased) == 1 && FakeUsb::Calls("FS_UNINIT") == 1);

  // The camera does not answer control commands: nothing else is tried.
  host_log::Clear();
  FakeUsb::Reset();
  FakeUsb::control_errno = EIO;
  {
    kinect_usb::BasicSensor<FakeUsb> sensor;
    CHECK(WaitLog("w Kinect USB: camera does not answer control commands", 1500ms));
    CHECK(WaitFor([] { return FakeUsb::Calls("close") == 1; }, 1000ms));
  }
  CHECK(FakeUsb::Calls("FS_INIT") == 0 && host_log::Count(kReleased) == 0);

  // FS_INIT refused: only the line.
  host_log::Clear();
  FakeUsb::Reset();
  FakeUsb::init_errno = EPERM;
  {
    kinect_usb::BasicSensor<FakeUsb> sensor;
    CHECK(WaitLog("w Kinect USB: FS_INIT errno " + std::to_string(EPERM), 1500ms));
    CHECK(WaitFor([] { return FakeUsb::Calls("close") == 1; }, 1000ms));
  }
  CHECK(FakeUsb::Calls("FS_OPEN 0") == 0 && FakeUsb::Calls("FS_UNINIT") == 0);
  CHECK(host_log::Count(kReleased) == 0);

  // FS_OPEN of the third endpoint refused: uninit, streams off, released, nothing started.
  host_log::Clear();
  FakeUsb::Reset();
  FakeUsb::open_fail_index = 2;
  {
    kinect_usb::BasicSensor<FakeUsb> sensor;
    CHECK(WaitLog("w Kinect USB: FS_OPEN 2 errno " + std::to_string(EINVAL), 1500ms));
    CHECK(WaitLog(kReleased, 1000ms));
    CHECK(!sensor.active());
  }
  CHECK(FakeUsb::Calls("FS_OPEN 3") == 0 && FakeUsb::Calls("FS_UNINIT") == 1);
  CHECK(FakeUsb::Calls("FS_CLOSE 0") == 0 && host_log::Count(kStarted) == 0);
  {
    std::lock_guard<std::mutex> lock(FakeUsb::mutex);
    CHECK(!FakeUsb::pending[0]);
  }

  // The camera's node cannot be opened: said once per errno, the relay is tried.
  host_log::Clear();
  FakeUsb::Reset();
  FakeUsb::open_errno = EACCES;
  {
    kinect_usb::BasicSensor<FakeUsb> sensor;
    const std::string refused = "w Kinect USB: camera /dev/ugen0.4 found but open failed (errno " +
                                std::to_string(EACCES) + "); trying the relay on 127.0.0.1:9301";
    CHECK(WaitLog(refused, 1500ms));
    CHECK(WaitFor([] { return FakeUsb::Calls("open /dev/ugen0.4") >= 2; }, 3000ms));
    CHECK(host_log::Count(refused) == 1);
    {
      std::lock_guard<std::mutex> lock(FakeUsb::mutex);
      FakeUsb::open_errno = EPERM;
    }
    CHECK(WaitLog("w Kinect USB: camera /dev/ugen0.4 found but open failed (errno " +
                      std::to_string(EPERM) + "); trying the relay on 127.0.0.1:9301",
                  3000ms));
    CHECK(host_log::Count(kNoSource) == 0 && !sensor.active());
  }
}

// ---- the assembled tree ----------------------------------------------------------------------
static std::string ReadFile(const char* path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}
static void CheckTree() {
  std::puts("tree: provenance and licences");
  const std::string sources = ReadFile("SOURCES.json");
  CHECK(sources.find("230359c2638f99add3972147e1690e62d8984a37c00181501680bdda7c1096c8") !=
        std::string::npos);
  CHECK(sources.find("8cff3386817fcfbc04d2c2dd7620ab61e5f4e9319592465050e8bd9a8a4d67c9") !=
        std::string::npos);
  CHECK(sources.find("package:PPSA50011/KINECT-source-and-licenses.zip:build/kinect-usb-20261014/"
                     "emu-include/xbox360ps5/virtual_depth_camera.hpp") != std::string::npos);
  const std::string camera = ReadFile("include/xbox360ps5/virtual_depth_camera.hpp");
  CHECK(camera.find("#include \"xbox360ps5/usb_kinect.hpp\"") != std::string::npos);
  CHECK(camera.find("bool valid=true;\n};") == std::string::npos);  // the 0.5.9 edit is gone
  CHECK(ReadFile("licenses/kinect-1.0.2/OPENKINECT-APACHE20").find("Apache License") != std::string::npos);
  CHECK(!ReadFile("licenses/kinect-1.0.2/OPENKINECT-CONTRIB").empty());
  CHECK(!ReadFile("licenses/kinect-1.0.2/LICENSE").empty());
  // The core's camera uses the calibration by value and asks whether it is valid.
  const std::string pscam = ReadFile(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_pscam.cc");
  if (!pscam.empty()) {
    CHECK(pscam.find("#include \"xbox360ps5/virtual_depth_camera.hpp\"") != std::string::npos);
    CHECK(pscam.find("camera_->calibration()") != std::string::npos);
    CHECK(pscam.find("calibration.valid") != std::string::npos);
  }
}

int main() {
  CheckCalibration();
  CheckSkeleton();
  CheckPure();
  CheckRelay();
  CheckDepthCamera();
  CheckUsbSession();
  CheckTree();
  if (failures) {
    std::printf("FAIL: %d check(s)\n", failures);
    return 1;
  }
  std::puts("PASS: KTCAL1 calibration, zero-confidence joints, control command, packet reassembly, "
            "relay client, calibration while active, direct USB session (simulated), provenance");
  return 0;
}
