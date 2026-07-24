# Apple Futex Fallback Design

## Goal

Keep the existing address-based `platform_futex` interface available when an
Apple SDK does not provide `<os/os_sync_wait_on_address.h>`, without adding a
stateful futex object or a process-wide parking table.

## Backend Selection

The Apple implementation selects its backend at compile time with
`__has_include`:

- When `<os/os_sync_wait_on_address.h>` is available, use the public
  `os_sync_wait_on_address` and `os_sync_wake_by_address_*` APIs.
- Otherwise, use the private `__ulock_wait2` and `__ulock_wake` APIs with
  `UL_COMPARE_AND_WAIT`.

This is intentionally an SDK-based decision. A binary built through the public
API branch requires an OS version that provides that API. Runtime fallback from
the public branch to ulock is outside this change.

## Semantics

Both backends retain the current contract:

- Wait only while the 32-bit word still equals the expected value.
- Permit spurious wakeups and let callers recheck their predicate.
- Signal at most one matching waiter.
- Broadcast to all matching waiters.
- Return `false` from timed wait only when the timeout expires.

The ulock timed wait converts milliseconds to the 64-bit nanosecond timeout
accepted by `__ulock_wait2`. A zero timeout returns `false` without entering the
kernel, because a zero ulock timeout represents an unbounded wait. Conversion
must saturate rather than overflow.

## Compatibility

The fallback requires an Apple system that exports `__ulock_wait2`, which sets
the practical lower bound at macOS 11 or iOS 14. The ulock functions are private
SPI: Apple does not guarantee source compatibility, binary compatibility, or
App Store acceptance. This limitation must be stated beside the fallback code.

Linux and Windows implementations are unchanged.

## Verification

- Compile the public branch with a current Apple SDK.
- Compile the fallback branch with an Apple SDK that lacks the public header,
  or force the capability macro off in an isolated compile check.
- Run sync tests on both Apple backend branches.
- Run the existing Linux and Windows sync tests to confirm those backends are
  unaffected.
