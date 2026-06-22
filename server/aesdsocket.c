#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define PORT 9000
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define RECV_BUF_SIZE 1024
#define SEND_BUF_SIZE 4096
#define PACKET_INIT_SIZE 128
#define TIMESTAMP_INTERVAL_SEC 10
#define RFC2822_FMT "%a, %d %b %Y %H:%M:%S %z"

static volatile sig_atomic_t exit_requested = 0;

static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t thread_list_mutex = PTHREAD_MUTEX_INITIALIZER;

struct thread_entry {
    pthread_t thread_id;
    int client_fd;
    SLIST_ENTRY(thread_entry) entries;
};

SLIST_HEAD(thread_list, thread_entry);
static struct thread_list active_threads = SLIST_HEAD_INITIALIZER(active_threads);

struct connection_info {
    int client_fd;
    char client_ip[INET_ADDRSTRLEN];
};

static int become_daemon(void)
{
    pid_t pid;

    pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid > 0) {
        exit(0);
    }

    if (setsid() < 0) {
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid > 0) {
        exit(0);
    }

    if (chdir("/") < 0) {
        return -1;
    }

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    if (open("/dev/null", O_RDONLY) < 0) {
        return -1;
    }
    if (open("/dev/null", O_RDWR) < 0) {
        return -1;
    }
    if (open("/dev/null", O_RDWR) < 0) {
        return -1;
    }

    return 0;
}

static void handle_signal(int signo)
{
    (void)signo;
    exit_requested = 1;
}

static int setup_server_socket(void)
{
    int server_fd;
    int opt = 1;
    struct sockaddr_in server_addr;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        return -1;
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        close(server_fd);
        return -1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, 16) < 0) {
        close(server_fd);
        return -1;
    }

    return server_fd;
}

static int append_data_to_file_unlocked(const char *data, size_t data_len)
{
    int fd;
    ssize_t written = 0;

    fd = open(DATA_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        syslog(LOG_ERR, "Failed to open %s: %s", DATA_FILE, strerror(errno));
        return -1;
    }

    while (written < (ssize_t)data_len) {
        ssize_t rc = write(fd, data + written, data_len - (size_t)written);
        if (rc < 0) {
            syslog(LOG_ERR, "Failed to write to %s: %s", DATA_FILE, strerror(errno));
            close(fd);
            return -1;
        }
        written += rc;
    }

    if (close(fd) < 0) {
        syslog(LOG_ERR, "Failed to close %s: %s", DATA_FILE, strerror(errno));
        return -1;
    }

    return 0;
}

static int send_file_to_client_unlocked(int client_fd)
{
    int file_fd;
    char send_buf[SEND_BUF_SIZE];
    ssize_t bytes_read;

    file_fd = open(DATA_FILE, O_RDONLY);
    if (file_fd < 0) {
        if (errno == ENOENT) {
            return 0;
        }
        syslog(LOG_ERR, "Failed to open %s for reading: %s", DATA_FILE, strerror(errno));
        return -1;
    }

    while ((bytes_read = read(file_fd, send_buf, sizeof(send_buf))) > 0) {
        ssize_t sent = 0;

        while (sent < bytes_read) {
            ssize_t rc = send(client_fd, send_buf + sent, (size_t)(bytes_read - sent), 0);
            if (rc < 0) {
                syslog(LOG_ERR, "Failed to send file contents: %s", strerror(errno));
                close(file_fd);
                return -1;
            }
            sent += rc;
        }
    }

    if (bytes_read < 0) {
        syslog(LOG_ERR, "Failed to read %s: %s", DATA_FILE, strerror(errno));
        close(file_fd);
        return -1;
    }

    if (close(file_fd) < 0) {
        syslog(LOG_ERR, "Failed to close %s: %s", DATA_FILE, strerror(errno));
        return -1;
    }

    return 0;
}

