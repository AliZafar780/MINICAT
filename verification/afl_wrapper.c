#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>

/* AFL wrapper: fork instrumented minicat server, send input file bytes, kill. */
int main(int argc, char **argv) {
    pid_t pid;
    int sock, rc;
    struct sockaddr_in sa;
    char buf[65536];
    FILE *f;
    size_t n = 0, sent = 0;

    pid = fork();
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        dup2(dn, 0); dup2(dn, 1); dup2(dn, 2);
        execl("./minicat_afl", "minicat_afl", "-l", "-p", "18601", "-H", "-k", (char *)0);
        _exit(127);
    }
    usleep(6000);
    if (argc > 1) {
        f = fopen(argv[1], "rb");
        if (f) {
            n = fread(buf, 1, sizeof(buf), f);
            fclose(f);
        }
    }
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock >= 0) {
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(18601);
        sa.sin_addr.s_addr = inet_addr("127.0.0.1");
        rc = connect(sock, (struct sockaddr *)&sa, sizeof(sa));
        if (rc == 0) {
            while (sent < n) {
                rc = (int)send(sock, buf + sent, (size_t)(n - sent), MSG_NOSIGNAL);
                if (rc <= 0) break;
                sent += (size_t)rc;
            }
        }
        close(sock);
    }
    usleep(4000);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return 0;
}
