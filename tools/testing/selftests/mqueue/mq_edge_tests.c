// SPDX-License-Identifier: GPL-2.0

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <mqueue.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <errno.h>
#include <sys/wait.h>

#ifndef BIT
#define BIT(nr) (1UL << (nr))
#endif

#define MQ_PEEK_FLAG_HAS_MORE  BIT(0)
#define MQ_IOC_BULK_PEEK _IOWR('M', 1, struct mq_bulk_peek_args)

struct mq_bulk_peek_args {
	uint32_t start_idx;
	uint32_t start_offset;
	uint32_t max_count;
	uint32_t reserved_in;
	uint64_t buf_ptr;
	uint64_t buf_size;
	uint32_t out_count;
	uint32_t next_idx;
	uint32_t next_offset;
	uint32_t reserved_out;
};

struct mq_peek_msg_hdr {
	uint32_t total_msg_len;
	uint32_t chunk_len;
	uint32_t msg_prio;
	uint32_t flags;
	uint8_t  payload[];
};

#define ALIGN8(x) (((x) + 7) & ~(size_t)7)

#define QUEUE_NAME "/test_edge"
#define BUF_SIZE   4096
#define MSG_MAX    1024

static int failed;
static int total;

static void check(const char *name, int cond)
{
	total++;
	if (cond) {
		printf("  [PASS] %s\n", name);
	} else {
		printf("  [FAIL] %s\n", name);
		failed++;
	}
}

static int do_peek(int fd, struct mq_bulk_peek_args *args, char *buf)
{
	args->buf_ptr = (uint64_t)(uintptr_t)buf;
	return ioctl(fd, MQ_IOC_BULK_PEEK, args);
}

/*
 * Fork a child that drops all capabilities and attempts the ioctl.
 * The child exits with 0 if ioctl returns -1 && errno == EPERM.
 */
