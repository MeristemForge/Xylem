# Net Leaf Refcount Design

## Goal

Make the public functions in `stream.c` and `datagram.c` protect the lifetime
of the object they dereference, without relying on TCP, UDP, TLS, or DTLS
wrapper references.

The caller must still pass a live reference. A function-local reference only
protects an operation that has entered the function; it cannot make an already
released pointer valid.

## Considered Approaches

1. Keep lifetime protection only in upper-layer wrappers. This avoids extra
   atomic operations but makes the leaf APIs unsafe when called directly and
   leaves correctness dependent on every caller.
2. Add references only to functions that can park. This protects blocking I/O
   but misses races in system calls, deadline updates, and close operations on
   multi-worker schedulers.
3. Make every leaf operation that dereferences an object hold a local reference.
   This gives `stream` and `datagram` a consistent ownership boundary. This is
   the selected approach.

## Behavior

- Deadline setters hold a local reference. They silently ignore updates after
  the object is closed.
- Address queries and shutdown operations hold a local reference and return
  `-1` when the object is closed.
- Interrupt operations hold a local reference while changing state and closing
  the waiter.
- Existing read, write, wait, and accept operations retain their current local
  reference protection.
- Object fields must not be read before the operation reference is acquired.
  In particular, datagram send validation that reads `connected` moves after
  `_datagram_ref()`.
- `stream_fd()` and `datagram_fd()` continue to return borrowed descriptors.
  A temporary reference inside either getter would end before the caller uses
  the descriptor and would therefore provide false safety. Their documentation
  will state that the owner must remain alive while the descriptor is used.
- Release functions remain the ownership-drop operations and do not acquire an
  additional reference.

## Audited Functions

The implementation will update these currently unprotected operations:

- Stream: `stream_interrupt`, both deadline setters, both address queries, both
  shutdown operations, `listener_interrupt`, and `listener_addr`.
- Datagram: `datagram_interrupt`, both deadline setters, both address queries,
  and the pre-reference object access in `datagram_try_send` and
  `datagram_send`.

## Verification

Add focused tests that exercise deadline and metadata operations racing with
close where the existing test infrastructure can reproduce the lifetime
boundary. Run the stream, TCP, UDS, UDP, TLS, and DTLS test targets, followed by
the project's normal build/test command.
