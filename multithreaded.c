#include <assert.h>     // for assert()
#include <dirent.h>     // for DIR and closedir()
#include <pthread.h>    // for pthread_mutex_t and related functions
#include <stdio.h>      // for printf()
#include <stdlib.h>     // for system() and malloc()
#include <sys/types.h>  // for opendir()
#include <string.h>     // for str{cmp,len,ncpy,ncat,rchr,tol}()
#include <unistd.h>     // for getcwd()

// Max absolute path length is 250, but allocate space for escaping special chars
#define MAX_ABSPATH_LEN (250+50)
#define MAX_THREADS 8


// base command prefix used for all grep invocations
char base_cmd[1024];
int base_cmd_len;

// Worker thread state, with respect to ENQUEUE workload
enum THRD_STATE {
    READY = 0,      // hasn't ran yet for this "cycle"
    IDLE = 1,       // ran, but did NOT enqueue new dirs
    DIDWORK = 2     // ran AND ENQUEUEd new dirs
};

struct task_node {
    char *abspath;  // escaped absolute path of dir
    struct task_node *next;
};

struct task_queue {
    pthread_mutex_t lock;
    struct task_node *head;
    struct task_node *tail;

    enum THRD_STATE thread_state[MAX_THREADS];
    // locks for synchronization of "cycles" and determining when to terminate
    pthread_mutex_t thread_lock[MAX_THREADS];
    pthread_mutex_t checklock;      // lock when modifying state of other threads
    int N;
    int is_done;
} task_queue;

void init_queue(struct task_queue *tq, int N) {
    pthread_mutex_init(&tq->lock, NULL);
    pthread_mutex_init(&tq->checklock, NULL);
    tq->head = NULL;
    tq->tail = NULL;

    tq->N = N;
    tq->is_done = 0;
    // Initialize thread states to READY for very first "cycle"
    for (int i = 0; i < MAX_THREADS; ++i) {
        tq->thread_state[i] = READY;
        pthread_mutex_init(&tq->thread_lock[i], NULL);
    }
}

void enqueue(struct task_queue *tq, char *abspath) {
    assert(tq != NULL);
    assert(abspath != NULL);

    // create new node to be enqueued
    struct task_node *new = (struct task_node *) malloc(sizeof(struct task_node));
    if (new == NULL) {
        fprintf(stderr, "Enqueue: malloc() failed\n");
        return;
    }

    pthread_mutex_lock(&tq->lock);
    new->abspath = abspath;
    new->next = NULL;

    if (tq->tail == NULL) {
        // enqueue to empty queue, set to both tail and head
        tq->head = new;
        tq->tail = new;
    } else {
        // add new tail
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
        // dequeue from empty queue
        retval = -1;
    } else {
        assert(removed->abspath != NULL);
        // copy abspath to provided buffer as we will dellocate original
        strncpy(buf, removed->abspath, MAX_ABSPATH_LEN);
        free(removed->abspath);     // deallocate original abspath string

        if (tq->head == tq->tail) {
            // dequeued last element, set queue to empty
            tq->head = NULL;
            tq->tail = NULL;
        } else {
            // dequeue current head; advance head to next element
            tq->head = tq->head->next;
        }
        // deallocate node AFTER updating since we needed tq->head->next
        free(removed);
    }
    pthread_mutex_unlock(&tq->lock);

    return retval;
}

int is_empty(struct task_queue *tq) {
    return tq->head == NULL;
}

void destroy_queue(struct task_queue *tq) {
    pthread_mutex_destroy(&tq->lock);
    pthread_mutex_destroy(&tq->checklock);

    // ensure all nodes/strings remaining in task queue are deallocated
    char discard[MAX_ABSPATH_LEN];
    while (!is_empty(tq)) {
        dequeue(tq, discard);
    }

    // destroy mutexes for each worker thread
    for (int i = 0; i < MAX_THREADS; ++i) {
        pthread_mutex_destroy(&tq->thread_lock[i]);
    }
}

