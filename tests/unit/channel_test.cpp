// The channel between the runtime and a module host (isolation/channel.hpp): frame headers and
// payload fields, every frame kind through a socket pair, payloads that arrive in pieces, headers
// and streams that are refused without allocating what they announce, the end of a stream between
// and within frames, and writes to a peer that is gone, which raise no SIGPIPE.
#include "isolation/channel.hpp"

#include <praenora/runtime/error.hpp>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using praenora::detail::Channel;
using praenora::detail::decode_header;
using praenora::detail::encode_frame;
using praenora::detail::encode_header;
using praenora::detail::Frame;
using praenora::detail::frame_kind_name;
using praenora::detail::FrameKind;
using praenora::detail::kFrameHeaderSize;
using praenora::detail::kFrameMagic;
using praenora::detail::max_payload;
using praenora::detail::open_channel_pair;
using praenora::detail::PayloadReader;
using praenora::detail::PayloadWriter;
using ReadStatus = praenora::detail::Channel::ReadStatus;
using Clock = std::chrono::steady_clock;

namespace {

// Every kind of frame, in both directions.
const std::vector<FrameKind>& all_kinds() {
    static const std::vector<FrameKind> kinds = {
        FrameKind::hello,        FrameKind::load,        FrameKind::create,
        FrameKind::initialize,   FrameKind::invoke,      FrameKind::finalize,
        FrameKind::destroy,      FrameKind::diagnostics, FrameKind::cancel,
        FrameKind::exit,         FrameKind::store_reply, FrameKind::ready,
        FrameKind::loaded,       FrameKind::load_failed, FrameKind::sink,
        FrameKind::log,          FrameKind::lookup,      FrameKind::result,
        FrameKind::provider_log, FrameKind::store_load,  FrameKind::store_save,
        FrameKind::store_refused};
    return kinds;
}

// The most that a test's socket buffers may hold: a socket whose buffers are larger is limited to
// it. The tests write entries of 1 to 4 MiB to a peer that does not read and expect the write to
// block, so a host whose default buffers take such an entry whole (Linux tuned for throughput:
// Rivanna's net.core.wmem_default and rmem_default are 16 MiB) would make them fail without a
// fault in the channel. A host with smaller defaults (stock Linux: 208 KiB; macOS: 8 KiB) keeps
// them.
constexpr int kMostBufferBytes = 256 << 10;

void limit_buffers(int descriptor) {
    for (const int option : {SO_SNDBUF, SO_RCVBUF}) {
        int current = 0;
        socklen_t size = sizeof current;
        if (::getsockopt(descriptor, SOL_SOCKET, option, &current, &size) == 0 &&
            current > kMostBufferBytes) {
            const int limit = kMostBufferBytes;
            ASSERT_EQ(::setsockopt(descriptor, SOL_SOCKET, option, &limit, sizeof limit), 0);
        }
    }
}

// A connected pair of channel ends.
struct Pair {
    Pair() {
        const auto ends = open_channel_pair();
        limit_buffers(ends.first);
        limit_buffers(ends.second);
        a = std::make_unique<Channel>(ends.first);
        b = std::make_unique<Channel>(ends.second);
    }
    std::unique_ptr<Channel> a, b;
};

// Writes raw bytes to `descriptor`, all of them.
void write_raw(int descriptor, const std::string& bytes) {
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::send(descriptor, bytes.data() + done, bytes.size() - done, 0);
        ASSERT_GT(n, 0);
        done += static_cast<std::size_t>(n);
    }
}

std::string header_bytes(std::uint32_t magic, std::uint32_t kind, std::uint64_t id,
                         std::uint32_t length) {
    return PayloadWriter().u32(magic).u32(kind).u64(id).u32(length).take();
}

std::string as_string(const std::array<unsigned char, kFrameHeaderSize>& header) {
    return std::string(reinterpret_cast<const char*>(header.data()), header.size());
}

// A payload of `size` bytes that may be sent with `kind`, within its limit.
std::string payload_of(FrameKind kind, std::size_t size) {
    std::string payload(std::min(size, max_payload(kind)), '\0');
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>(i * 7 + 1);
    }
    return payload;
}

} // namespace

