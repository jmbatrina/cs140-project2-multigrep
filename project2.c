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
int is_done;
int N;

struct task_node {
    char *abspath;
    struct task_node *next;
};

struct task_queue {
    pthread_mutex_t lock;
    int numIdleWorkers;
    struct task_node *head;
    struct task_node *tail;
} task_queue;

void init_queue(struct task_queue *tq) {
    pthread_mutex_init(&tq->lock, NULL);
    tq->numIdleWorkers = 0;
    tq->head = NULL;
    tq->tail = NULL;
}

char *escape_special_chars(const char *string, char *buf) {
    int len = strlen(string);
    char *start = (char *)string;
    char *end;
    strncpy(buf, "'", 2);
    while ((end = strchr(start, '\'')) != NULL) {
        strncat(buf, start, end-start);
        strncat(buf, "'\"'\"'", 6);
        start = end+1;
    }
    if (start != NULL) {
        strncat(buf, start, (string+len)-start+1);
    }
    strncat(buf, "'", 2);

    return buf;
}

char *make_abspath(const char *parent_abspath, char *path) {
    char *lastChar = path + strlen(path)-1;
    if (*lastChar == '/') {
        *lastChar = '\0';
    }

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
    if (new == NULL) {
        fprintf(stderr, "Enqueue: malloc() failed\n");
        return;
    }

    pthread_mutex_lock(&tq->lock);
    new->abspath = abspath;
    new->next = NULL;

    if (tq->tail == NULL) {
        tq->head = new;
        tq->tail = new;
    } else {
        tq->tail->next = new;
        tq->tail = new;
    }
    pthread_mutex_unlock(&tq->lock);
}

int dequeue(struct task_queue *tq, char *buf) {
    assert(tq != NULL);
    assert(buf != NULL);

    int retval = 0;
    pthread_mutex_lock(&tq->lock);
    struct task_node *removed = tq->head;
    if (removed == NULL) {
        retval = -1;
    } else {
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
    }
    pthread_mutex_unlock(&tq->lock);

    return retval;
}

int is_empty(struct task_queue *tq) {
    return tq->head == NULL;
}

void incrementIdle(struct task_queue *tq) {
    pthread_mutex_lock(&tq->lock);
    ++tq->numIdleWorkers;
    pthread_mutex_unlock(&tq->lock);
}

void decrementIdle(struct task_queue *tq) {
    pthread_mutex_lock(&tq->lock);
    --tq->numIdleWorkers;
    pthread_mutex_unlock(&tq->lock);
}

int grepNextDir(int id) {
    char curpath[MAX_ABSPATH_LEN];
    char cmd[1024];

    if (is_empty(&task_queue) || dequeue(&task_queue, curpath) == -1)
        return 0;

    int didWork = 0;
    printf("[%d] DIR %s\n", id, curpath);
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
            didWork = 1;
        } else {
            char buf[MAX_ABSPATH_LEN];
            escape_special_chars(entry_abspath, buf);
            strncpy(cmd, base_cmd, base_cmd_len+1);
            strncat(cmd, buf, MAX_ABSPATH_LEN);

            if (system(cmd) == 0) {
                printf("[%d] PRESENT %s\n", id, entry_abspath);
            } else {
                printf("[%d] ABSENT %s\n", id, entry_abspath);
            }
        }
    }
    closedir(curdir);

    return didWork;
}

void worker(void *vid) {
    int id = *((int *) vid);
    int was_idle = 0;
    while (!is_done) {
        int didWork = grepNextDir(id);
        if (!didWork && !was_idle) {
            incrementIdle(&task_queue);
            was_idle = 1;
        } else if (didWork && was_idle) {
            decrementIdle(&task_queue);
            was_idle = 0;
        }

        if (task_queue.numIdleWorkers == N) {
            is_done = 1;
        }
    }

    printf("[%d] DONE\n", id);
}

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    const char *grep_bin = "grep";
    N = strtol(argv[1], NULL, 10);
    char *rootpath = argv[2];
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
    // // strncat(base_cmd, " ", 2);
    strncat(base_cmd, " > /dev/null ", 14);
    strncat(base_cmd, escape_special_chars(searchstr, buf), MAX_ABSPATH_LEN);
    strncat(base_cmd, " ", 2);
    base_cmd_len = strlen(base_cmd);

    is_done = 0;

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
