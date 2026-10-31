# Local registry test

This test is not automated CI. On the WoW project's WSL build host, run it
from the operations repository root with `PLAYERBOTS_SOURCE` set to the
absolute path of this module checkout:

```bash
scripts/wow-build-host-guard && mkdir -p work/registry-test && (
  flock -n 9 &&
  c++ -std=c++17 -pthread -I "$PLAYERBOTS_SOURCE/src/Guide" \
    "$PLAYERBOTS_SOURCE/tests/BotActivityRegistryTest.cpp" \
    -o work/registry-test/bot-activity-registry &&
  work/registry-test/bot-activity-registry
) 9</var/lib/wow-worker/build.lock
```

This tests in-process reservation behavior; it does not verify dynamic module
sharing, the lifetime of native actions, or gameplay.