TEST(Channel, HeaderIsLittleEndian) {
    const auto header = encode_header(FrameKind::invoke, 0x0102030405060708ULL, 0x0A0B0C0D);
    const std::string expected = std::string("\x50\x52\x4D\x48", 4) +                 // "PRMH"
                                 std::string("\x05\x00\x00\x00", 4) +                 // invoke
                                 std::string("\x08\x07\x06\x05\x04\x03\x02\x01", 8) + // the id
                                 std::string("\x0D\x0C\x0B\x0A", 4);                  // the length
    EXPECT_EQ(as_string(header), expected);
    EXPECT_EQ(kFrameMagic, 0x484D5250u);

    std::string problem;
    const auto decoded = decode_header(header.data(), problem);
    // invoke allows any length that fits the header.
    ASSERT_TRUE(decoded.has_value()) << problem;
    EXPECT_EQ(decoded->kind, FrameKind::invoke);
    EXPECT_EQ(decoded->id, 0x0102030405060708ULL);
    EXPECT_EQ(decoded->payload_size, 0x0A0B0C0Du);
}

TEST(Channel, PayloadFieldsRoundTrip) {
    const std::string payload = PayloadWriter()
                                    .u8(0xFE)
                                    .u32(0xDEADBEEF)
                                    .u64(std::numeric_limits<std::uint64_t>::max() - 1)
                                    .i32(-5)
                                    .bytes("t\xC3\xABxt")
                                    .take();
    PayloadReader reader(payload);
    std::uint8_t a = 0;
    std::uint32_t b = 0;
    std::uint64_t c = 0;
    std::int32_t d = 0;
    ASSERT_TRUE(reader.u8(a));
    ASSERT_TRUE(reader.u32(b));
    ASSERT_TRUE(reader.u64(c));
    ASSERT_TRUE(reader.i32(d));
    EXPECT_EQ(a, 0xFE);
    EXPECT_EQ(b, 0xDEADBEEFu);
    EXPECT_EQ(c, std::numeric_limits<std::uint64_t>::max() - 1);
    EXPECT_EQ(d, -5);
    EXPECT_EQ(reader.rest(), "t\xC3\xABxt");

    // A field that runs past the end is refused.
    // A reader views its payload: the payload outlives it.
    const std::string short_payload("\x01\x02\x03", 3);
    PayloadReader short_reader(short_payload);
    std::uint32_t too_long = 0;
    EXPECT_FALSE(short_reader.u32(too_long));

    // Bytes of a given length, then the rest; more than are left are refused, and nothing is
    // taken then.
    const std::string bytes_payload = PayloadWriter().u32(3).bytes("abcdef").take();
    PayloadReader bytes_reader(bytes_payload);
    std::uint32_t size = 0;
    std::string_view field = "unchanged";
    ASSERT_TRUE(bytes_reader.u32(size));
    EXPECT_FALSE(bytes_reader.bytes(7, field));
    EXPECT_EQ(field, "unchanged");
    ASSERT_TRUE(bytes_reader.bytes(size, field));
    EXPECT_EQ(field, "abc");
    EXPECT_EQ(bytes_reader.rest(), "def");
}

TEST(Channel, EveryKindHasANameAndALimit) {
    for (const FrameKind kind : all_kinds()) {
        const char* name = frame_kind_name(static_cast<std::uint32_t>(kind));
        ASSERT_NE(name, nullptr) << static_cast<std::uint32_t>(kind);
        EXPECT_GT(std::strlen(name), 0u);
    }
    EXPECT_EQ(frame_kind_name(0), nullptr);
    EXPECT_EQ(frame_kind_name(12), nullptr);
    EXPECT_EQ(frame_kind_name(27), nullptr);
    EXPECT_EQ(frame_kind_name(0xFFFFFFFFu), nullptr);
    // Frames that say everything in their header carry no payload.
    EXPECT_EQ(max_payload(FrameKind::cancel), 0u);
    EXPECT_EQ(max_payload(FrameKind::exit), 0u);
    // What the module host sends is bounded well below what a header can announce.
    for (const FrameKind kind :
         {FrameKind::ready, FrameKind::loaded, FrameKind::load_failed, FrameKind::sink,
          FrameKind::log, FrameKind::lookup, FrameKind::result, FrameKind::provider_log,
          FrameKind::store_load, FrameKind::store_refused}) {
        EXPECT_LE(max_payload(kind), std::size_t{4} << 20)
            << frame_kind_name(static_cast<std::uint32_t>(kind));
    }
    // The store's frames carry a key of at most 256 bytes and an entry of at most 128 MiB, and a
    // few fields: a save no more than the entry limit and the key limit together.
    constexpr std::size_t kKey = 256;
    constexpr std::size_t kEntry = 134217728;
    EXPECT_EQ(max_payload(FrameKind::store_load), 4 + kKey);
    EXPECT_EQ(max_payload(FrameKind::store_save), 4 + 4 + kKey + kEntry);
    EXPECT_EQ(max_payload(FrameKind::store_refused), 4 + 8 + 8 + kKey);
    EXPECT_EQ(max_payload(FrameKind::store_reply), 1 + kEntry);
}

