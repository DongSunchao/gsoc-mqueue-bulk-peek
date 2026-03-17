# POSIX Message Queue BULK_PEEK ioctl (CAP_CHECKPOINT_RESTORE gated)

This repository contains a proof-of-concept (PoC) kernel patch and validation
test suites for introducing the `MQ_IOC_BULK_PEEK` ioctl to Linux POSIX message
queues.

This work is part of a Google Summer of Code (GSoC) effort related to
checkpoint/restore in userspace (CRIU).

## Architecture Design

To improve resilience against denial-of-service patterns and low-memory pressure, the kernel implementation applies the following design choices:

1. **Byte-level chunking (pagination):** Uses `start_offset` and `MQ_PEEK_FLAG_HAS_MORE` to support incremental reads of large messages across `msg_msgseg` boundaries.
2. **Hard-capped kernel buffer:** The implementation caps internal `kzalloc` allocation at 8 KB (`MAX_PEEK_BUF_SIZE`), regardless of the user-requested size.
3. **Strict 8-byte alignment:** Each entry in the peek buffer is 8-byte aligned to avoid layout disclosure and preserve 32/64-bit compatibility.
4. **Capability-gated access:** Requires `CAP_CHECKPOINT_RESTORE` or `CAP_SYS_ADMIN` (namespace-aware via `ns_capable`).

## Repository Structure

```
kernel_patch/
  0000-cover-letter.patch
  0001-ipc-msgutil-introduce-msg_copy_part_to_kernel-hel.patch
  0002-mqueue-introduce-MQ_IOC_BULK_PEEK-uapi-and-ioctl-.patch
  0003-ipc-mqueue-implement-MQ_IOC_BULK_PEEK-ioctl.patch

diff/
  mqueue_bulk_peek.diff   Single-file combined diff (for reference)

tests/
  mq_chunk_tests.c        Boundary conditions and end-to-end payload validation
  mq_stress_tests.c       SMP concurrency stress test (multi-thread send/receive/peek)
  mq_edge_tests.c         Edge cases: permissions, ABI contract, priority ordering,
                          zero-length messages, out-of-range index, write-only fd
```

## Building Tests

```bash
cd tests
gcc -Wall -Wextra -Wpedantic -o mq_chunk_tests  mq_chunk_tests.c  -lpthread -lrt
gcc -Wall -Wextra -Wpedantic -o mq_stress_tests mq_stress_tests.c -lpthread -lrt
gcc -Wall -Wextra -Wpedantic -o mq_edge_tests   mq_edge_tests.c   -lpthread -lrt
```

All tests must run as root on a kernel with the patch applied.

## Test Results

### 1. Boundary Conditions and Payload Validation

Tested with 100 KB and 50 KB messages, chunked through an 8 KB kernel buffer.

```
====================================================
  POSIX MQueue Bulk Peek - Validation Test Suite
====================================================

[Test 1] Empty Queue...
  -> PASS
[Test 2] Tiny Buffer (Less than Header Size)...
  -> PASS
[Test 3] Large max_count (999999)...
  -> PASS
[Test 4] Invalid Offset (start_offset == msg_len)...
  -> PASS

[Test 5] End-to-End Payload Validation (100KB & 50KB)...
  -> Verifying Message 0 (100KB)... OK
  -> Verifying Message 1 (50KB)... OK

====================================================
  All tests passed.
====================================================
```

### 2. SMP Concurrency Stress Test

4 mutator threads (send/receive) and 4 peeker threads running concurrently for 10 seconds.

```
--- Results ---
  peeks:        119656
  sends:        191696
  ioctl errors: 0
  hdr errors:   0
  data errors:  0

  RESULT: PASS
```

### 3. Edge Cases and Security

```
====================================================
  POSIX MQueue Bulk Peek - Edge Case Test Suite
====================================================

[Test 1] Unprivileged process gets EPERM
  [PASS] EPERM for unprivileged user

[Test 2] reserved_in != 0 returns EINVAL
  [PASS] EINVAL when reserved_in = 1
  [PASS] EINVAL when reserved_in = 0xFFFFFFFF

[Test 3] max_count = 0 fast path
  [PASS] returns success
  [PASS] out_count == 0
  [PASS] next_idx echoes start_idx (7)
  [PASS] next_offset echoes start_offset (42)

[Test 4] Write-only fd returns EBADF
  [PASS] EBADF on write-only fd

[Test 5] Priority ordering (highest first)
  [PASS] ioctl succeeds
  [PASS] out_count == 4
  [PASS] priority order: 5, 5, 3, 1
  [PASS] FIFO within same priority (H before h)

[Test 6] Zero-length message
  [PASS] ioctl succeeds
  [PASS] out_count == 1
  [PASS] total_msg_len == 0
  [PASS] chunk_len == 0
  [PASS] no HAS_MORE flag

[Test 7] start_idx beyond queue length
  [PASS] ioctl succeeds (not an error)
  [PASS] out_count == 0
  [PASS] start_idx=9999: ioctl succeeds
  [PASS] start_idx=9999: out_count == 0

====================================================
  21 / 21 passed
====================================================
```
