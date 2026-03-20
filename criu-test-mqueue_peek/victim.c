#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <mqueue.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

int main() {
    mqd_t mq;
    struct mq_attr attr = { .mq_flags = 0, .mq_maxmsg = 10, .mq_msgsize = 50000, .mq_curmsgs = 0 };
    mq_unlink("/test_queue");
    mq = mq_open("/test_queue", O_CREAT | O_RDWR, 0644, &attr);
    
    if (mq == (mqd_t)-1) { 
        perror("mq_open failed"); 
        return 1; 
    }
    
    printf("[Victim] Creating messages with different priorities and sizes...\n");

    char msg1[] = "Low Priority (1) Small Message";
    if (mq_send(mq, msg1, sizeof(msg1), 1) == 0) {
        printf("[Victim] Sent Msg1 | Prio: 1  | Size: %zu\n", sizeof(msg1));
    }

    char *msg2 = malloc(50000);
    memset(msg2, 'B', 49999);
    msg2[49999] = '\0';
    if (mq_send(mq, msg2, 50000, 5) == 0) {
        printf("[Victim] Sent Msg2 | Prio: 5  | Size: 50000 (Will be chunked!)\n");
    }

    char msg3[] = "High Priority (10) Message";
    if (mq_send(mq, msg3, sizeof(msg3), 10) == 0) {
        printf("[Victim] Sent Msg3 | Prio: 10 | Size: %zu\n", sizeof(msg3));
    }

    char msg4[256];
    memset(msg4, 'D', 255);
    msg4[255] = '\0';
    if (mq_send(mq, msg4, 256, 20) == 0) {
        printf("[Victim] Sent Msg4 | Prio: 20 | Size: 256\n");
    }

    printf("\n[Victim] All messages sent!\n");
    printf("[Victim] The kernel should sort them so CRIU reads them in this order:\n");
    printf("[Victim]   -> Prio 20\n");
    printf("[Victim]   -> Prio 10\n");
    printf("[Victim]   -> Prio 5 (Chunked into ~7 pieces)\n");
    printf("[Victim]   -> Prio 1\n");
    
    printf("\n[Victim] My PID is %d. Dump me!\n", getpid());
    
    while (1) sleep(10);
    return 0;
}