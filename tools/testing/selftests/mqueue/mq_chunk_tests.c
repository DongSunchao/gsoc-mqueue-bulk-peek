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

#define QUEUE_NAME "/test_queue_chunk"
#define PEEK_BUF_SIZE 8192 /* kernel caps at 8K */

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

static void fill_pattern(char *buf, size_t size, char seed)
{
	size_t i;

	for (i = 0; i < size; i++)
		buf[i] = (char)(seed + (i % 10));
}

static int verify_pattern(const char *buf, size_t size, char seed)
{
	size_t i;

	for (i = 0; i < size; i++) {
		char want = (char)(seed + (i % 10));

		if (buf[i] != want) {
			printf("    [FAIL] payload mismatch at byte %zu: expected %c, got %c\n",
			       i, want, buf[i]);
			return -1;
		}
	}
	return 0;
}

static int parse_one_call(const char *buf, size_t buf_limit,
			  const struct mq_bulk_peek_args *args,
			  uint32_t *curr_idx, size_t *curr_msg_offset,
			  char *reassembled_0, size_t msg0_size,
			  char *reassembled_1, size_t msg1_size)
{
	uint32_t i;
	size_t offset = 0;

	for (i = 0; i < args->out_count; i++) {
		const struct mq_peek_msg_hdr *hdr;
		size_t raw_len, aligned_len;

		if (offset + sizeof(*hdr) > buf_limit) {
			printf("    [FAIL] entry header exceeds buffer\n");
			return -1;
		}

		hdr = (const struct mq_peek_msg_hdr *)(buf + offset);
		raw_len = sizeof(*hdr) + hdr->chunk_len;
		aligned_len = ALIGN8(raw_len);

		if (hdr->chunk_len > hdr->total_msg_len) {
			printf("    [FAIL] invalid chunk_len > total_msg_len\n");
			return -1;
		}
		if (offset + aligned_len > buf_limit) {
			printf("    [FAIL] entry exceeds buffer\n");
			return -1;
		}

		if (*curr_idx == 0) {
			if (*curr_msg_offset + hdr->chunk_len > msg0_size) {
				printf("    [FAIL] msg0 write exceeds destination\n");
				return -1;
			}
			memcpy(reassembled_0 + *curr_msg_offset, hdr->payload, hdr->chunk_len);
		} else if (*curr_idx == 1) {
			if (*curr_msg_offset + hdr->chunk_len > msg1_size) {
				printf("    [FAIL] msg1 write exceeds destination\n");
				return -1;
			}
			memcpy(reassembled_1 + *curr_msg_offset, hdr->payload, hdr->chunk_len);
		} else {
			printf("    [FAIL] unexpected message index %u\n", *curr_idx);
			return -1;
		}

		if (hdr->flags & MQ_PEEK_FLAG_HAS_MORE) {
			*curr_msg_offset += hdr->chunk_len;
		} else {
			(*curr_idx)++;
			*curr_msg_offset = 0;
		}

		offset += aligned_len;
	}

	return 0;
}

