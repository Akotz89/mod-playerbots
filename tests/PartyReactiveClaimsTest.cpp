#include <cassert>

#include "../src/Mgr/Item/PartyReactiveClaims.h"

int main()
{
    using namespace PartyReactiveClaims;

    assert(!Register(0));
    assert(!Acquire(9000, 1, Now() + 1000, 1, 1));

    for (std::uint64_t actor = 1; actor <= 641; ++actor)
        assert(Register(actor));
    assert(participants.size() == 641);

    for (std::uint64_t actor = 1000; actor < 1065; ++actor)
        assert(Register(actor));
    for (std::uint64_t actor = 1000; actor < 1065; ++actor)
        assert(Acquire(actor, actor + 10000, Now() + 1000, 1, 1));
    assert(claims.size() == 65);

    assert(!Acquire(1000, 0, Now() + 1000, 1, 1));
    assert(!Acquire(1065, 1065 + 10000, Now() + 1000, 1, 1));
    assert(!Acquire(1065, 1000 + 10000, Now() + 1000, 1, 1));
    assert(!Acquire(1000, 20000, Now() + 1000, 1, 1));
    assert(!Allowed(1065, 1000 + 10000, 1, 1, true));
    assert(!Allowed(1000, 20000, 1, 1, true));
    assert(Allowed(1065, 1000 + 10000, 1, 1, false));
    assert(Allowed(1000, 20000, 1, 1, false));

    Target const owned{2, 3, 30000};
    assert(Register(2000));
    assert(Register(2001));
    assert(Acquire(2000, 30000, Now() + 1000, 2, 3));
    assert(!Acquire(2001, 30000, Now() + 1000, 2, 3));
    Release(2001, 30000, 2, 3);
    assert(claims.count(owned));
    Release(2000, 30000, 2, 3);
    assert(!claims.count(owned));

    participants[3000] = Now() - 1;
    assert(!Acquire(3000, 30000, Now() + 1000, 2, 3));
    assert(!participants.count(3000));
    assert(Register(3001));

    Target const expired{4, 5, 40000};
    claims[expired] = {4000, Now() - 1};
    assert(Register(4001));
    assert(Acquire(4001, 40000, Now() + 1000, 4, 5));
    assert(claims.count(expired));
    assert(claims.at(expired).actor == 4001);

    return 0;
}
