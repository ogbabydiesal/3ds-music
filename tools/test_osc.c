// Host-side test: encode a few messages with source/osc.c and send them to localhost.
// Build: cc -I common common/osc.c tools/test_osc.c -o build/test_osc
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include "osc.h"

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 9000;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in d = { .sin_family = AF_INET, .sin_port = htons(port) };
    d.sin_addr.s_addr = inet_addr("127.0.0.1");
    uint8_t buf[256];
    size_t n;
    n = osc_encode(buf, sizeof buf, "/3ds/hello", "f", 1.0);            sendto(s, buf, n, 0, (void *)&d, sizeof d);
    n = osc_encode(buf, sizeof buf, "/3ds/xy", "ff", 0.25, 0.75);       sendto(s, buf, n, 0, (void *)&d, sizeof d);
    n = osc_encode(buf, sizeof buf, "/3ds/accel", "fff", 0.1, -0.2, 1.0); sendto(s, buf, n, 0, (void *)&d, sizeof d);
    n = osc_encode(buf, sizeof buf, "/3ds/mixed", "isf", 42, "abc", 3.5); sendto(s, buf, n, 0, (void *)&d, sizeof d);
    close(s);
    return 0;
}
