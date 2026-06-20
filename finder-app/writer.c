#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    const char *writefile;
    const char *writestr;
    int fd;
    ssize_t bytes_written;
    size_t len;

    openlog("writer", LOG_PID, LOG_USER);

    if (argc != 3) {
        syslog(LOG_ERR, "Invalid number of arguments");
        fprintf(stderr, "Usage: %s <writefile> <writestr>\n", argv[0]);
        closelog();
        return 1;
    }

    writefile = argv[1];
    writestr = argv[2];

    syslog(LOG_DEBUG, "Writing %s to %s", writestr, writefile);

    fd = open(writefile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        syslog(LOG_ERR, "Error opening file %s: %s", writefile, strerror(errno));
        closelog();
        return 1;
    }

    len = strlen(writestr);
    bytes_written = write(fd, writestr, len);
    if (bytes_written < 0 || (size_t)bytes_written != len) {
        syslog(LOG_ERR, "Error writing to file %s: %s", writefile, strerror(errno));
        close(fd);
        closelog();
        return 1;
    }

    if (close(fd) < 0) {
        syslog(LOG_ERR, "Error closing file %s: %s", writefile, strerror(errno));
        closelog();
        return 1;
    }

    closelog();
    return 0;
}
