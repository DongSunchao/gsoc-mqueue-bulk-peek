# POSIX Message Queue BULK_PEEK for CRIU

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
criu-test-mqueue_peek/
  0001-images-add-POSIX-mqueue-protobuf-definitions.patch
  0002-criu-add-POSIX-mqueue-fd-checkpoint-restore.patch
  0003-zdtm-add-POSIX-mqueue-C-R-test.patch

kernel_patch/
  0000-cover-letter.patch
  0001-ipc-msgutil-introduce-msg_copy_part_to_kernel-hel.patch
  0002-mqueue-introduce-MQ_IOC_BULK_PEEK-uapi-and-ioctl-.patch
  0003-ipc-mqueue-implement-MQ_IOC_BULK_PEEK-ioctl.patch

diff/
  mqueue_bulk_peek.diff   Single-file combined diff (for reference)

tools/testing/selftests/mqueue/
  mq_chunk_tests.c        Boundary conditions and end-to-end payload validation
  mq_stress_tests.c       SMP concurrency stress test (multi-thread send/receive/peek)
  mq_edge_tests.c         Edge cases: permissions, ABI contract, priority ordering,
                          zero-length messages, out-of-range index, write-only fd
```
## Boot Patched Kernel in QEMU

```bash
qemu-system-x86_64 \
  -kernel /root/linux/arch/x86/boot/bzImage \
  -append "root=/dev/vda console=ttyS0 rw nokaslr" \
  -drive file=bookworm.img,format=raw,if=virtio \
  -nographic \
  -m 2G \
  -smp 2 \
  -net nic,model=virtio \
  -net user,hostfwd=tcp::10022-:22 \
  -virtfs local,path=/root/mycriu/criu,mount_tag=host_criu,security_model=passthrough,id=host_criu
```

## Building Tests (VM Environment)

```bash
cd tools/testing/selftests/mqueue
gcc -static -o mq_chunk_tests  mq_chunk_tests.c  -lpthread -lrt
gcc -static -o mq_stress_tests mq_stress_tests.c -lpthread -lrt
gcc -static -o mq_edge_tests   mq_edge_tests.c   -lpthread -lrt
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

### 4. CRIU Integration Test

Application output (`posix-mqueue.out`):

```text
root@syzkaller:~/mycriu/criu# cat test/zdtm/static/posix-mqueue.out
09:14:49.996:    66: Sending messages to mqueue...
09:14:50.450:    66: Restored! Receiving and verifying messages...
09:14:50.450:    66: Verified message #0: [Prio 20] 'Message B: Priority 20 (Highest)'
09:14:50.450:    66: Verified message #1: [Prio 10] 'Message A: Priority 10 (First in)'
09:14:50.450:    66: Verified message #2: [Prio 10] 'Message C: Priority 10 (Second in)'
09:14:50.451:    66: Verified message #3: [Prio 5] 'Message D: Priority 5 (Lowest)'
09:14:50.451:    66: PASS
```

Dump log excerpt:

```text
(00.153169) Obtaining task auvx ...
(00.155467) Dumping path for -3 fd via self 15 [/root/mycriu/criu/test/zdtm/static]
...
(00.167297) Running post-dump scripts
(00.167430) Unfreezing tasks into 2
(00.167576) Unseizing 66 into 2
(00.168857) Writing stats
(00.170276) Dumping finished successfully
```

Restore log excerpt:

```text
(00.109078) pidfile: Wrote pid 66 to /root/mycriu/criu/test/zdtm/static/posix-mqueue.pid (2 bytes)
(00.109539) net: Unlock network
(00.109910) pie: 66: seccomp: mode 0 on tid 66
(00.110257) 66 was trapped
(00.110547) 66 (native) is going to execute the syscall 202, required is 15
(00.110692) 66 was trapped
(00.110857) 66 was trapped
...
(00.113283) Run late stage hook from criu master for external devices
(00.113405) Running pre-resume scripts
(00.113549) Restore finished successfully. Tasks resumed.
(00.113681) Writing stats
(00.115214) Running post-resume scripts
```

