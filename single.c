#include <assert.h>     // for assert()
#include <dirent.h>     // for DIR and closedir()
#include <stdio.h>      // for printf()
#include <stdlib.h>     // for system() and malloc()
#include <sys/types.h>  // for opendir()
#include <string.h>     // for str{cmp,len,ncpy,ncat,rchr,tol}()
#include <unistd.h>     // for getcwd()

// Max absolute path length is 250, but allocate space for escaping special chars
#define MAX_ABSPATH_LEN (250+50)


// base command prefix used for all grep invocations
char base_cmd[1024];
int base_cmd_len;

struct task_node {
    char *abspath;  // escaped absolute path of dir
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

void enqueue(struct task_queue *tq, char *abspath) {
    assert(tq != NULL);
    assert(abspath != NULL);

    // create new node to be enqueued
    struct task_node *new = (struct task_node *) malloc(sizeof(struct task_node));
    if (new == NULL) {
        fprintf(stderr, "Enqueue: malloc() failed\n");
        return;
    }

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
}

int dequeue(struct task_queue *tq, char *buf) {
    assert(tq != NULL);
    assert(buf != NULL);

    int retval = 0;
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
            // dequeued only element, set queue to empty
            tq->head = NULL;
            tq->tail = NULL;
        } else {
            //
            tq->head = tq->head->next;
        }
        // deallocate node AFTER updating since we needed tq->head->next
        free(removed);
    }

    return retval;
}

int is_empty(struct task_queue *tq) {
    return tq->head == NULL;
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

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    // get parameters from commandline args
    // N is in base 10; assume whole argv[1] is valid number
    // const int N = strtol(argv[1], NULL, 10); // NOTE: N is IGNORED
    char *rootpath = argv[2];
    const char *searchstr = argv[3];

    init_queue(&task_queue);
    // enqueue rootpath; ensure it is an absolute path
    char buf[MAX_ABSPATH_LEN];
    enqueue(&task_queue, make_abspath(getcwd(buf, MAX_ABSPATH_LEN), rootpath));

    // construct base command: grep > /dev/null 'escaped_searchstr'
    strncpy(base_cmd, "grep > /dev/null ", 18);
    strncat(base_cmd, escape_special_chars(searchstr, buf), MAX_ABSPATH_LEN);
    strncat(base_cmd, " ", 2);      // pre-add space for searchfile
    base_cmd_len = strlen(base_cmd);

    // do work; We are done if queue becomes empty (i.e. No new dirs enqueued)
    while (!is_empty(&task_queue)) {
        grepNextDir(&task_queue, 0);
    }

    return 0;
}
