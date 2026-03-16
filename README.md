# POSIX Message Queue BULK_PEEK for CRIU

This repository contains a proof-of-concept (PoC) kernel patch and validation test suites for introducing the MQ_IOC_BULK_PEEK ioctl to Linux POSIX message queues.

This work is part of a Google Summer of Code (GSoC) effort for the Checkpoint/Restore In Userspace (CRIU) project.

## Architecture Design

To improve resilience against denial-of-service patterns and low-memory pressure, the kernel implementation applies the following design choices:
1. **Byte-level chunking (pagination):** Uses start_offset and MQ_PEEK_FLAG_HAS_MORE to support incremental reads of large messages across msg_msgseg boundaries.
2. **Hard-capped kernel buffer:** The implementation limits internal kvzalloc allocation to **64 KB**, regardless of user request size, requiring iterative userspace reads.
3. **Strict 8-byte alignment:** Helps avoid layout disclosure concerns and preserves 32/64-bit compatibility.

## Repository Structure
* /kernel_patch/mqueue_bulk_peek.diff - Core kernel modifications, primarily in ipc/mqueue.c and include/uapi/linux/mqueue.h.
* /tests/mqueue_test_chunk.c - Boundary and payload validation test (empty queue, invalid offsets, small buffer handling, end-to-end data verification).
* /tests/mqueue_test_stress.c - SMP concurrency stress test with concurrent send/receive/peek activity.

---

## Test Results (Kselftest Output)

### 1. Boundary Conditions and Payload Validation
Tested with 100 KB and 50 KB messages using a 32 KB userspace buffer.
```text
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

### 2. SMP Concurrency and Stress Test
4 mutator threads (send/receive) and 4 peeker threads (ioctl with 16 KB buffers) running concurrently.

Plaintext
POSIX MQueue Bulk Peek Stress Test
[Stress] Starting 4 mutator threads and 4 peeker threads...
[Stress] Running send/receive/ioctl workload for 10 seconds.
  -> Tick 1: peeks=100303 | sends=19291
  -> Tick 2: peeks=201018 | sends=38285
  -> Tick 3: peeks=302581 | sends=57351
  -> Tick 4: peeks=404266 | sends=75053
  -> Tick 5: peeks=504427 | sends=93639
  -> Tick 6: peeks=608780 | sends=111648
  -> Tick 7: peeks=708831 | sends=130575
  -> Tick 8: peeks=812271 | sends=148764
  -> Tick 9: peeks=912260 | sends=169021
  -> Tick 10: peeks=1007017 | sends=187196

[Stress] Stopping threads and checking for stability...
  Total messages peeked via ioctl: 1007021
  Total messages sent:             187196