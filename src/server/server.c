#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>

int server_start(void) {
    while (1) {
        socket(PF_INET, SOCK_STREAM, 0);
        printf("Waiting for request...\n");
        sleep(1);
    };
}