static int append_timestamp_unlocked(void)
{
    char time_buf[64];
    char line[128];
    struct timespec ts;
    struct tm tm_info;
    size_t line_len;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        syslog(LOG_ERR, "clock_gettime failed: %s", strerror(errno));
        return -1;
    }

    if (localtime_r(&ts.tv_sec, &tm_info) == NULL) {
        syslog(LOG_ERR, "localtime_r failed: %s", strerror(errno));
        return -1;
    }

    if (strftime(time_buf, sizeof(time_buf), RFC2822_FMT, &tm_info) == 0) {
        syslog(LOG_ERR, "strftime failed to format timestamp");
        return -1;
    }

    line_len = (size_t)snprintf(line, sizeof(line), "timestamp:%s\n", time_buf);
    return append_data_to_file_unlocked(line, line_len);
}

static int append_char_to_packet(char **packet_buf, size_t *packet_len, size_t *packet_cap, char ch)
{
    if (*packet_len + 1 > *packet_cap) {
        size_t new_cap = (*packet_cap == 0) ? PACKET_INIT_SIZE : (*packet_cap * 2);
        char *new_buf = realloc(*packet_buf, new_cap);

        if (new_buf == NULL) {
            syslog(LOG_ERR, "Failed to allocate memory for packet buffer");
            *packet_len = 0;
            return -1;
        }

        *packet_buf = new_buf;
        *packet_cap = new_cap;
    }

    (*packet_buf)[*packet_len] = ch;
    (*packet_len)++;
    return 0;
}

static int process_complete_packet(int client_fd, char *packet_buf, size_t packet_len)
{
    int rc = 0;

    if (packet_len == 0) {
        return 0;
    }

    pthread_mutex_lock(&file_mutex);
    if (append_data_to_file_unlocked(packet_buf, packet_len) == 0) {
        rc = send_file_to_client_unlocked(client_fd);
    } else {
        rc = -1;
    }
    pthread_mutex_unlock(&file_mutex);

    return rc;
}

static void *timer_thread_func(void *arg)
{
    int i;

    (void)arg;

    while (!exit_requested) {
        for (i = 0; i < TIMESTAMP_INTERVAL_SEC && !exit_requested; i++) {
            sleep(1);
        }
        if (exit_requested) {
            break;
        }

        pthread_mutex_lock(&file_mutex);
        append_timestamp_unlocked();
        pthread_mutex_unlock(&file_mutex);
    }

    return NULL;
}

static void *connection_thread_func(void *arg)
{
    struct connection_info *info = arg;
    int client_fd = info->client_fd;
    char client_ip[INET_ADDRSTRLEN];
    char recv_buf[RECV_BUF_SIZE];
    char *packet_buf = NULL;
    size_t packet_len = 0;
    size_t packet_cap = 0;
    ssize_t bytes_received;
    size_t i;

    strncpy(client_ip, info->client_ip, sizeof(client_ip));
    client_ip[sizeof(client_ip) - 1] = '\0';
    free(info);

    while (!exit_requested) {
        bytes_received = recv(client_fd, recv_buf, sizeof(recv_buf), 0);
        if (bytes_received == 0) {
            break;
        }
        if (bytes_received < 0) {
            if (errno == EINTR && exit_requested) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            syslog(LOG_ERR, "recv failed: %s", strerror(errno));
            break;
        }

        for (i = 0; i < (size_t)bytes_received; i++) {
            if (recv_buf[i] == '\n') {
                if (append_char_to_packet(&packet_buf, &packet_len, &packet_cap, '\n') == 0) {
                    if (process_complete_packet(client_fd, packet_buf, packet_len) < 0) {
                        free(packet_buf);
                        syslog(LOG_INFO, "Closed connection from %s", client_ip);
                        close(client_fd);
                        return NULL;
                    }
                }
                packet_len = 0;
            } else {
                if (append_char_to_packet(&packet_buf, &packet_len, &packet_cap, recv_buf[i]) < 0) {
                    packet_len = 0;
                }
            }
        }
    }

    free(packet_buf);
    syslog(LOG_INFO, "Closed connection from %s", client_ip);
    close(client_fd);
    return NULL;
}