TEST(Channel, EveryKindRoundTrips) {
    Pair pair;
    std::uint64_t id = 1;
    for (const FrameKind kind : all_kinds()) {
        SCOPED_TRACE(frame_kind_name(static_cast<std::uint32_t>(kind)));
        Frame frame;
        frame.kind = kind;
        frame.id = id++ * 0x100000001ULL;
        frame.payload = payload_of(kind, 1000);
        std::string problem;
        ASSERT_TRUE(pair.a->write(frame, &problem)) << problem;
        const Channel::ReadResult read = pair.b->read();
        ASSERT_EQ(read.status, ReadStatus::frame) << read.problem;
        EXPECT_EQ(read.frame.kind, frame.kind);
        EXPECT_EQ(read.frame.id, frame.id);
        EXPECT_EQ(read.frame.payload, frame.payload);
    }
    // And the other way, with the encoded form written as one block.
    Frame frame;
    frame.kind = FrameKind::sink;
    frame.id = 42;
    frame.payload = std::string("\x00", 1) + "chunk";
    write_raw(pair.b->descriptor(), encode_frame(frame));
    const Channel::ReadResult read = pair.a->read();
    ASSERT_EQ(read.status, ReadStatus::frame) << read.problem;
    EXPECT_EQ(read.frame.payload, frame.payload);
}

