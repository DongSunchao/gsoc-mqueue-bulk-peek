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

#define MQ_PEEK_FLAG_HAS_MORE  (1 << 0)
#define MQ_IOC_BULK_PEEK 0xc0304d01 

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
    uint8_t  payload[0];
};

#define ALIGN8(x) (((x) + 7) & ~7)

void fill_pattern(char *buf, size_t size, char seed) {
    for (size_t i = 0; i < size; i++) buf[i] = seed + (i % 10);
}

int verify_pattern(char *buf, size_t size, char seed) {
    for (size_t i = 0; i < size; i++) {
        if (buf[i] != (char)(seed + (i % 10))) {
                printf("    [FAIL] Payload mismatch at byte %zu: expected %c, got %c\n",
                    i, (char)(seed + (i % 10)), buf[i]);
            return 1;
        }
    }
    return 0;
}

int main() {
    printf("====================================================\n");
    printf("  POSIX MQueue Bulk Peek - Validation Test Suite\n");
    printf("====================================================\n\n");

    mqd_t mq;
    struct mq_attr attr = { .mq_flags = 0, .mq_maxmsg = 10, .mq_msgsize = 120000, .mq_curmsgs = 0 };
    mq_unlink("/test_queue");
    mq = mq_open("/test_queue", O_CREAT | O_RDWR, 0644, &attr);
    if (mq == (mqd_t)-1) { perror("mq_open failed"); return 1; }

    int mq_fd = open("/dev/mqueue/test_queue", O_RDONLY);
    
    struct mq_bulk_peek_args args = {0};
    char *tiny_buf = malloc(16);
    int ret, failed_tests = 0;

    printf("[Test 1] Empty Queue...\n");
    args.buf_ptr = (uint64_t)(uintptr_t)tiny_buf; args.buf_size = 16; args.max_count = 10;
    ret = ioctl(mq_fd, MQ_IOC_BULK_PEEK, &args);
    if (ret == 0 && args.out_count == 0) printf("  -> PASS\n"); else { printf("  -> FAIL\n"); failed_tests++; }

    size_t MSG_0_SIZE = 100000, MSG_1_SIZE = 50000;
    char *src_msg0 = malloc(MSG_0_SIZE), *src_msg1 = malloc(MSG_1_SIZE);
    fill_pattern(src_msg0, MSG_0_SIZE, 'A'); 
    fill_pattern(src_msg1, MSG_1_SIZE, 'a');
    mq_send(mq, src_msg0, MSG_0_SIZE, 2); 
    mq_send(mq, src_msg1, MSG_1_SIZE, 1);

    printf("[Test 2] Tiny Buffer (Less than Header Size)...\n");
    args.buf_size = 8; 
    ret = ioctl(mq_fd, MQ_IOC_BULK_PEEK, &args);
    if (ret == -1 && errno == EINVAL) printf("  -> PASS\n"); else { printf("  -> FAIL\n"); failed_tests++; }

    printf("[Test 3] Large max_count (999999)...\n");
    char *normal_buf = malloc(16384);
    args.buf_ptr = (uint64_t)(uintptr_t)normal_buf; args.buf_size = 16384; args.max_count = 999999; 
    args.start_idx = 0; args.start_offset = 0;
    ret = ioctl(mq_fd, MQ_IOC_BULK_PEEK, &args);
    if (ret == 0 && args.out_count > 0) printf("  -> PASS\n"); else { printf("  -> FAIL\n"); failed_tests++; }

    printf("[Test 4] Invalid Offset (start_offset == msg_len)...\n");
    args.start_idx = 0; args.start_offset = MSG_0_SIZE; args.max_count = 10;
    ret = ioctl(mq_fd, MQ_IOC_BULK_PEEK, &args);
    if (ret == -1 && errno == EINVAL) printf("  -> PASS\n"); else { printf("  -> FAIL\n"); failed_tests++; }

    printf("\n[Test 5] End-to-End Payload Validation (100KB & 50KB)...\n");
    char *reassembled_0 = malloc(MSG_0_SIZE); memset(reassembled_0, 0, MSG_0_SIZE);
    char *reassembled_1 = malloc(MSG_1_SIZE); memset(reassembled_1, 0, MSG_1_SIZE);

    char *test5_buf = malloc(32768);
    args.buf_ptr = (uint64_t)(uintptr_t)test5_buf;
    args.buf_size = 32768; 
    args.start_idx = 0; args.start_offset = 0; args.max_count = 10;
    
    while (1) {
        ret = ioctl(mq_fd, MQ_IOC_BULK_PEEK, &args);
        if (ret != 0 || args.out_count == 0) break;

        size_t offset = 0;
        for (uint32_t i = 0; i < args.out_count; i++) {
            struct mq_peek_msg_hdr *hdr = (struct mq_peek_msg_hdr *)(test5_buf + offset);
            
            size_t write_offset = (i == 0) ? args.start_offset : 0;

            if (args.start_idx + i == 0) {
                memcpy(reassembled_0 + write_offset, hdr->payload, hdr->chunk_len);
            } else if (args.start_idx + i == 1) {
                memcpy(reassembled_1 + write_offset, hdr->payload, hdr->chunk_len);
            }
            offset += ALIGN8(sizeof(struct mq_peek_msg_hdr) + hdr->chunk_len);
        }
        args.start_idx = args.next_idx;
        args.start_offset = args.next_offset;
    }

    printf("  -> Verifying Message 0 (100KB)... ");
    if (verify_pattern(reassembled_0, MSG_0_SIZE, 'A') == 0) printf("OK\n"); else failed_tests++;
    
    printf("  -> Verifying Message 1 (50KB)... ");
    if (verify_pattern(reassembled_1, MSG_1_SIZE, 'a') == 0) printf("OK\n"); else failed_tests++;

    printf("\n====================================================\n");
    if (failed_tests == 0) printf("  All tests passed.\n");
    else printf("  %d tests failed.\n", failed_tests);
    printf("====================================================\n");

    close(mq_fd); return 0;
}