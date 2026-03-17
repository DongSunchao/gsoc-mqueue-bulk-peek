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
#include <pthread.h>
#include <time.h>

#define MQ_PEEK_FLAG_HAS_MORE  (1 << 0)
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

#define MAX_MSG_SIZE   65536
#define MSG_MIN_SIZE   1000
#define PEEK_BUF_SIZE  8192   /* kernel caps at MAX_PEEK_BUF_SIZE (8K) */
#define MAX_PRIO       32
#define TEST_DURATION  10
#define NUM_MUTATORS   4
#define NUM_PEEKERS    4
#define FILL_BYTE      'X'

static volatile int keep_running = 1;

static volatile long total_peeks;
static volatile long total_sends;
static volatile long ioctl_errors;  /* unexpected ioctl failures */
static volatile long hdr_errors;    /* invalid header fields */
static volatile long data_errors;   /* payload content mismatch */

static void *mutator_thread(void *arg)
{
	mqd_t mq = *(mqd_t *)arg;
	char *buf = malloc(MAX_MSG_SIZE);
	unsigned int seed = time(NULL) ^ (unsigned long)pthread_self();

	memset(buf, FILL_BYTE, MAX_MSG_SIZE);

	while (keep_running) {
		size_t size = (rand_r(&seed) % (MAX_MSG_SIZE - MSG_MIN_SIZE + 1))
			      + MSG_MIN_SIZE;
		unsigned int prio = rand_r(&seed) % MAX_PRIO;

		if (mq_send(mq, buf, size, prio) == 0)
			__atomic_fetch_add(&total_sends, 1, __ATOMIC_RELAXED);

		/* 60% chance to drain, keeping the queue dynamic. */
		if (rand_r(&seed) % 100 < 60)
			mq_receive(mq, buf, MAX_MSG_SIZE, NULL);
	}

	free(buf);
	return NULL;
}

/*
 * Walk the peek buffer returned by the kernel and validate each
 * message header and payload byte. Returns number of bad entries.
 */
static int validate_peek_buffer(const char *buf, size_t buf_limit,
				uint32_t out_count)
{
	const struct mq_peek_msg_hdr *hdr;
	size_t offset = 0;
	int bad = 0;

	for (uint32_t i = 0; i < out_count; i++) {
		if (offset + sizeof(*hdr) > buf_limit) {
			__atomic_fetch_add(&hdr_errors, 1, __ATOMIC_RELAXED);
			return bad + 1;
		}

		hdr = (const struct mq_peek_msg_hdr *)(buf + offset);

		if (hdr->total_msg_len < MSG_MIN_SIZE ||
		    hdr->total_msg_len > MAX_MSG_SIZE) {
			__atomic_fetch_add(&hdr_errors, 1, __ATOMIC_RELAXED);
			bad++;
		}
		if (hdr->chunk_len > hdr->total_msg_len) {
			__atomic_fetch_add(&hdr_errors, 1, __ATOMIC_RELAXED);
			bad++;
		}
		if (hdr->msg_prio >= MAX_PRIO) {
			__atomic_fetch_add(&hdr_errors, 1, __ATOMIC_RELAXED);
			bad++;
		}
		if (hdr->flags & ~(uint32_t)MQ_PEEK_FLAG_HAS_MORE) {
			__atomic_fetch_add(&hdr_errors, 1, __ATOMIC_RELAXED);
			bad++;
		}

		/* Spot-check payload: all bytes should be FILL_BYTE. */
		for (uint32_t j = 0; j < hdr->chunk_len; j++) {
			if (hdr->payload[j] != FILL_BYTE) {
				__atomic_fetch_add(&data_errors, 1,
						   __ATOMIC_RELAXED);
				bad++;
				break;
			}
		}

		size_t entry = sizeof(*hdr) + hdr->chunk_len;
		offset += (entry + 7) & ~(size_t)7; /* 8-byte aligned */
	}

	return bad;
}

