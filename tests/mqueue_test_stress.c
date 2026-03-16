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

#define MAX_MSG_SIZE 65536 
volatile int keep_running = 1;
long total_peeks = 0;
long total_sends = 0;

void *mutator_thread(void *arg) {
    mqd_t mq = *(mqd_t *)arg;
    char *buf = malloc(MAX_MSG_SIZE);
    memset(buf, 'X', MAX_MSG_SIZE);
    
    unsigned int seed = time(NULL) ^ pthread_self();
    
    while (keep_running) {

        size_t size = (rand_r(&seed) % 64536) + 1000; 
        unsigned int prio = rand_r(&seed) % 32;
        
        if (mq_send(mq, buf, size, prio) == 0) {
            __atomic_fetch_add(&total_sends, 1, __ATOMIC_RELAXED);
        }

        //  60% probability to receive a message, causing red-black tree to rebalance
        if (rand_r(&seed) % 100 < 60) { 
            mq_receive(mq, buf, MAX_MSG_SIZE, NULL);
        }
    }
    free(buf);
    return NULL;
}

void *peeker_thread(void *arg) {
    int mq_fd = *(int *)arg;
    char *peek_buf = malloc(16384);
    
    struct mq_bulk_peek_args args;
    
    while (keep_running) {
        memset(&args, 0, sizeof(args));
        args.buf_ptr = (uint64_t)(uintptr_t)peek_buf;
        args.buf_size = 16384; 
        args.max_count = 50;
        args.start_idx = 0;
        args.start_offset = 0;
        
        while (keep_running) {
            int ret = ioctl(mq_fd, MQ_IOC_BULK_PEEK, &args);
            
            if (ret != 0 || args.out_count == 0) break;
            
            __atomic_fetch_add(&total_peeks, args.out_count, __ATOMIC_RELAXED);

            args.start_idx = args.next_idx;
            args.start_offset = args.next_offset;
        }
    }
    free(peek_buf);
    return NULL;
}

int main() {
    printf("POSIX MQueue Bulk Peek Stress Test\n");

    struct mq_attr attr = { .mq_flags = O_NONBLOCK, .mq_maxmsg = 50, .mq_msgsize = MAX_MSG_SIZE, .mq_curmsgs = 0 };
    mq_unlink("/test_queue_stress");
    mqd_t mq_rw = mq_open("/test_queue_stress", O_CREAT | O_RDWR | O_NONBLOCK, 0644, &attr);
    if (mq_rw == (mqd_t)-1) { perror("mq_open failed"); return 1; }

    int mq_ioctl = open("/dev/mqueue/test_queue_stress", O_RDONLY);

    int num_mutators = 4;
    int num_peekers = 4; 
    pthread_t threads[20];

    printf("[Stress] Starting %d mutator threads and %d peeker threads...\n", num_mutators, num_peekers);
    
    for (int i = 0; i < num_mutators; i++) pthread_create(&threads[i], NULL, mutator_thread, &mq_rw);
    for (int i = 0; i < num_peekers; i++) pthread_create(&threads[num_mutators + i], NULL, peeker_thread, &mq_ioctl);

    printf("[Stress] Running send/receive/ioctl workload for 10 seconds.\n");
    for (int i = 0; i < 10; i++) {
        sleep(1);
        printf("  -> Tick %d: peeks=%ld | sends=%ld\n", i + 1, total_peeks, total_sends);
    }

    keep_running = 0;
    printf("\n[Stress] Stopping threads and checking for stability...\n");

    for (int i = 0; i < num_mutators + num_peekers; i++) pthread_join(threads[i], NULL);

    printf("  Total messages peeked via ioctl: %ld\n", total_peeks);
    printf("  Total messages sent:             %ld\n", total_sends);

    close(mq_ioctl);
    mq_close(mq_rw);
    return 0;
}