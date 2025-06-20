#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdbool.h>

#include "listener_socket.h"
#include "queue.h"
#include "rwlock.h"
#include "iowrapper.h"
#include "response.h"

#define BUF_SIZE 4096

static queue_t *queue_create;
static int num_threads;
static pthread_t  *workers;
static Listener_Socket_t*new_listener = NULL;

static pthread_mutex_t new_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct lock_entry {
    char *uri;
    rwlock_t *lock;
    struct lock_entry *next;
} lock_entry_t;

static lock_entry_t   *create_lock  = NULL;
static pthread_mutex_t locks_mutex = PTHREAD_MUTEX_INITIALIZER;

int in_array(char character, char *word) {
    int count = 0;
    int l = strlen(word);

    for (int i = 0; i < l; i++) {
        if (word[i] == character) {
            count++;
        }
    }

     if (count >= 1) {
        return 0;
    } else {
        return -1;
    }
}

static rwlock_t *creates_lock(const char *uri) {
    pthread_mutex_lock(&locks_mutex);
    lock_entry_t *entry = create_lock;

    while (entry!= NULL && strcmp(entry->uri, uri) != 0) {
        entry = entry->next;
    }

    if (entry == NULL) {
        entry = calloc(1, sizeof(*entry));
        if (entry == NULL) {
            exit(1);
        }

        entry->uri  = strdup(uri);
        if (entry->uri == NULL) {
            exit(1);
        }

        entry->lock = rwlock_new(READERS, 0);
        entry->next = create_lock;
        create_lock = entry;
    }

    pthread_mutex_unlock(&locks_mutex);
    return entry->lock;
}

static void handle_sigterm(int sig) {
    (void)sig;
    if (new_listener!=NULL) {
        ls_delete(&new_listener);
    }

    if (queue_create!=NULL) {
        queue_delete(&queue_create);
    }
    exit(EXIT_SUCCESS);
}

static ssize_t reading(int fd, char *buf, size_t max) {
    size_t i = 0;
    char c;
    ssize_t n;

    while (i < max - 1) {
        n = read(fd, &c, 1);
        if (n <= 0) {
            return -1;
        }

        buf[i] = c;
        i++;
        if (i >= 2 && buf[i-2] =='\r'&& buf[i-1] == '\n') {
            break;
        }
    }

    buf[i] = '\0';
    return i;
}

static void send_status(int fd,const char *version,const Response_t *resp,size_t content_length)
{
    dprintf(fd,"%s %u %s\r\n""Content-Length: %zu\r\n""\r\n",version,response_get_code(resp),response_get_message(resp),content_length);
}

