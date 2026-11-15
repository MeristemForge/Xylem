# Buffered Reader/Writer

Reader and Writer are optional byte-stream helpers over TCP, TLS, UDS, and
RUDP streams. Applications can instead call the transport APIs directly.
WebSocket manages its own framing and buffers and does not use these helpers.

## Ownership and use

The public headers are `include/xylem/net/xylem-reader.h` and
`include/xylem/net/xylem-writer.h`; implementations are in `src/net/`.
Create a wrapper with the connection handle and matching transport enum.
A non-positive buffer size selects the default 4096-byte buffer.
TLS requires `XYLEM_ENABLE_TLS=ON` and OpenSSL >= 3.5.

These APIs run on Xylem coroutines. Each reader or writer has one logical
owner; concurrent calls on the same wrapper are unsupported. A reader and
writer can share a connection under the transport's one-reader/one-writer
contract. Once a reader is buffering input, read through that wrapper:
direct transport reads would skip its unread buffered bytes. Similarly,
direct writes can overtake pending Writer data unless it is flushed first.

Wrappers borrow the connection. Destroy them after their operations return,
and before destroying the connection. Reader destruction releases its buffer;
Writer destruction attempts a final flush. Neither destroys the connection.

## Reading

| Operation | Behavior |
|-----------|----------|
| `xylem_reader_read` | Returns available buffered bytes first; otherwise performs one transport read. Returns a byte count, 0 on EOF, or a negative error. |
| `xylem_reader_read_full` | Reads exactly the requested length; returns that length, 0 on early EOF, or a negative error. |
| `xylem_reader_read_until` | Reads through a delimiter, including it in the result. Returns a byte count, 0 on early EOF, or a negative error. |
| `xylem_reader_peek` | Copies bytes without consuming them. Returns 0 on success or -1 if the length exceeds the internal buffer or cannot be read. |

`read_until` returns -1 as soon as the delimiter cannot fit in the output
buffer. For an 8-byte destination, `abcdefg\n` succeeds with 8 bytes;
`abcdefgh` returns -1 without waiting for another byte or EOF. The internal
buffer size does not change this output limit.

`read_full` and `read_until` may consume and copy a prefix before EOF or an
error, but do not report its length. A failed call is not transactional;
retrying with a larger destination does not reconstruct the consumed prefix.
Use `read` and track progress in the application if partial data must be retained.

## Writing

`xylem_writer_write` returns 0 when input has been buffered or written, and
-1 on error. Small writes are batched. Pending data is flushed before another
write would exceed capacity; writes at least as large as the internal buffer
bypass it after pending data is flushed. Filling the buffer exactly does not
by itself trigger a flush. With an 8-byte buffer, two 4-byte writes can both
remain pending.

Call `xylem_writer_flush` at message boundaries and before waiting for a peer's
response. It returns 0 on success or -1 on failure; an empty buffer is a no-op.
Explicitly flush and check the result before destruction if final write errors
must be detected. The void `destroy` function cannot report them. A successful
transport write does not itself acknowledge that the peer application received
or processed the data.

A flush clears the pending buffer even when the transport reports failure.
The transport may already have sent a prefix, so replaying the entire payload
can duplicate data. Discard the writer and connection after a transport write
failure; a later empty flush returning 0 does not establish delivery of the
failed payload.

## Tests

`tests/test-reader.c` covers reads, exact-length reads, delimiter scanning, and
output-capacity boundaries with internal buffers of 1, 4, 8, and 16 bytes.
Boundary cases include EOF, a peer keeping its connection open, and a delimiter
in the last available byte. `tests/test-writer.c` covers batching, explicit and
destructor flushing, large-write bypass, and invalid write arguments.