static void *peeker_thread(void *arg)
{
	int mq_fd = (int)(intptr_t)arg;
	char *peek_buf = malloc(PEEK_BUF_SIZE);
	struct mq_bulk_peek_args args;

	while (keep_running) {
		memset(&args, 0, sizeof(args));
		args.buf_ptr  = (uint64_t)(uintptr_t)peek_buf;
		args.buf_size = PEEK_BUF_SIZE;
		args.max_count = 50;

		/* Paginate through the entire queue snapshot. */
		while (keep_running) {
			int ret = ioctl(mq_fd, MQ_IOC_BULK_PEEK, &args);

			if (ret != 0) {
				/* EINVAL is expected when queue mutates under us. */
				if (errno != EINVAL)
					__atomic_fetch_add(&ioctl_errors, 1,
							   __ATOMIC_RELAXED);
				break;
			}
			if (args.out_count == 0)
				break;

			validate_peek_buffer(peek_buf, PEEK_BUF_SIZE,
					     args.out_count);
			__atomic_fetch_add(&total_peeks, args.out_count,
					   __ATOMIC_RELAXED);

			args.start_idx    = args.next_idx;
			args.start_offset = args.next_offset;
		}
	}

	free(peek_buf);
	return NULL;
}

int main(void)
{
	printf("POSIX MQueue Bulk Peek Stress Test\n\n");

	struct mq_attr attr = {
		.mq_flags   = O_NONBLOCK,
		.mq_maxmsg  = 50,
		.mq_msgsize = MAX_MSG_SIZE,
	};

	mq_unlink("/test_queue_stress");
	mqd_t mq = mq_open("/test_queue_stress",
			    O_CREAT | O_RDWR | O_NONBLOCK, 0644, &attr);
	if (mq == (mqd_t)-1) {
		perror("mq_open");
		return 1;
	}

	int mq_fd = open("/dev/mqueue/test_queue_stress", O_RDONLY);
	if (mq_fd < 0) {
		perror("open /dev/mqueue");
		mq_close(mq);
		return 1;
	}

	pthread_t thr[NUM_MUTATORS + NUM_PEEKERS];

	printf("[Stress] %d mutators + %d peekers, running %d seconds\n",
	       NUM_MUTATORS, NUM_PEEKERS, TEST_DURATION);

	for (int i = 0; i < NUM_MUTATORS; i++)
		pthread_create(&thr[i], NULL, mutator_thread, &mq);
	for (int i = 0; i < NUM_PEEKERS; i++)
		pthread_create(&thr[NUM_MUTATORS + i], NULL, peeker_thread,
			       (void *)(intptr_t)mq_fd);

	for (int i = 0; i < TEST_DURATION; i++) {
		sleep(1);
		printf("  Tick %2d: peeks=%-10ld sends=%-10ld "
		       "err(ioctl=%ld hdr=%ld data=%ld)\n",
		       i + 1, total_peeks, total_sends,
		       ioctl_errors, hdr_errors, data_errors);
	}

	printf("\nStopping threads...\n");
	keep_running = 0;
	for (int i = 0; i < NUM_MUTATORS + NUM_PEEKERS; i++)
		pthread_join(thr[i], NULL);

	/* --- verdict --- */
	int failed = 0;

	printf("\n--- Results ---\n");
	printf("  peeks:        %ld\n", total_peeks);
	printf("  sends:        %ld\n", total_sends);
	printf("  ioctl errors: %ld\n", ioctl_errors);
	printf("  hdr errors:   %ld\n", hdr_errors);
	printf("  data errors:  %ld\n", data_errors);

	if (total_peeks == 0) {
		printf("  [FAIL] zero peeks -- ioctl not working\n");
		failed = 1;
	}
	if (total_sends == 0) {
		printf("  [FAIL] zero sends\n");
		failed = 1;
	}
	if (ioctl_errors > 0) {
		printf("  [FAIL] unexpected ioctl errors\n");
		failed = 1;
	}
	if (hdr_errors > 0) {
		printf("  [FAIL] invalid peek message headers\n");
		failed = 1;
	}
	if (data_errors > 0) {
		printf("  [FAIL] payload corruption detected\n");
		failed = 1;
	}

	printf("\n  %s\n\n", failed ? "RESULT: FAIL" : "RESULT: PASS");

	close(mq_fd);
	mq_close(mq);
	mq_unlink("/test_queue_stress");
	return failed ? 1 : 0;
}