char *escape_special_chars(const char *string, char *buf) {
    int len = strlen(string);
    char *start = (char *)string;
    char *end;

    // wrap string with ' (single quotes) to escape (most) special chars
    strncpy(buf, "'", 2);
    // However, we need to escape single quotes (') that appear inside the string
    // This is done by ending the current string segment, via ' (single quote)
    // wrap the single quote as another string segment "'", then start another
    // escaped string segment with a single quote '
    while ((end = strchr(start, '\'')) != NULL) {   // find each ' inside string
        strncat(buf, start, end-start); // copy chars from uncopied segment upto BEFORE '
        strncat(buf, "'\"'\"'", 6);     // single quote "escaped" with double quotes
        start = end+1;                  // next string segment starts AFTER '
    }

    // if start is not equal to end of string (string+len, null terminating byte)
    // then we have a remaining uncopied segment, which we simply append
    if (start != (string+len))
        strncat(buf, start, (string+len)-start);
    // close escaped string with '
    strncat(buf, "'", 2);

    return buf;
}

char *make_abspath(const char *parent_abspath, char *path) {
    // Normalize path: strip possible / at end by overwriting with null byte
    char *lastChar = path + strlen(path)-1;
    if (*lastChar == '/') {
        *lastChar = '\0';
    }

    // always allocate new space for created string since it will be enqueued
    char *abspath = (char *) malloc(MAX_ABSPATH_LEN * sizeof(char));
    if (path[0] == '/') {
        // already absolute path, simply make copy
        strncpy(abspath, path, MAX_ABSPATH_LEN);
    } else {
        // join parent_abspath and path with path separator /
        strncpy(abspath, parent_abspath, MAX_ABSPATH_LEN);
        strncat(abspath, "/", 2);
        strncat(abspath, path, MAX_ABSPATH_LEN);
    }

    return abspath;
}

int grepNextDir(struct task_queue *tq, int id) {
    char curpath[MAX_ABSPATH_LEN];
    char cmd[1024];

    if (is_empty(tq) || dequeue(tq, curpath) == -1) {
        return 0;       // queue is empty, standby
    }
    // NOTE: next dir path already placed in curpath by dequeue() above
    // since we only enqueue absolute paths, curpath is also absolute

    int didWork = 0;    // did we ENQUEUE new dirs?
    printf("[%d] DIR %s\n", id, curpath);
    DIR *curdir = opendir(curpath);
    struct dirent *entry;
    while ((entry = readdir(curdir)) != NULL) {
        // name of file or directory is AFTER last / (located by strchr())
        char *base_name = strrchr(entry->d_name, '/');
        // in case NO / found, entry->d_name is already base_name
        base_name = base_name ? base_name+1 : entry->d_name;
        // skip . and .. entries
        if (strcmp(base_name, ".") == 0 || strcmp(base_name, "..") == 0)
            continue;

        // for printing, ALWAYS use absolute path
        char *entry_abspath = make_abspath(curpath, base_name);
        if (entry->d_type == DT_DIR) {
            // found subdirectory, ENQUEUE as absolutepath
            enqueue(tq, entry_abspath);
            printf("[%d] ENQUEUE %s\n", id, entry_abspath);
            didWork = 1;    // we ENQUEUEd new dirs, inform others
        } else {
            // found file, call grep with pre-computed base command
            strncpy(cmd, base_cmd, base_cmd_len+1); // +1 for null terminating byte

            // BUT escape searchfile for shell
            char buf[MAX_ABSPATH_LEN];
            escape_special_chars(entry_abspath, buf);
            strncat(cmd, buf, MAX_ABSPATH_LEN);     // append escaped filepath

            if (system(cmd) == 0) {     // grep returns 0 for success
                printf("[%d] PRESENT %s\n", id, entry_abspath);
            } else {
                printf("[%d] ABSENT %s\n", id, entry_abspath);
            }
        }
    }
    closedir(curdir);   // done with curdir

    return didWork;
}

