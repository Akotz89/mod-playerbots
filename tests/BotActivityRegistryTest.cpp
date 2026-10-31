#include "BotActivityRegistry.h"

#include <cstdlib>
#include <atomic>
#include <functional>
#include <iostream>
#include <thread>
#include <vector>

namespace
{
void Require(bool condition, char const* message)
{
    if (condition)
        return;

    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
}
}

int main()
{
    BotActivity::OwnerToken const runA = {BotActivity::OwnerKind::DungeonSimRun, 10, 100};
    BotActivity::OwnerToken const runB = {BotActivity::OwnerKind::DungeonSimRun, 11, 100};
    BotActivity::OwnerToken const staleRunA = {BotActivity::OwnerKind::DungeonSimRun, 9, 100};
    BotActivity::OwnerToken const outdoorA = {BotActivity::OwnerKind::OutdoorQuest, 20, 200};
    BotActivity::OwnerToken const staleOutdoorA = {BotActivity::OwnerKind::OutdoorQuest, 19, 200};
    BotActivity::OwnerToken const otherOutdoorObjective = {BotActivity::OwnerKind::OutdoorQuest, 20, 201};

    Require(BotActivity::TryReserveMany(runA, {3, 1, 2}), "first owner could not reserve its complete set");
    Require(BotActivity::IsReserved(1) && BotActivity::IsReserved(2) && BotActivity::IsReserved(3),
        "successful multi-reserve did not publish every member");
    Require(!BotActivity::TryReserveMany(runB, {2, 4}), "conflicting multi-reserve was accepted");
    Require(!BotActivity::IsReserved(4), "failed multi-reserve partially committed an unconflicted member");
    Require(BotActivity::TryReserveMany(runA, {1, 2, 3}), "same-owner idempotent reserve failed");
    Require(!BotActivity::TryReserveMany(outdoorA, {3, 5}),
        "outdoor quest reservation ignored an existing dungeon owner");
    Require(!BotActivity::IsReserved(5),
        "failed cross-activity reservation partially committed an unconflicted member");

    BotActivity::ReleaseMany(runB, {2});
    Require(BotActivity::IsReserved(2), "competing owner released another owner's member");
    BotActivity::ReleaseMany(staleRunA, {2});
    Require(BotActivity::IsReserved(2), "stale generation released the current owner's member");

    BotActivity::ReleaseMany(runA, {2});
    Require(!BotActivity::IsReserved(2), "owning generation could not release its member");
    Require(BotActivity::IsReserved(1) && BotActivity::IsReserved(3),
        "targeted release erased unrelated members");
    Require(BotActivity::TryReserveMany(runB, {2, 4}), "released members could not be atomically reused");

    BotActivity::ReleaseMany(runA, {3});
    Require(BotActivity::TryReserveMany(outdoorA, {3, 5, 5}),
        "outdoor quest reservation could not claim a released complete set");
    Require(BotActivity::IsReserved(3) && BotActivity::IsReserved(5),
        "successful outdoor quest reservation did not publish every unique member");
    Require(!BotActivity::TryReserveMany(runA, {3, 6}),
        "dungeon owner ignored an existing outdoor quest reservation");
    Require(!BotActivity::IsReserved(6),
        "failed reverse cross-activity reservation partially committed an unconflicted member");

    BotActivity::ReleaseMany(runB, {3, 5});
    Require(BotActivity::IsReserved(3) && BotActivity::IsReserved(5),
        "competing activity kind released an outdoor quest reservation");
    BotActivity::ReleaseMany(staleOutdoorA, {3, 5});
    Require(BotActivity::IsReserved(3) && BotActivity::IsReserved(5),
        "stale outdoor quest generation released the current reservation");
    BotActivity::ReleaseMany(otherOutdoorObjective, {3, 5});
    Require(BotActivity::IsReserved(3) && BotActivity::IsReserved(5),
        "different outdoor quest objective released the current reservation");

    BotActivity::ReleaseMany(outdoorA, {3, 5});
    Require(!BotActivity::IsReserved(3) && !BotActivity::IsReserved(5),
        "exact outdoor quest owner could not release its reservations");
    Require(BotActivity::IsReserved(1) && BotActivity::IsReserved(2) && BotActivity::IsReserved(4),
        "outdoor quest release erased reservations owned by dungeon activities");

    std::atomic<bool> start{false};
    bool first = false, second = false;
    auto claim = [&](BotActivity::OwnerToken const& owner, uint32_t extra, bool& result) {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        result = BotActivity::TryReserveMany(owner, {20, extra});
    };
    std::thread a(claim, std::cref(runA), 21, std::ref(first));
    std::thread b(claim, std::cref(runB), 22, std::ref(second));
    start.store(true, std::memory_order_release);
    a.join();
    b.join();
    Require(first != second, "overlapping simultaneous claims did not select exactly one owner");
    Require(BotActivity::IsReserved(20) && BotActivity::IsReserved(first ? 21 : 22) &&
        !BotActivity::IsReserved(first ? 22 : 21), "losing claim left a partial reservation");
    BotActivity::ReleaseMany(first ? runA : runB, {20, 21, 22});

    std::cout << "PASS: owner-aware atomic cross-activity bot registry, including OutdoorQuest\n";
    return EXIT_SUCCESS;
}
