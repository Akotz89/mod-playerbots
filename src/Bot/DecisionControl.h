/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DECISIONCONTROL_H
#define PLAYERBOTS_DECISIONCONTROL_H

#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <thread>

class DecisionControl
{
public:
    using Clock = std::chrono::steady_clock;
    enum class Operation { Attack, Heal2050 };
    struct Context
    {
        std::uint64_t master = 0, bot = 0, group = 0, target = 0;
        std::uint32_t map = 0, instance = 0;
        Operation operation = Operation::Attack;
        std::uint64_t focus = 0, ownerSession = 0, botSession = 0, targetSession = 0;
        bool requireFocus = true;
        std::thread::id thread = std::this_thread::get_id();
        bool operator==(Context const& other) const
        {
            return master == other.master && bot == other.bot && group == other.group && target == other.target &&
                   map == other.map && instance == other.instance && operation == other.operation &&
                   focus == other.focus && requireFocus == other.requireFocus && ownerSession == other.ownerSession &&
                   botSession == other.botSession && targetSession == other.targetSession && thread == other.thread;
        }
    };
    struct Ticket
    {
        std::uint64_t lifetime, revision, snapshot;
        Context context;
        Clock::time_point expires;
    };

    explicit DecisionControl(std::uint64_t lifetime) : lifetime(lifetime) {}

    void Invalidate()
    {
        pending.reset();
        if (revision == std::numeric_limits<std::uint64_t>::max())
            exhausted = true;
        else
            ++revision;
    }
    void SetMode(bool automatic)
    {
        Invalidate();
        this->automatic = automatic;
    }
    bool Available() const { return lifetime && !exhausted; }
    bool CanSetMode() const { return Available() && revision < std::numeric_limits<std::uint64_t>::max(); }

    std::optional<Ticket> Observe(Context const& context, Clock::time_point now)
    {
        pending.reset();
        if (!lifetime || exhausted || snapshot == std::numeric_limits<std::uint64_t>::max() ||
            now > Clock::time_point::max() - std::chrono::milliseconds(500))
        {
            exhausted = true;
            return std::nullopt;
        }
        pending = Ticket{lifetime, revision, ++snapshot, context, now + std::chrono::milliseconds(500)};
        return pending;
    }
    bool Current(Ticket const& ticket, Context const& context, Clock::time_point now) const
    {
        return automatic && !exhausted && lifetime && ticket.lifetime == lifetime && ticket.revision == revision &&
               ticket.snapshot == snapshot && ticket.context == context && now < ticket.expires;
    }
    std::optional<Ticket> Consume(std::uint64_t expectedLifetime, std::uint64_t expectedRevision,
                                  std::uint64_t expectedSnapshot, std::uint64_t target, Context const& context,
                                  Clock::time_point now)
    {
        if (!pending || expectedLifetime != pending->lifetime || expectedRevision != pending->revision ||
            expectedSnapshot != pending->snapshot || !target || target != pending->context.target ||
            !Current(*pending, context, now))
            return std::nullopt;
        auto ticket = pending;
        pending.reset();
        return ticket;
    }
    std::optional<Ticket> Find(std::uint64_t expectedLifetime, std::uint64_t expectedRevision,
                               std::uint64_t expectedSnapshot, std::uint64_t target, Operation operation) const
    {
        if (!pending || pending->lifetime != expectedLifetime || pending->revision != expectedRevision ||
            pending->snapshot != expectedSnapshot || pending->context.target != target ||
            pending->context.operation != operation || !pending->context.requireFocus)
            return std::nullopt;
        return pending;
    }
    static bool ParseInteger(std::string_view text, std::uint64_t& value)
    {
        if (text.empty())
            return false;
        auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        return result.ec == std::errc{} && result.ptr == text.data() + text.size();
    }

    // Counters never wrap; public values also permit standalone boundary tests of the real state.
    const std::uint64_t lifetime;
    std::uint64_t revision = 1, snapshot = 0;
    bool automatic = true;

private:
    bool exhausted = false;
    std::optional<Ticket> pending;
};

#endif