TEST(Channel, ReadsAPayloadSplitAcrossReads) {
    Pair pair;
    Frame frame;
    frame.kind = FrameKind::invoke;
    frame.id = 7;
    frame.payload.assign(300000, 'x');
    frame.payload[0] = 'a';
    frame.payload.back() = 'z';
    const std::string bytes = encode_frame(frame);
    const int descriptor = pair.a->descriptor();
    // In pieces, one inside the header, with pauses between them.
    std::thread writer([&bytes, descriptor] {
        const std::vector<std::size_t> cuts = {3, 11, kFrameHeaderSize, 5000, 120001, bytes.size()};
        std::size_t done = 0;
        for (const std::size_t cut : cuts) {
            write_raw(descriptor, bytes.substr(done, cut - done));
            done = cut;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    const Channel::ReadResult read = pair.b->read();
    writer.join();
    ASSERT_EQ(read.status, ReadStatus::frame) << read.problem;
    EXPECT_EQ(read.frame.payload, frame.payload);
}

// A payload's buffer grows with the bytes that arrive, and never beyond the length its header
// announced: a reader of a large frame (a compiled model of the store) holds no more than that.
TEST(Channel, APayloadNeverTakesMoreThanItsAnnouncedLength) {
    Pair pair;
    Frame frame;
    frame.kind = FrameKind::store_save;
    frame.id = 3;
    frame.payload = payload_of(FrameKind::store_save, (std::size_t{3} << 20) + 5);
    std::string problem;
    std::thread writer([&pair, &frame, &problem] { pair.a->write(frame, &problem); });
    const Channel::ReadResult read = pair.b->read();
    writer.join();
    ASSERT_EQ(read.status, ReadStatus::frame) << read.problem << problem;
    EXPECT_EQ(read.frame.payload, frame.payload);
    EXPECT_LE(read.frame.payload.capacity(), frame.payload.size() + 64);
}

// A frame written from parts travels as the frame of their concatenation; parts over the kind's
// limit together are refused, and nothing is written.
TEST(Channel, WritesAFrameFromItsParts) {
    Pair pair;
    std::string problem;
    ASSERT_TRUE(pair.a->write_parts(FrameKind::store_save, 12,
                                    {std::string_view("\x01\x00\x00\x00", 4), "", "key", "entry"},
                                    &problem))
        << problem;
    const Channel::ReadResult read = pair.b->read();
    ASSERT_EQ(read.status, ReadStatus::frame) << read.problem;
    EXPECT_EQ(read.frame.kind, FrameKind::store_save);
    EXPECT_EQ(read.frame.id, 12u);
    EXPECT_EQ(read.frame.payload, std::string("\x01\x00\x00\x00", 4) + "keyentry");

    const std::string half(max_payload(FrameKind::store_load) / 2 + 1, 'k');
    EXPECT_FALSE(pair.a->write_parts(FrameKind::store_load, 13, {half, half}, &problem));
    EXPECT_NE(problem.find("at most"), std::string::npos) << problem;
    ASSERT_TRUE(pair.a->write_parts(FrameKind::store_load, 14, {"next"}, &problem)) << problem;
    const Channel::ReadResult next = pair.b->read();
    ASSERT_EQ(next.status, ReadStatus::frame) << next.problem;
    EXPECT_EQ(next.frame.id, 14u);
    EXPECT_EQ(next.frame.payload, "next");
}

// A write with a deadline gives up at it when the peer takes no more bytes (it does not read),
// instead of blocking for good; one that had written part of its frame leaves the channel unusable
// (broken(): every later write fails at once). A peer that reads gets the whole frame.
TEST(Channel, AWriteWithADeadlineGivesUpWhenThePeerDoesNotRead) {
    Pair pair;
    const std::string entry(std::size_t{4} << 20, 'e');
    const auto started = Clock::now();
    std::string problem;
    EXPECT_EQ(pair.a->write_parts_until(FrameKind::store_reply, 1, {"\x01", entry},
                                        Clock::now() + std::chrono::milliseconds(200), &problem),
              Channel::WriteStatus::timed_out);
    EXPECT_LT(Clock::now() - started, std::chrono::seconds(5));
    EXPECT_NE(problem.find("deadline"), std::string::npos) << problem;
    EXPECT_TRUE(pair.a->broken());
    EXPECT_EQ(pair.a->write_parts_until(FrameKind::cancel, 2, {},
                                        Clock::now() + std::chrono::seconds(5), &problem),
              Channel::WriteStatus::failed);
    EXPECT_NE(problem.find("cut short"), std::string::npos) << problem;
    EXPECT_FALSE(pair.a->write(Frame{FrameKind::cancel, 3, {}}));

    Pair reading;
    std::thread reader([&reading, &entry] {
        const Channel::ReadResult read = reading.b->read();
        EXPECT_EQ(read.status, ReadStatus::frame) << read.problem;
        EXPECT_EQ(read.frame.payload, "\x01" + entry);
    });
    EXPECT_EQ(reading.a->write_parts_until(FrameKind::store_reply, 2, {"\x01", entry},
                                           Clock::now() + std::chrono::seconds(30), &problem),
              Channel::WriteStatus::written)
        << problem;
    reader.join();
    EXPECT_FALSE(reading.a->broken());
}

// A timed write leaves a read on the same end alone: another thread reading a frame that arrives
// in pieces while the write waits keeps reading (no read ever finds the descriptor non-blocking),
// and gets the frame whole.
TEST(Channel, ATimedWriteDoesNotDisturbAReadOnTheSameEnd) {
    Pair pair;
    Channel::ReadResult read;
    std::thread reader([&pair, &read] { read = pair.a->read(); });
    const std::string entry(std::size_t{1} << 20, 'e');
    std::thread writer([&pair, &entry] {
        EXPECT_EQ(pair.a->write_parts_until(FrameKind::store_reply, 1, {"\x01", entry},
                                            Clock::now() + std::chrono::milliseconds(400)),
                  Channel::WriteStatus::timed_out);
    });
    // While the write waits: the first bytes of a frame, then, a while later, the rest.
    const std::string frame = encode_frame(Frame{FrameKind::result, 9, std::string(5, '\0')});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    write_raw(pair.b->descriptor(), frame.substr(0, 3));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    write_raw(pair.b->descriptor(), frame.substr(3));
    writer.join();
    reader.join();
    EXPECT_EQ(read.status, ReadStatus::frame) << read.problem;
    EXPECT_EQ(read.frame.id, 9u);
}

// A timed write that does not get its turn (another writer holds the channel) writes nothing: the
// channel stays usable, and the same frame can be written again later, as a cancel frame is.
TEST(Channel, ATimedWriteThatGetsNoTurnWritesNothing) {
    Pair pair;
    const std::string entry(std::size_t{4} << 20, 'e');
    // An untimed writer holds the channel until the peer reads its frame.
    std::thread holder([&pair, &entry] {
        EXPECT_TRUE(pair.a->write_parts(FrameKind::store_reply, 1, {"\x01", entry}));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(pair.a->write_parts_until(FrameKind::cancel, 2, {},
                                        Clock::now() + std::chrono::milliseconds(50)),
              Channel::WriteStatus::timed_out);
    EXPECT_FALSE(pair.a->broken());
    const Channel::ReadResult big = pair.b->read();
    holder.join();
    EXPECT_EQ(big.status, ReadStatus::frame) << big.problem;
    EXPECT_EQ(
        pair.a->write_parts_until(FrameKind::cancel, 2, {}, Clock::now() + std::chrono::seconds(5)),
        Channel::WriteStatus::written);
    const Channel::ReadResult cancel = pair.b->read();
    EXPECT_EQ(cancel.status, ReadStatus::frame) << cancel.problem;
    EXPECT_EQ(cancel.frame.kind, FrameKind::cancel);
    EXPECT_EQ(cancel.frame.id, 2u);
}

// A write in slices asks after each slice it waited whether to go on: told to stop, it stops
// within a slice, and a frame it had begun leaves the channel unusable.
TEST(Channel, AWriteInSlicesStopsWhenToldTo) {
    Pair pair;
    const std::string entry(std::size_t{4} << 20, 'e');
    const auto stop_at = Clock::now() + std::chrono::milliseconds(150);
    const auto started = Clock::now();
    std::string problem;
    EXPECT_EQ(pair.a->write_parts_while(
                  FrameKind::store_reply, 1, {"\x01", entry},
                  [stop_at] { return Clock::now() < stop_at; }, std::nullopt, &problem),
              Channel::WriteStatus::stopped);
    const auto took = Clock::now() - started;
    EXPECT_GE(took, std::chrono::milliseconds(140));
    EXPECT_LT(took, std::chrono::seconds(2));
    EXPECT_TRUE(pair.a->broken());

    // With a peer that reads, it writes the whole frame.
    Pair reading;
    std::thread reader([&reading, &entry] {
        const Channel::ReadResult read = reading.b->read();
        EXPECT_EQ(read.status, ReadStatus::frame) << read.problem;
        EXPECT_EQ(read.frame.payload, "\x01" + entry);
    });
    EXPECT_EQ(reading.a->write_parts_while(
                  FrameKind::store_reply, 2, {"\x01", entry}, [] { return true; }, std::nullopt),
              Channel::WriteStatus::written);
    reader.join();
}

// A peer that keeps taking a little at a time (1 KiB every few milliseconds) cannot stretch a
// bounded write: a write with a deadline gives up at it, and one in slices stops within a slice of
// being told to, however often the peer makes room (the system's send timeout bounds each wait for
// room, not a whole write, so the channel bounds the write itself).
TEST(Channel, ABoundedWriteKeepsItsBoundAgainstAPeerThatReadsSlowly) {
    const std::string entry(std::size_t{4} << 20, 'e');
    for (const bool sliced : {false, true}) {
        SCOPED_TRACE(sliced ? "in slices" : "with a deadline");
        Pair pair;
        std::atomic<bool> done{false};
        std::thread trickle([&pair, &done] {
            char buffer[1024];
            while (!done.load()) {
                pollfd readable{pair.b->descriptor(), POLLIN, 0};
                if (::poll(&readable, 1, 2) > 0 &&
                    ::recv(pair.b->descriptor(), buffer, sizeof buffer, 0) <= 0) {
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
        const auto started = Clock::now();
        const auto bound = started + std::chrono::milliseconds(300);
        std::string problem;
        const Channel::WriteStatus status =
            sliced ? pair.a->write_parts_while(
                         FrameKind::store_reply, 1, {"\x01", entry},
                         [bound] { return Clock::now() < bound; }, std::nullopt, &problem)
                   : pair.a->write_parts_until(FrameKind::store_reply, 1, {"\x01", entry}, bound,
                                               &problem);
        const auto took = Clock::now() - started;
        done.store(true);
        trickle.join();
        EXPECT_EQ(status, sliced ? Channel::WriteStatus::stopped : Channel::WriteStatus::timed_out)
            << problem;
        EXPECT_GE(took, std::chrono::milliseconds(290));
        EXPECT_LT(took, std::chrono::seconds(1));
        EXPECT_TRUE(pair.a->broken());
    }
}

TEST(Channel, RefusesABadMagic) {
    Pair pair;
    write_raw(pair.a->descriptor(),
              header_bytes(0x12345678, static_cast<std::uint32_t>(FrameKind::log), 0, 4) + "abcd");
    const Channel::ReadResult read = pair.b->read();
    EXPECT_EQ(read.status, ReadStatus::broken);
    EXPECT_NE(read.problem.find("magic"), std::string::npos) << read.problem;
}

TEST(Channel, RefusesAnUnknownKind) {
    Pair pair;
    write_raw(pair.a->descriptor(), header_bytes(kFrameMagic, 99, 0, 0));
    const Channel::ReadResult read = pair.b->read();
    EXPECT_EQ(read.status, ReadStatus::broken);
    EXPECT_NE(read.problem.find("kind"), std::string::npos) << read.problem;
}

TEST(Channel, RefusesALengthOverTheKindsLimitWithoutAllocatingIt) {
    // Announced, never sent: a reader that allocated first would try to reserve 4 GiB.
    for (const FrameKind kind :
         {FrameKind::sink, FrameKind::log, FrameKind::result, FrameKind::loaded, FrameKind::cancel,
          FrameKind::store_save, FrameKind::store_reply}) {
        SCOPED_TRACE(frame_kind_name(static_cast<std::uint32_t>(kind)));
        Pair pair;
        write_raw(pair.a->descriptor(),
                  header_bytes(kFrameMagic, static_cast<std::uint32_t>(kind), 3, 0xFFFFFFFFu));
        const Channel::ReadResult read = pair.b->read();
        EXPECT_EQ(read.status, ReadStatus::broken);
        EXPECT_NE(read.problem.find("limit"), std::string::npos) << read.problem;
        EXPECT_EQ(read.frame.payload.capacity() < (std::size_t{1} << 20), true);

        std::string problem;
        const auto header = encode_header(kind, 3, 0);
        std::array<unsigned char, kFrameHeaderSize> huge = header;
        huge[16] = huge[17] = huge[18] = huge[19] = 0xFF;
        EXPECT_FALSE(decode_header(huge.data(), problem).has_value());
    }
    // A frame that may not carry so much is never encoded either.
    Frame frame;
    frame.kind = FrameKind::result;
    frame.payload.assign(max_payload(FrameKind::result) + 1, 'x');
    EXPECT_THROW(static_cast<void>(encode_frame(frame)), praenora::Error);
}

TEST(Channel, RefusesTheKindsItIsToldToAtTheirHeader) {
    // The store's kinds from the module host of a step that does not share compiled models: such a
    // frame is refused as soon as its header is read, whatever length it announces, and the reader
    // waits for none of its payload; other kinds still pass.
    Pair pair;
    pair.b->refuse_kinds({FrameKind::store_load, FrameKind::store_save}, "is not for this channel");
    Frame log;
    log.kind = FrameKind::log;
    log.payload = PayloadWriter().i32(2).bytes("hi").take();
    ASSERT_TRUE(pair.a->write(log));
    const Channel::ReadResult passed = pair.b->read();
    ASSERT_EQ(passed.status, ReadStatus::frame) << passed.problem;
    EXPECT_EQ(passed.frame.kind, FrameKind::log);

    write_raw(pair.a->descriptor(),
              header_bytes(kFrameMagic, static_cast<std::uint32_t>(FrameKind::store_save), 1,
                           std::uint32_t{64} << 20));
    const auto start = Clock::now();
    const Channel::ReadResult read = pair.b->read(Clock::now() + std::chrono::seconds(5));
    EXPECT_EQ(read.status, ReadStatus::broken);
    EXPECT_EQ(read.problem, "a store_save frame is not for this channel");
    EXPECT_LT(Clock::now() - start, std::chrono::seconds(1));
    EXPECT_LT(read.frame.payload.capacity(), std::size_t{1} << 20);
}

TEST(Channel, ATruncatedHeaderIsBroken) {
    Pair pair;
    write_raw(pair.a->descriptor(), header_bytes(kFrameMagic, 1, 0, 0).substr(0, 9));
    pair.a->shut_down();
    const Channel::ReadResult read = pair.b->read();
    EXPECT_EQ(read.status, ReadStatus::broken);
    EXPECT_NE(read.problem.find("header"), std::string::npos) << read.problem;
}

TEST(Channel, ATruncatedPayloadIsBrokenAndNeverAllocatedWhole) {
    // invoke may carry nearly 4 GiB: the reader grows the payload only as bytes arrive.
    Pair pair;
    write_raw(
        pair.a->descriptor(),
        header_bytes(kFrameMagic, static_cast<std::uint32_t>(FrameKind::invoke), 9, 0xFFFFFF00u) +
            std::string(1000, 'p'));
    pair.a->shut_down();
    const auto start = Clock::now();
    const Channel::ReadResult read = pair.b->read();
    EXPECT_EQ(read.status, ReadStatus::broken);
    EXPECT_NE(read.problem.find("payload"), std::string::npos) << read.problem;
    EXPECT_LT(read.frame.payload.capacity(), std::size_t{1} << 20);
    EXPECT_LT(Clock::now() - start, std::chrono::seconds(5));
}

TEST(Channel, EndBetweenFramesIsACleanEnd) {
    Pair pair;
    Frame frame;
    frame.kind = FrameKind::exit;
    ASSERT_TRUE(pair.a->write(frame));
    pair.a->shut_down();
    EXPECT_EQ(pair.b->read().status, ReadStatus::frame);
    const Channel::ReadResult end = pair.b->read();
    EXPECT_EQ(end.status, ReadStatus::end) << end.problem;
}

TEST(Channel, ReadsWithADeadline) {
    Pair pair;
    const auto start = Clock::now();
    const Channel::ReadResult read = pair.b->read(Clock::now() + std::chrono::milliseconds(50));
    EXPECT_EQ(read.status, ReadStatus::timed_out);
    EXPECT_GE(Clock::now() - start, std::chrono::milliseconds(50));

    Frame frame;
    frame.kind = FrameKind::ready;
    frame.payload = "abc";
    ASSERT_TRUE(pair.a->write(frame));
    const Channel::ReadResult ready = pair.b->read(Clock::now() + std::chrono::seconds(10));
    ASSERT_EQ(ready.status, ReadStatus::frame) << ready.problem;
    EXPECT_EQ(ready.frame.payload, "abc");
}

TEST(Channel, WritingToAPeerThatIsGoneFailsWithoutSigpipe) {
    Pair pair;
    pair.b.reset(); // the peer's end is closed
    Frame frame;
    frame.kind = FrameKind::log;
    frame.payload = PayloadWriter().i32(2).bytes(std::string(200000, 'm')).take();
    std::string problem;
    // A SIGPIPE would end this process: the default action is to terminate.
    EXPECT_FALSE(pair.a->write(frame, &problem));
    EXPECT_FALSE(problem.empty());
    EXPECT_FALSE(pair.a->write(frame, &problem));
}

TEST(Channel, ShutDownEndsABlockedRead) {
    Pair pair;
    Channel::ReadResult read;
    std::thread reader([&] { read = pair.b->read(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    pair.b->shut_down();
    reader.join();
    EXPECT_EQ(read.status, ReadStatus::end);
}

#endif // _WIN32