void wakeup_threads(struct task_queue *tq, int onlyWakeIdle) {
    for (int i = 0; i < tq->N; ++i) {
        if (tq->thread_state[i] != READY) {
            if (onlyWakeIdle && tq->thread_state[i] != IDLE) {
                // instructed to only wake IDLE threads, but this thread is NOT IDLE
                continue;
            }

            // set thread state to READY and wake up so that it can run again
            tq->thread_state[i] = READY;
            pthread_mutex_unlock(&tq->thread_lock[i]);
        }
    }
}

void worker(void *idp) {
    int id = *((int *) idp);
    // Only terminate when is_done, i.e. no more dirs to be enqueued
    while (!task_queue.is_done) {
        // acquire lock; threads that already ran for this cycle will sleep
        pthread_mutex_lock(&task_queue.thread_lock[id]);
        const int didWork = grepNextDir(&task_queue, id);
        // update thread state based on didWork (see above)
        task_queue.thread_state[id] = didWork? DIDWORK : IDLE;

        // acquire checklock since we might modify state of other threads
        pthread_mutex_lock(&task_queue.checklock);
        // we added new dirs, wakeup threads that did not ENQUEUE for this "cycle"
        // i.e. the IDLE threads so that they have chance to ENQUEUE
        if (didWork && !is_empty(&task_queue))
            wakeup_threads(&task_queue, /*onlyWakeIdle=*/1);

        // Check thread states; assume first that all were able to run
        // without ENQUEUEing new dirs, i.e. they were IDLE
        int all_ran = 1;
        int all_idle = 1;
        for (int i = 0; i < task_queue.N; ++i) {
            switch (task_queue.thread_state[i]) {
            case READY:
                all_ran = 0;    // found thread that did not run, also NOT IDLE
            case DIDWORK:
                all_idle = 0;   // found thread which DIDWORK, i.e. NOT IDLE
            default:
                ;               // do nothing
            }
        }

        // If all threads have already ran for this cycle, start NEXT cycle
        if (all_ran)
            wakeup_threads(&task_queue, /*onlyWakeIdle=*/0);

        // If all threads were idle for this cycle, no one will ENQUEUE anymore
        // Hence, we are DONE; wakeup all threads so they can terminate
        if (all_idle && is_empty(&task_queue)) {
            task_queue.is_done = 1;
            wakeup_threads(&task_queue, /*onlyWakeIdle=*/0);
        }
        pthread_mutex_unlock(&task_queue.checklock);
    }
}

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    // get parameters from commandline args
    // N is in base 10; assume whole argv[1] is valid number
    const int N = strtol(argv[1], NULL, 10);
    char *rootpath = argv[2];
    const char *searchstr = argv[3];

    init_queue(&task_queue, N);
    // enqueue rootpath; ensure it is an absolute path
    char buf[MAX_ABSPATH_LEN];
    enqueue(&task_queue, make_abspath(getcwd(buf, MAX_ABSPATH_LEN), rootpath));

    // construct base command: grep > /dev/null 'escaped_searchstr'
    strncpy(base_cmd, "grep > /dev/null ", 18);
    strncat(base_cmd, escape_special_chars(searchstr, buf), MAX_ABSPATH_LEN);
    strncat(base_cmd, " ", 2);      // pre-add space for searchfile
    base_cmd_len = strlen(base_cmd);

    // create N worker threads
    pthread_t tid[N];
    int id[N];
    for (int i = 0; i < N; ++i) {
        id[i] = i;
        pthread_create(&tid[i], NULL, (void *)worker, &id[i]);
    }
    // wait for ALL N worker threads to finish
    for (int i = 0; i < N; ++i) {
        pthread_join(tid[i], NULL);
    }

    // We are done, ensure mutex and remaining nodes (if any)
    // in the task queue are destroyed/deallocated
    destroy_queue(&task_queue);

    return 0;
}