static void handle_request(int fd) {
    char buf[BUF_SIZE];
    char method[8];
    char uri[1024];
    char version[16];
    char request_id[64] = "0";
    int  content_length  = 0;
    int  status_code = 500;

    if (reading(fd, buf, sizeof(buf)) < 0) {
        close(fd);
        return;
    }

    if (sscanf(buf, "%7s %1023s %15s", method, uri, version) != 3) {
        status_code = response_get_code(&RESPONSE_BAD_REQUEST);
        goto jump;
    }

    while (true) {
        if (reading(fd, buf, sizeof(buf)) <= 0) {
            break;
        }

        if (strcmp(buf, "\r\n") == 0) {
            break;
        }

        char *one = strtok(buf, ":");
        char *two = strtok(NULL, "\r\n");

        if (one == NULL || two== NULL) {
            continue;
        }

        while (*two == ' ') {
            two++;
        }

        if (strcmp(one, "Content-Length") == 0) {
            content_length = atoi(two);
        }
        else if (strcmp(one, "Request-Id") == 0) {
            strncpy(request_id, two, sizeof(request_id)-1);
        }
    }

    rwlock_t *lock = creates_lock(uri);

    if (strcmp(method, "GET") == 0) {
        reader_lock(lock);
        int f = open(uri + 1, O_RDONLY);

        if (f>=0) {
            struct stat st;
            fstat(f, &st);
            size_t len = st.st_size;

            send_status(fd, version, &RESPONSE_OK, len);
            pass_n_bytes(f, fd, len);
            close(f);

            status_code = response_get_code(&RESPONSE_OK);
        }
        else {
            const char *msg = "Not Found\n";
            send_status(fd, version, &RESPONSE_NOT_FOUND, strlen(msg));
            write_n_bytes(fd, (char *)msg, strlen(msg));

            status_code = response_get_code(&RESPONSE_NOT_FOUND);
        }
        pthread_mutex_lock(&new_mutex);
        write(STDERR_FILENO, "GET,", 4);
        write(STDERR_FILENO, uri, strlen(uri));
        write(STDERR_FILENO, ",", 1);
        dprintf(STDERR_FILENO, "%d", status_code);
        write(STDERR_FILENO, ",", 1);
        write(STDERR_FILENO, request_id, strlen(request_id));
        write(STDERR_FILENO, "\n", 1);
        pthread_mutex_unlock(&new_mutex);
        reader_unlock(lock);
    }
 else if(strcmp(method, "PUT") == 0) {
    char*data = calloc(1, content_length);
    if (data == NULL) {
        exit(1);
    }

    ssize_t got = read_n_bytes(fd, data, content_length);
    if (got<0) {
        free(data);
        close(fd);
        return;
    }

    writer_lock(lock);
    bool accessed = (access(uri + 1, F_OK) == 0);
    int f = open(uri + 1, O_CREAT | O_WRONLY | O_TRUNC, 0666);

    if (f >= 0) {
        write_n_bytes(f, data, got);
        close(f);

        if (accessed==false) {
            const char *msg = "Created\n";
            send_status(fd, version, &RESPONSE_CREATED, strlen(msg));
            write_n_bytes(fd,(char*)msg,strlen(msg));
            status_code = response_get_code(&RESPONSE_CREATED);
        } 
        
        else {
            const char *msg = "OK\n";
            send_status(fd, version, &RESPONSE_OK, strlen(msg));
            write_n_bytes(fd, (char*)msg, strlen(msg));
            status_code = response_get_code(&RESPONSE_OK);
        }
    } else {
        const Response_t *response;
        if (errno == EACCES||errno == EISDIR||errno == ENOENT) {
            response = &RESPONSE_FORBIDDEN;
        } else {
            response = &RESPONSE_INTERNAL_SERVER_ERROR;
        }

        const char *msg = response_get_message(response);

        send_status(fd, version, response, strlen(msg)+1);
        write_n_bytes(fd, (char*)msg, strlen(msg)+1);
        status_code = response_get_code(response);
    }
    free(data);
    pthread_mutex_lock(&new_mutex);
    write(STDERR_FILENO, "PUT,", 4);
    write(STDERR_FILENO, uri, strlen(uri));
    write(STDERR_FILENO, ",", 1);
    dprintf(STDERR_FILENO, "%d", status_code);
    write(STDERR_FILENO, ",", 1);
    write(STDERR_FILENO, request_id, strlen(request_id));
    write(STDERR_FILENO, "\n", 1);
    pthread_mutex_unlock(&new_mutex);
    writer_unlock(lock);
}
    else {
        const char *msg = "Method Not Allowed\n";
        send_status(fd, version, &RESPONSE_NOT_IMPLEMENTED, strlen(msg));
        dprintf(fd, "Allow: GET, PUT\r\n\r\n");
        write_n_bytes(fd, (char *)msg, strlen(msg));

        pthread_mutex_lock(&new_mutex);
        write(STDERR_FILENO, method, strlen(method));
        write(STDERR_FILENO, ",", 1);
        write(STDERR_FILENO, uri, strlen(uri));
        write(STDERR_FILENO, ",", 1);
        dprintf(STDERR_FILENO, "%d", response_get_code(&RESPONSE_NOT_IMPLEMENTED));
        write(STDERR_FILENO, ",", 1);
        write(STDERR_FILENO, request_id, strlen(request_id));
        write(STDERR_FILENO, "\n", 1);
        pthread_mutex_unlock(&new_mutex);
    }

jump:
    close(fd);
}

static void *workerfunc(void *arg) {
    (void)arg;
    while (1) {
        void *elem;
        if (queue_pop(queue_create, &elem)) {
            int cfd = *(int *)elem;
            free(elem);
            handle_request(cfd);
        }
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    long process = sysconf(_SC_NPROCESSORS_ONLN);
    if (process<1) {
        process = 1;
    }
    num_threads = (int)process;
    int opt;
    while ((opt = getopt(argc, argv, "t:")) != -1) {
        if (opt == 't') {
            int t = atoi(optarg);
            if (t>0) {
                num_threads =t;
            }
        } else {
            write(STDERR_FILENO, "Usage: ", 7);
            write(STDERR_FILENO, argv[0], strlen(argv[0]));
            write(STDERR_FILENO, " [-t threads] <port>\n", 20);
            exit(1);
        }
    }

    if (optind>=argc) {
        write(STDERR_FILENO, "Usage: ", 7);
        write(STDERR_FILENO, argv[0], strlen(argv[0]));
        write(STDERR_FILENO, " [-t threads] <port>\n", 20);
        exit(1);
    }

    int port = atoi(argv[optind]);
    signal(SIGTERM, handle_sigterm);
    queue_create = queue_new(1024);
    if (queue_create == NULL) {
        exit(1);
    }

    workers = calloc(num_threads, sizeof(*workers));
    if (workers == NULL) {
        exit(1);
    }
    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&workers[i], NULL, workerfunc, NULL) != 0) {
            exit(1);
        }
    }

    new_listener = ls_new(port);
    if (new_listener == NULL) {
        write(STDERR_FILENO, "Failed to initialize listener socket on port ", 39);
        dprintf(STDERR_FILENO, "%d\n", port);
        exit(1);
    }

    while (1) {
        int number = ls_accept(new_listener);
        if (number<0) {
            if (errno == EINTR) {
                continue;
            }
            else{
                break;
            }
            
        }
        int*mem = calloc(1, sizeof(*mem));
        if (mem == NULL) {
            continue;
        }
        *mem=number;
        if (!queue_push(queue_create, mem)) {
            free(mem);
            write(STDERR_FILENO, "Cannot push queue\n", 19);
            close(number);
        }
    }
    ls_delete(&new_listener);
    queue_delete(&queue_create);
    return 0;
}