int main(void)
{
	int ret;
	int mq_fd = -1;
	mqd_t mq = (mqd_t)-1;
	char *tiny_buf = NULL;
	char *peek_buf = NULL;
	char *src_msg0 = NULL;
	char *src_msg1 = NULL;
	char *reassembled_0 = NULL;
	char *reassembled_1 = NULL;

	const size_t msg0_size = 100000;
	const size_t msg1_size = 50000;

	printf("====================================================\n");
	printf("  POSIX MQueue Bulk Peek - Validation Test Suite\n");
	printf("====================================================\n");

	if (geteuid() != 0) {
		printf("\n[SKIP] this test requires root (ioctl gated by capabilities)\n");
		return 0;
	}

	struct mq_attr attr = {
		.mq_flags = 0,
		.mq_maxmsg = 10,
		.mq_msgsize = 120000,
	};

	mq_unlink(QUEUE_NAME);
	mq = mq_open(QUEUE_NAME, O_CREAT | O_RDWR, 0644, &attr);
	if (mq == (mqd_t)-1) {
		perror("mq_open");
		return 1;
	}

	mq_fd = open("/dev/mqueue" QUEUE_NAME, O_RDONLY);
	if (mq_fd < 0) {
		perror("open /dev/mqueue");
		ret = 1;
		goto out;
	}

	tiny_buf = malloc(16);
	peek_buf = malloc(PEEK_BUF_SIZE);
	src_msg0 = malloc(msg0_size);
	src_msg1 = malloc(msg1_size);
	reassembled_0 = malloc(msg0_size);
	reassembled_1 = malloc(msg1_size);
	if (!tiny_buf || !peek_buf || !src_msg0 || !src_msg1 ||
	    !reassembled_0 || !reassembled_1) {
		perror("malloc");
		ret = 1;
		goto out;
	}

	memset(reassembled_0, 0, msg0_size);
	memset(reassembled_1, 0, msg1_size);

	printf("\n[Test 1] Empty Queue\n");
	{
		struct mq_bulk_peek_args args = {0};

		args.buf_ptr = (uint64_t)(uintptr_t)tiny_buf;
		args.buf_size = 16;
		args.max_count = 10;
		ret = ioctl(mq_fd, MQ_IOC_BULK_PEEK, &args);
		check("ioctl succeeds", ret == 0);
		check("out_count == 0", args.out_count == 0);
	}

	fill_pattern(src_msg0, msg0_size, 'A');
	fill_pattern(src_msg1, msg1_size, 'a');
	if (mq_send(mq, src_msg0, msg0_size, 2) != 0) {
		perror("mq_send msg0");
		ret = 1;
		goto out;
	}
	if (mq_send(mq, src_msg1, msg1_size, 1) != 0) {
		perror("mq_send msg1");
		ret = 1;
		goto out;
	}

	printf("\n[Test 2] Tiny Buffer (Less than Header Size)\n");
	{
		struct mq_bulk_peek_args args = {0};

		args.buf_ptr = (uint64_t)(uintptr_t)tiny_buf;
		args.buf_size = 8;
		args.max_count = 10;
		ret = ioctl(mq_fd, MQ_IOC_BULK_PEEK, &args);
		check("EINVAL", ret == -1 && errno == EINVAL);
	}

	printf("\n[Test 3] Large max_count (999999)\n");
	{
		struct mq_bulk_peek_args args = {0};

		args.buf_ptr = (uint64_t)(uintptr_t)peek_buf;
		args.buf_size = PEEK_BUF_SIZE;
		args.max_count = 999999;
		ret = ioctl(mq_fd, MQ_IOC_BULK_PEEK, &args);
		check("ioctl succeeds", ret == 0);
		check("out_count > 0", args.out_count > 0);
	}

	printf("\n[Test 4] Invalid Offset (start_offset == msg_len)\n");
	{
		struct mq_bulk_peek_args args = {0};

		args.buf_ptr = (uint64_t)(uintptr_t)peek_buf;
		args.buf_size = PEEK_BUF_SIZE;
		args.max_count = 10;
		args.start_idx = 0;
		args.start_offset = msg0_size;
		ret = ioctl(mq_fd, MQ_IOC_BULK_PEEK, &args);
		check("EINVAL", ret == -1 && errno == EINVAL);
	}

	printf("\n[Test 5] End-to-End Payload Validation (100KB & 50KB)\n");
	{
		struct mq_bulk_peek_args args = {0};
		uint32_t curr_idx;
		size_t curr_msg_offset;

		args.buf_ptr = (uint64_t)(uintptr_t)peek_buf;
		args.buf_size = PEEK_BUF_SIZE;
		args.max_count = 10;
		args.start_idx = 0;
		args.start_offset = 0;

		while (1) {
			ret = ioctl(mq_fd, MQ_IOC_BULK_PEEK, &args);
			if (ret != 0) {
				printf("    [FAIL] ioctl failed: %s\n", strerror(errno));
				failed++;
				break;
			}
			if (args.out_count == 0)
				break;

			curr_idx = args.start_idx;
			curr_msg_offset = args.start_offset;
			if (parse_one_call(peek_buf, PEEK_BUF_SIZE, &args,
					   &curr_idx, &curr_msg_offset,
					   reassembled_0, msg0_size,
					   reassembled_1, msg1_size) != 0) {
				failed++;
				break;
			}

			args.start_idx = args.next_idx;
			args.start_offset = args.next_offset;
		}

		check("verify msg0", verify_pattern(reassembled_0, msg0_size, 'A') == 0);
		check("verify msg1", verify_pattern(reassembled_1, msg1_size, 'a') == 0);
	}

	printf("\n====================================================\n");
	printf("  %d / %d passed", total - failed, total);
	if (failed)
		printf("  (%d FAILED)", failed);
	printf("\n====================================================\n");
	ret = failed ? 1 : 0;

out:
	if (mq_fd >= 0)
		close(mq_fd);
	if (mq != (mqd_t)-1)
		mq_close(mq);
	mq_unlink(QUEUE_NAME);

	free(tiny_buf);
	free(peek_buf);
	free(src_msg0);
	free(src_msg1);
	free(reassembled_0);
	free(reassembled_1);
	return ret;
}