static void test_eperm(int mq_fd)
{
	printf("\n[Test 1] Unprivileged process gets EPERM\n");

	pid_t pid = fork();

	if (pid == 0) {
		/* Drop all privileges: setuid to nobody (65534) */
		if (setuid(65534) != 0) {
			perror("setuid");
			_exit(2);
		}

		char buf[BUF_SIZE];
		struct mq_bulk_peek_args args = {0};

		args.buf_ptr  = (uint64_t)(uintptr_t)buf;
		args.buf_size = BUF_SIZE;
		args.max_count = 10;

		int ret = ioctl(mq_fd, MQ_IOC_BULK_PEEK, &args);

		_exit((ret == -1 && errno == EPERM) ? 0 : 1);
	}

	int status;

	waitpid(pid, &status, 0);
	check("EPERM for unprivileged user",
	      WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_reserved_in(int mq_fd)
{
	printf("\n[Test 2] reserved_in != 0 returns EINVAL\n");

	char buf[BUF_SIZE];
	struct mq_bulk_peek_args args = {0};

	args.buf_size  = BUF_SIZE;
	args.max_count = 10;
	args.reserved_in = 1;

	int ret = do_peek(mq_fd, &args, buf);

	check("EINVAL when reserved_in = 1",
	      ret == -1 && errno == EINVAL);

	args.reserved_in = 0xFFFFFFFF;
	ret = do_peek(mq_fd, &args, buf);
	check("EINVAL when reserved_in = 0xFFFFFFFF",
	      ret == -1 && errno == EINVAL);
}

static void test_max_count_zero(int mq_fd)
{
	printf("\n[Test 3] max_count = 0 fast path\n");

	char buf[BUF_SIZE];
	struct mq_bulk_peek_args args = {0};

	args.buf_size  = BUF_SIZE;
	args.max_count = 0;
	args.start_idx = 7;
	args.start_offset = 42;

	int ret = do_peek(mq_fd, &args, buf);

	check("returns success", ret == 0);
	check("out_count == 0", args.out_count == 0);
	check("next_idx echoes start_idx (7)", args.next_idx == 7);
	check("next_offset echoes start_offset (42)", args.next_offset == 42);
}

static void test_write_only_fd(void)
{
	printf("\n[Test 4] Write-only fd returns EBADF\n");

	mqd_t mq_wo = mq_open(QUEUE_NAME, O_WRONLY);

	if (mq_wo == (mqd_t)-1) {
		printf("  [SKIP] cannot open write-only mq\n");
		return;
	}

	/*
	 * mq_open returns a file descriptor directly usable with ioctl
	 * on some implementations. To be safe, also try via /dev/mqueue.
	 */
	int wo_fd = open("/dev/mqueue" QUEUE_NAME, O_WRONLY);

	if (wo_fd < 0) {
		printf("  [SKIP] cannot open /dev/mqueue write-only\n");
		mq_close(mq_wo);
		return;
	}

	char buf[BUF_SIZE];
	struct mq_bulk_peek_args args = {0};

	args.buf_size  = BUF_SIZE;
	args.max_count = 10;

	int ret = do_peek(wo_fd, &args, buf);

	check("EBADF on write-only fd",
	      ret == -1 && errno == EBADF);

	close(wo_fd);
	mq_close(mq_wo);
}

static void test_priority_ordering(int mq_fd, mqd_t mq)
{
	printf("\n[Test 5] Priority ordering (highest first)\n");

	/* Send 4 messages with distinct priorities and payloads */
	const struct {
		unsigned int prio;
		char marker;
	} msgs[] = {
		{ 1, 'L' },  /* low */
		{ 5, 'H' },  /* high */
		{ 3, 'M' },  /* medium */
		{ 5, 'h' },  /* high, FIFO second */
	};

	for (int i = 0; i < 4; i++) {
		char payload[32];

		memset(payload, msgs[i].marker, sizeof(payload));
		if (mq_send(mq, payload, sizeof(payload), msgs[i].prio) != 0) {
			printf("  [SKIP] mq_send failed: %s\n", strerror(errno));
			return;
		}
	}

	/* Expected peek order: prio 5 ('H'), prio 5 ('h'), prio 3 ('M'), prio 1 ('L') */
	char buf[BUF_SIZE];
	struct mq_bulk_peek_args args = {0};

	args.buf_size  = BUF_SIZE;
	args.max_count = 10;

	int ret = do_peek(mq_fd, &args, buf);

	check("ioctl succeeds", ret == 0);
	check("out_count == 4", args.out_count == 4);

	if (ret != 0 || args.out_count != 4) {
		char drain[MSG_MAX];

		while (mq_receive(mq, drain, sizeof(drain), NULL) > 0)
			;
		return;
	}

	const unsigned int expect_prio[] = { 5, 5, 3, 1 };
	const char expect_marker[] = { 'H', 'h', 'M', 'L' };

	size_t offset = 0;
	int order_ok = 1;

	for (int i = 0; i < 4; i++) {
		struct mq_peek_msg_hdr *hdr =
			(struct mq_peek_msg_hdr *)(buf + offset);

		if (hdr->msg_prio != expect_prio[i]) {
			printf("  [FAIL] msg[%d] prio: expected %u, got %u\n",
			       i, expect_prio[i], hdr->msg_prio);
			order_ok = 0;
		}
		if (hdr->chunk_len > 0 && hdr->payload[0] != (uint8_t)expect_marker[i]) {
			printf("  [FAIL] msg[%d] marker: expected '%c', got '%c'\n",
			       i, expect_marker[i], (char)hdr->payload[0]);
			order_ok = 0;
		}

		size_t entry = ALIGN8(sizeof(*hdr) + hdr->chunk_len);

		offset += entry;
	}

	check("priority order: 5, 5, 3, 1", order_ok);
	check("FIFO within same priority (H before h)",
	      order_ok);

	/* Drain the queue for subsequent tests */
	char drain[MSG_MAX];

	while (mq_receive(mq, drain, sizeof(drain), NULL) > 0)
		;
}

static void test_zero_length_message(int mq_fd, mqd_t mq)
{
	printf("\n[Test 6] Zero-length message\n");

	if (mq_send(mq, "", 0, 1) != 0) {
		printf("  [SKIP] mq_send(size=0) failed: %s\n", strerror(errno));
		return;
	}

	char buf[BUF_SIZE];
	struct mq_bulk_peek_args args = {0};

	args.buf_size  = BUF_SIZE;
	args.max_count = 10;

	int ret = do_peek(mq_fd, &args, buf);

	check("ioctl succeeds", ret == 0);
	check("out_count == 1", args.out_count == 1);

	if (ret == 0 && args.out_count == 1) {
		struct mq_peek_msg_hdr *hdr = (struct mq_peek_msg_hdr *)buf;

		check("total_msg_len == 0", hdr->total_msg_len == 0);
		check("chunk_len == 0", hdr->chunk_len == 0);
		check("no HAS_MORE flag", !(hdr->flags & MQ_PEEK_FLAG_HAS_MORE));
	}

	char drain[MSG_MAX];

	while (mq_receive(mq, drain, sizeof(drain), NULL) >= 0)
		;
}

static void test_start_idx_out_of_range(int mq_fd, mqd_t mq)
{
	printf("\n[Test 7] start_idx beyond queue length\n");

	char msg[] = "hello";

	mq_send(mq, msg, sizeof(msg), 1);
	mq_send(mq, msg, sizeof(msg), 2);

	char buf[BUF_SIZE];
	struct mq_bulk_peek_args args = {0};

	args.buf_size  = BUF_SIZE;
	args.max_count = 10;

	/* Queue has 2 messages (idx 0, 1). Request start_idx = 2. */
	args.start_idx = 2;
	int ret = do_peek(mq_fd, &args, buf);

	check("ioctl succeeds (not an error)", ret == 0);
	check("out_count == 0", args.out_count == 0);

	/* Way beyond: start_idx = 9999 */
	args.start_idx = 9999;
	args.out_count = 42;
	ret = do_peek(mq_fd, &args, buf);
	check("start_idx=9999: ioctl succeeds", ret == 0);
	check("start_idx=9999: out_count == 0", args.out_count == 0);

	/* Drain */
	char drain[MSG_MAX];

	while (mq_receive(mq, drain, sizeof(drain), NULL) > 0)
		;
}

int main(void)
{
	printf("====================================================\n");
	printf("  POSIX MQueue Bulk Peek - Edge Case Test Suite\n");
	printf("====================================================\n");

	if (geteuid() != 0) {
		printf("\n[SKIP] this test requires root (ioctl gated by capabilities)\n");
		return 0;
	}

	struct mq_attr attr = {
		.mq_flags   = O_NONBLOCK,
		.mq_maxmsg  = 10,
		.mq_msgsize = 1024,
	};

	mq_unlink(QUEUE_NAME);
	mqd_t mq = mq_open(QUEUE_NAME, O_CREAT | O_RDWR | O_NONBLOCK,
			    0644, &attr);
	if (mq == (mqd_t)-1) {
		perror("mq_open");
		return 1;
	}

	int mq_fd = open("/dev/mqueue" QUEUE_NAME, O_RDONLY);

	if (mq_fd < 0) {
		perror("open /dev/mqueue");
		mq_close(mq);
		return 1;
	}

	test_eperm(mq_fd);
	test_reserved_in(mq_fd);
	test_max_count_zero(mq_fd);
	test_write_only_fd();
	test_priority_ordering(mq_fd, mq);
	test_zero_length_message(mq_fd, mq);
	test_start_idx_out_of_range(mq_fd, mq);

	printf("\n====================================================\n");
	printf("  %d / %d passed", total - failed, total);
	if (failed)
		printf("  (%d FAILED)", failed);
	printf("\n====================================================\n");

	close(mq_fd);
	mq_close(mq);
	mq_unlink(QUEUE_NAME);
	return failed ? 1 : 0;
}
