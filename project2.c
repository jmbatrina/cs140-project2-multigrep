#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <assert.h>

#define MAX_ABSPATH_LEN 250

char base_cmd[1024];
int base_cmd_len;

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

char *make_abspath(const char * parent_abspath, const char *path) {
    char *abspath = (char *) malloc(MAX_ABSPATH_LEN * sizeof(char));
    if (path[0] == '/') {
        strncpy(abspath, path, MAX_ABSPATH_LEN);
    } else {
        strncpy(abspath, parent_abspath, MAX_ABSPATH_LEN);
        strncat(abspath, "/", 2);
        strncat(abspath, path, MAX_ABSPATH_LEN);
    }

    return abspath;
}

void enqueue(struct task_queue *tq, char *abspath) {
    assert(tq != NULL);
    assert(abspath != NULL);

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

    struct task_node *removed = tq->head;
    if (removed == NULL) {
        return -1;
    }

    assert(removed->abspath != NULL);
    strncpy(buf, removed->abspath, MAX_ABSPATH_LEN);
    free(removed->abspath);

    if (tq->head == tq->tail) {
        tq->head = NULL;
        tq->tail = NULL;
    } else {
        tq->head = tq->head->next;
    }
    free(removed);

    return 0;
}

int is_empty(struct task_queue *tq) {
    return tq->head == NULL;
}

void worker(void *vid) {
    int id = *((int *) vid);
    while (!is_empty(&task_queue)) {
        char curpath[MAX_ABSPATH_LEN];
        char cmd[1024];

        if (dequeue(&task_queue, curpath) == -1)
            continue;

        DIR *curdir = opendir(curpath);
        struct dirent *entry;
        while ((entry = readdir(curdir)) != NULL) {
            char *base_name = strrchr(entry->d_name, '/');
            base_name = base_name ? base_name+1 : entry->d_name;
            if (strcmp(base_name, ".") == 0 || strcmp(base_name, "..") == 0)
                continue;

            char *entry_abspath = make_abspath(curpath, base_name);
            if (entry->d_type == DT_DIR) {
                enqueue(&task_queue, entry_abspath);
                printf("[%d] ENQUEUE %s\n", id, entry_abspath);
            } else {
                strncpy(cmd, base_cmd, base_cmd_len+1);
                strncat(cmd, "\"", 2);
                strncat(cmd, entry_abspath, MAX_ABSPATH_LEN);
                strncat(cmd, "\"", 2);

                if (system(cmd) == 0) {
                    printf("[%d] PRESENT %s\n", id, entry_abspath);
                } else {
                    printf("[%d] ABSENT %s\n", id, entry_abspath);
                }
            }
        }
        closedir(curdir);
    }
}

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    const char *grep_bin = "grep";
    const int N = strtol(argv[1], NULL, 10);
    const char *rootpath = argv[2];
    const char *searchstr = argv[3];

    printf("grep_bin: %s\n", grep_bin);
    printf("N: %d\n", N);
    printf("rootpath: %s\n", rootpath);
    printf("searchstr: %s\n", searchstr);
    printf("\n");

    init_queue(&task_queue);
    char buf[MAX_ABSPATH_LEN];
    enqueue(&task_queue, make_abspath(getcwd(buf, MAX_ABSPATH_LEN), rootpath));

    // construct base command: grep > /dev/null "searchstr"
    strncpy(base_cmd, grep_bin, 5);
    // // strncat(base_cmd, " \"", 3);
    strncat(base_cmd, " > /dev/null \"", 15);
    strncat(base_cmd, searchstr, MAX_ABSPATH_LEN);
    strncat(base_cmd, "\" ", 3);
    base_cmd_len = strlen(base_cmd);

    pthread_t tid[N];
    int id[N];
    for (int i = 0; i < N; ++i) {
        id[i] = i;
        pthread_create(&tid[i], NULL, (void *)worker, &id[i]);
    }
    for (int i = 0; i < N; ++i) {
        pthread_join(tid[i], NULL);
    }

    return 0;
}
