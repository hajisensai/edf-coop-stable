// The transport rewrite's own tests (docs/netcode-rewrite-plan.md W1): datagram classes, the record type
// meter, duplicates, fragments, congestion budgets and the multipath direct link. No game needed.
#include <winsock2.h>
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../src/congestion.h"
#include "../src/fragment.h"
#include "../src/netclass.h"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                   \
    do {                                                              \
        ++g_checks;                                                   \
        if (!(cond)) {                                                \
            ++g_failures;                                             \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                             \
    } while (0)

const std::string kA = "0002bbbbbbbbbbbbbbbbbbbbbbbbbbbb";
const std::string kB = "0002cccccccccccccccccccccccccccc";

// A plaintext controller datagram: the 8-byte header, then records written as the game writes them.
struct Datagram {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(dn::kControllerDatagramHeader, 0);
    Datagram& record(uint32_t type, size_t size) {
        const uint32_t header = static_cast<uint32_t>(size) << 20 | (type & 0xFFFFF);
        const auto* h = reinterpret_cast<const uint8_t*>(&header);
        bytes.insert(bytes.end(), h, h + 4);
        bytes.insert(bytes.end(), size, static_cast<uint8_t>(type >> 8));
        return *this;
    }
    // SendReliable (12D0AC0): the envelope 0x00401200 and a sequence number, then the record.
    Datagram& reliable(uint32_t type, size_t size, uint32_t seq) {
        const uint32_t envelope = 0x00401200;
        bytes.insert(bytes.end(), reinterpret_cast<const uint8_t*>(&envelope), reinterpret_cast<const uint8_t*>(&envelope) + 4);
        bytes.insert(bytes.end(), reinterpret_cast<const uint8_t*>(&seq), reinterpret_cast<const uint8_t*>(&seq) + 4);
        return record(type, size);
    }
};

void testParseRecords() {
    printf("netclass: the records of a controller datagram\n");
    Datagram d;
    d.record(0x2100, 40).reliable(0x3000, 100, 7).record(0x1400, 4);
    std::vector<dn::RecordInfo> records;
    CHECK(dn::parseControllerDatagram(d.bytes.data(), d.bytes.size(), records));
    CHECK(records.size() == 4);
    CHECK(records[0].type == 0x2100 && records[0].size == 40 && !records[0].reliable);
    CHECK(records[1].type == 0x1200 && !records[1].reliable);  // the envelope itself
    CHECK(records[2].type == 0x3000 && records[2].size == 100 && records[2].reliable);
    CHECK(records[3].type == 0x1400 && !records[3].reliable);
    // A record running past the end: not parsed.
    std::vector<uint8_t> cut(d.bytes.begin(), d.bytes.end() - 2);
    CHECK(!dn::parseControllerDatagram(cut.data(), cut.size(), records));
    CHECK(!dn::parseControllerDatagram(d.bytes.data(), 4, records));
}

void testClassify() {
    printf("netclass: state, event, control and unknown datagrams\n");
    dn::StateLearner learner;
    std::vector<dn::RecordInfo> records;
    Datagram pose;
    pose.record(0x2100, 60);
    dn::parseControllerDatagram(pose.bytes.data(), pose.bytes.size(), records);
    // Not learnt yet: unknown (repaired as before).
    CHECK(dn::classify(records, true, learner) == dn::TrafficClass::Unknown);
    // Sent every 90 ms to the same player: state after kSamples intervals.
    for (uint32_t i = 0; i <= dn::StateLearner::kSamples; ++i) learner.observe(kA, 0x2100, 1000 + i * 90);
    CHECK(learner.isState(0x2100));
    CHECK(dn::classify(records, true, learner) == dn::TrafficClass::State);
    // A long gap breaks a run: another type sent with gaps never becomes state.
    for (uint32_t i = 0; i < 100; ++i) learner.observe(kA, 0x2200, 1000 + i * (i % 10 == 9 ? 400 : 90));
    CHECK(!learner.isState(0x2200));
    // The event batch is never state, however often it goes.
    for (uint32_t i = 0; i < 100; ++i) learner.observe(kA, dn::kRecordEventBatch, 1000 + i * 16);
    CHECK(!learner.isState(dn::kRecordEventBatch));
    // A reliable record: the game resends it - event, also next to state.
    Datagram event;
    event.record(0x2100, 60).reliable(0x3000, 20, 1);
    dn::parseControllerDatagram(event.bytes.data(), event.bytes.size(), records);
    CHECK(dn::classify(records, true, learner) == dn::TrafficClass::Event);
    // ...but next to an unreliable record nothing resends, the old repair stays.
    Datagram mixed;
    mixed.record(0x2200, 30).reliable(0x3000, 20, 2);
    dn::parseControllerDatagram(mixed.bytes.data(), mixed.bytes.size(), records);
    CHECK(dn::classify(records, true, learner) == dn::TrafficClass::Unknown);
    // Only acknowledgements: control.
    Datagram ack;
    ack.record(0x1400, 4).record(0x1300, 4);
    dn::parseControllerDatagram(ack.bytes.data(), ack.bytes.size(), records);
    CHECK(dn::classify(records, true, learner) == dn::TrafficClass::Control);
    CHECK(dn::classify(records, false, learner) == dn::TrafficClass::Unknown);
    learner.forget();
    CHECK(!learner.isState(0x2100));
}

void testPendingDatagram() {
    printf("netclass: the flush hands the records to the send on the same thread, for that size only\n");
    Datagram d;
    d.record(0x2100, 60);
    const size_t n = d.bytes.size();
    dn::notePendingDatagram(d.bytes.data(), n);
    auto taken = dn::takePendingDatagram(n);
    CHECK(taken && taken->parsed && taken->records.size() == 1 && taken->records[0].type == 0x2100);
    CHECK(!dn::takePendingDatagram(n));  // taken once
    dn::notePendingDatagram(d.bytes.data(), n);
    CHECK(!dn::takePendingDatagram(n + 1));  // a packet of the plugin's own in between...
    CHECK(dn::takePendingDatagram(n).has_value());  // ...leaves the note for the game's datagram
    dn::notePendingDatagram(d.bytes.data(), n);
    bool other = true;
    std::thread([&] { other = dn::takePendingDatagram(n).has_value(); }).join();
    CHECK(!other);  // another thread's send is not this one
    CHECK(dn::takePendingDatagram(n).has_value());
}

void testRecordTypeMeter() {
    printf("netclass: per record type rate, bytes and interval\n");
    dn::RecordTypeMeter meter;
    std::vector<dn::RecordInfo> records;
    Datagram d;
    d.record(0x2100, 60).reliable(0x3000, 10, 1);
    dn::parseControllerDatagram(d.bytes.data(), d.bytes.size(), records);
    for (int i = 0; i < 10; ++i) meter.record(kA, records, 1000 + i * 100);
    meter.record(kB, records, 1050);
    meter.recordDatagram(dn::TrafficClass::Event, d.bytes.size());
    const auto lines = meter.take(1.0);
    CHECK(lines.size() == 4);  // the class line and three types
    bool pose = false, batch = false;
    for (const auto& line : lines) {
        if (line.find("NETTYPE 0x02100") == 0)
            pose = line.find("11 records") != std::string::npos && line.find("every 100 ms (min 100, max 100)") != std::string::npos;
        if (line.find("NETTYPE 0x03000") == 0) batch = line.find("11 reliable") != std::string::npos;
    }
    CHECK(pose);
    CHECK(batch);
    CHECK(lines[0].find("event 1") != std::string::npos);
    CHECK(meter.take(1.0).empty());
}

void testDuplicateFilter() {
    printf("netclass: a copy over a second path is delivered once\n");
    dn::DuplicateFilter filter;
    const uint8_t x[] = {1, 2, 3, 4}, y[] = {1, 2, 3, 5};
    CHECK(filter.first(kA, x, sizeof(x), 1000));
    CHECK(!filter.first(kA, x, sizeof(x), 1010));  // the copy
    CHECK(filter.first(kB, x, sizeof(x), 1010));   // the same bytes from someone else
    CHECK(filter.first(kA, y, sizeof(y), 1020));
    CHECK(filter.first(kA, x, sizeof(x), 1000 + dn::DuplicateFilter::kWindowMs + 100));  // long gone
    CHECK(filter.duplicates() == 1);
}


void testFragments() {
    printf("fragment: a datagram above 1170 bytes and a 140 KB message, in any order, copies and all\n");
    std::vector<uint8_t> datagram(1408);
    for (size_t i = 0; i < datagram.size(); ++i) datagram[i] = static_cast<uint8_t>(i * 7);
    auto parts = dn::splitIntoFragments(1, 0, 0, datagram.data(), datagram.size());
    CHECK(parts.size() == 2);
    for (const auto& p : parts) CHECK(p.size() <= dn::kFragmentPacket && dn::isFragment(p.data(), p.size()));
    dn::Reassembler r;
    CHECK(!r.add(kA, parts[1].data(), parts[1].size(), 1000));  // the last one first
    CHECK(!r.add(kA, parts[1].data(), parts[1].size(), 1001));  // and again: a copy
    auto done = r.add(kA, parts[0].data(), parts[0].size(), 1002);
    CHECK(done && done->bytes == datagram && done->tag == 0 && done->flags == 0);
    CHECK(r.buffered() == 0);

    // A start message of 1024 loadout records (about 143 bytes each).
    std::vector<uint8_t> big(1024 * 143);
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(i ^ (i >> 8));
    parts = dn::splitIntoFragments(77, dn::kFragmentBulk, 9, big.data(), big.size());
    CHECK(parts.size() == (big.size() + dn::kFragmentPayload - 1) / dn::kFragmentPayload);
    std::optional<dn::FragmentMessage> whole;
    for (size_t i = 0; i < parts.size(); ++i) {
        const auto& p = parts[(i * 37) % parts.size()];  // shuffled (37 is prime to the count)
        auto m = r.add(kB, p.data(), p.size(), 2000 + i);
        if (m) whole = std::move(m);
    }
    CHECK(whole && whole->bytes == big && whole->tag == 9 && whole->flags == dn::kFragmentBulk);
    // Too large, or empty: not split.
    std::vector<uint8_t> huge(dn::kMaxFragmentedBytes + 1);
    CHECK(dn::splitIntoFragments(2, 0, 0, huge.data(), huge.size()).empty());
    // A fragment that lies about its size, or a short one: dropped.
    auto bad = dn::splitIntoFragments(3, 0, 0, datagram.data(), datagram.size());
    bad[0].pop_back();
    CHECK(!r.add(kA, bad[0].data(), bad[0].size(), 3000) && r.malformed() == 1);
    // A message that never completes is let go after the timeout, and its memory with it.
    parts = dn::splitIntoFragments(4, 0, 0, datagram.data(), datagram.size());
    r.add(kA, parts[0].data(), parts[0].size(), 4000);
    CHECK(r.buffered() == datagram.size());
    r.add(kA, parts[0].data(), 3, 4000 + dn::Reassembler::kTimeoutMs + 1);  // anything, after the timeout
    CHECK(r.buffered() == 0 && r.dropped() == 1);
    // Per sender at most kMaxPartialPerSender messages wait: the oldest gives way.
    for (uint32_t id = 10; id < 10 + dn::Reassembler::kMaxPartialPerSender + 1; ++id) {
        parts = dn::splitIntoFragments(id, 0, 0, datagram.data(), datagram.size());
        r.add(kA, parts[0].data(), parts[0].size(), 5000 + id);
    }
    CHECK(r.dropped() == 2 && r.buffered() == dn::Reassembler::kMaxPartialPerSender * datagram.size());
}

void testRateController() {
    printf("congestion: the budget follows queueing delay and loss, not a fixed cap\n");
    dn::RateController c;
    CHECK(c.rate() == dn::RateController::kStartRate);
    uint64_t now = 1000;
    // A clean 40 ms path that is used: the rate grows.
    for (int i = 0; i < 200; ++i, now += 40) {
        c.onSent(c.rate() / 25, now);  // filling the rate (25 rounds a second)
        c.onRtt(40, now);
    }
    const uint32_t grown = c.rate();
    CHECK(grown > dn::RateController::kStartRate * 4);
    CHECK(c.baseRttMs() == 40 && c.queueMs() == 0);
    // A queue builds (the delay climbs 100 ms above the base): the rate comes down.
    for (int i = 0; i < 60; ++i, now += 140) {
        c.onSent(c.rate() / 7, now);
        c.onRtt(140, now);
    }
    CHECK(c.queueMs() == 100 && c.rate() < grown / 2);
    // Loss cuts it, once per round trip.
    dn::RateController lossy;
    lossy.onRtt(40, now);
    lossy.onLoss(now);
    lossy.onLoss(now + 1);
    CHECK(lossy.rate() == static_cast<uint32_t>(dn::RateController::kStartRate * dn::RateController::kLossCut));
    // Never below the game's own budget.
    for (int i = 0; i < 100; ++i) c.onLoss(now += 1000);
    CHECK(c.rate() == dn::RateController::kMinRate);
    // An idle path proves nothing: it does not grow while the sender leaves it empty.
    dn::RateController idle;
    for (int i = 0; i < 200; ++i, now += 40) idle.onRtt(40, now);
    CHECK(idle.rate() == dn::RateController::kStartRate);
    // The base round trip forgets a minimum older than its window (a route that got longer).
    dn::RateController moved;
    moved.onRtt(20, 100000);
    for (uint64_t t = 100000; t < 100000 + dn::RateController::kBaseWindowMs + 2000; t += 100) moved.onRtt(80, t);
    CHECK(moved.baseRttMs() == 80);
}

}  // namespace

int main() {
    testParseRecords();
    testClassify();
    testPendingDatagram();
    testRecordTypeMeter();
    testDuplicateFilter();
    testFragments();
    testRateController();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 && g_checks > 0 ? 0 : 1;
}
