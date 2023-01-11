#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <assert.h>

#define MAX_ABSPATH_LEN 250

struct task_node {
    char *abspath;
    struct task_node *next;
};

struct task_queue {
    struct task_node *head;
    struct task_node *tail;
} task_queue;

void init_queue(struct task_queue *tq) {
    tq->head = NULL;
    tq->tail = NULL;
}

void enqueue(struct task_queue *tq, const char *path) {
    assert(tq != NULL);
    assert(path != NULL);

    char *abspath = (char *) malloc(MAX_ABSPATH_LEN * sizeof(char));
    if (path[0] == '/') {
        strncpy(abspath, path, MAX_ABSPATH_LEN);
    } else {
        getcwd(abspath, MAX_ABSPATH_LEN);
        strncat(abspath, "/", 2);
        strncat(abspath, path, MAX_ABSPATH_LEN);
    }

    struct task_node *new = (struct task_node *) malloc(sizeof(struct task_node));
    new->abspath = abspath;
    new->next = NULL;

    if (tq->tail == NULL) {
        tq->head = new;
        tq->tail = new;
    } else {
        tq->tail->next = new;
        tq->tail = new;
    }
}

int dequeue(struct task_queue *tq, char *buf) {
    assert(tq != NULL);
    assert(buf != NULL);

    if (tq->head == NULL) {
        return -1;
    }
    assert(tq->head->abspath != NULL);

    strncpy(buf, tq->head->abspath, MAX_ABSPATH_LEN);
    if (tq->head == tq->tail) {
        tq->head = NULL;
        tq->tail = NULL;
    } else {
        tq->head = tq->head->next;
    }
}

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    init_queue(&task_queue);

    const char *grep_bin = "grep";
    const int N = strtol(argv[1], NULL, 10);
    const char *rootpath = argv[2];
    const char *searchstr = argv[3];

    printf("grep_bin: %s\n", grep_bin);
    printf("N: %d\n", N);
    printf("rootpath: %s\n", rootpath);
    printf("searchstr: %s\n", searchstr);
    printf("\n");

    enqueue(&task_queue, rootpath);
    char abspath[MAX_ABSPATH_LEN];
    dequeue(&task_queue, abspath);

    // construct command: grep > /dev/null "searchstr" "rootpath"
    char buf[1024];
    strncpy(buf, grep_bin, 5);
    strncat(buf, " \"", 3);
    // strncat(buf, " > /dev/null \"", 15);
    strncat(buf, searchstr, MAX_ABSPATH_LEN);
    strncat(buf, "\" \"", 4);
    strncat(buf, abspath, MAX_ABSPATH_LEN);
    strncat(buf, "\"", 2);

    printf("Command: %s\n", buf);

    return system(buf);
}