static int add_thread_entry(pthread_t thread_id, int client_fd)
{
    struct thread_entry *entry = malloc(sizeof(*entry));

    if (entry == NULL) {
        syslog(LOG_ERR, "Failed to allocate thread entry");
        return -1;
    }

    entry->thread_id = thread_id;
    entry->client_fd = client_fd;

    pthread_mutex_lock(&thread_list_mutex);
    SLIST_INSERT_HEAD(&active_threads, entry, entries);
    pthread_mutex_unlock(&thread_list_mutex);

    return 0;
}

static void shutdown_active_connections(void)
{
    struct thread_entry *entry;

    pthread_mutex_lock(&thread_list_mutex);
    SLIST_FOREACH(entry, &active_threads, entries) {
        if (entry->client_fd >= 0) {
            shutdown(entry->client_fd, SHUT_RDWR);
        }
    }
    pthread_mutex_unlock(&thread_list_mutex);
}

static void join_all_threads(void)
{
    struct thread_entry *entry;

    while (1) {
        pthread_mutex_lock(&thread_list_mutex);
        entry = SLIST_FIRST(&active_threads);
        if (entry == NULL) {
            pthread_mutex_unlock(&thread_list_mutex);
            break;
        }
        SLIST_REMOVE_HEAD(&active_threads, entries);
        pthread_mutex_unlock(&thread_list_mutex);

        pthread_join(entry->thread_id, NULL);
        free(entry);
    }
}

static int start_connection_thread(int client_fd, const char *client_ip)
{
    struct connection_info *info;
    pthread_t thread_id;
    int rc;

    info = malloc(sizeof(*info));
    if (info == NULL) {
        syslog(LOG_ERR, "Failed to allocate connection info");
        return -1;
    }

    info->client_fd = client_fd;
    strncpy(info->client_ip, client_ip, sizeof(info->client_ip));
    info->client_ip[sizeof(info->client_ip) - 1] = '\0';

    rc = pthread_create(&thread_id, NULL, connection_thread_func, info);
    if (rc != 0) {
        syslog(LOG_ERR, "pthread_create failed: %s", strerror(rc));
        free(info);
        return -1;
    }

    if (add_thread_entry(thread_id, client_fd) < 0) {
        shutdown(client_fd, SHUT_RDWR);
        pthread_join(thread_id, NULL);
        return -1;
    }

    return 0;
}

static int start_timer_thread(void)
{
    pthread_t thread_id;
    int rc;

    rc = pthread_create(&thread_id, NULL, timer_thread_func, NULL);
    if (rc != 0) {
        syslog(LOG_ERR, "Failed to create timer thread: %s", strerror(rc));
        return -1;
    }

    if (add_thread_entry(thread_id, -1) < 0) {
        exit_requested = 1;
        pthread_join(thread_id, NULL);
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    int server_fd;
    struct sigaction sa;
    bool daemon_mode = false;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            daemon_mode = true;
        } else {
            fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
            return -1;
        }
    }

    if (daemon_mode && become_daemon() < 0) {
        return -1;
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0) {
        syslog(LOG_ERR, "Failed to register signal handlers: %s", strerror(errno));
        closelog();
        return -1;
    }

    server_fd = setup_server_socket();
    if (server_fd < 0) {
        syslog(LOG_ERR, "Failed to set up server socket");
        closelog();
        return -1;
    }

    if (start_timer_thread() < 0) {
        close(server_fd);
        closelog();
        return -1;
    }

    while (!exit_requested) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd;
        char client_ip[INET_ADDRSTRLEN];

        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR && exit_requested) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            continue;
        }

        if (inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip)) == NULL) {
            strncpy(client_ip, "unknown", sizeof(client_ip));
            client_ip[sizeof(client_ip) - 1] = '\0';
        }

        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        if (start_connection_thread(client_fd, client_ip) < 0) {
            syslog(LOG_INFO, "Closed connection from %s", client_ip);
            close(client_fd);
        }
    }

    close(server_fd);
    shutdown_active_connections();
    join_all_threads();
    unlink(DATA_FILE);
    syslog(LOG_INFO, "Caught signal, exiting");
    closelog();

    return 0;
}

