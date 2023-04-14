#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>

#include <iostream>
#include <sys/mman.h>
#include <netinet/in.h>

int setup_sock(uint16_t port) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == -1)
        return sock;

    sockaddr_in addr{
        .sin_family = AF_INET,
        .sin_port   = htons(port),
        .sin_addr   = {INADDR_ANY},
    };

    int rc = bind(sock, (sockaddr*)&addr, sizeof(addr));
    if (rc == -1)
        return rc;

    return sock;
}

struct metrics_store {
    uint64_t packets_received = 0;
};

class recvmsg_serv {
public:
    recvmsg_serv(int isockfd): sockfd(isockfd) {}

    void run() {
        constexpr size_t buf_len = 4096;
        constexpr size_t bufs_count = 1;

        char buff[bufs_count][buf_len] = {0};
        iovec iov[bufs_count] = {};
        msghdr msg = {};

        for (size_t i = 0; i < bufs_count; ++i)
            iov[i] = iovec{
                .iov_base = buff[i],
                .iov_len  = sizeof(buff[i]),
            };

        char anciliary[2048];
        msg.msg_control = anciliary;
        msg.msg_controllen = sizeof(anciliary);
        msg.msg_iov = iov;
        msg.msg_iovlen = bufs_count;

        sockaddr_in src = {};
        msg.msg_name = &src;
        msg.msg_namelen = sizeof(src);

        while (true) {
            auto sz = recvmsg(sockfd, &msg, MSG_TRUNC);
            if (sz == -1) {
                fprintf(stderr, "bad recvmsg\n");
                continue;
            }

            if (size_t(sz) > buf_len)
                fprintf(stderr, "truncated msg need %zu received %zu\n", sz, buf_len);


            char str[INET_ADDRSTRLEN + 1] = {0};
            inet_ntop(AF_INET, &src.sin_addr, str, sizeof(str));
            printf("ipaddr: %s:%i\n", str, ntohs(src.sin_port));

            ++metrics.packets_received;
            if (metrics.packets_received % 10000 == 0)
                printf("received: %lu\n", metrics.packets_received);
            //printf("received %zu: %s\n", msg.msg_iov[0].iov_len, msg.msg_iov[0].iov_base);
        }
    }

private:
    int sockfd;
    metrics_store metrics;
};

int main() {
    auto sockfd = setup_sock(1337);
    if (sockfd == -1) {
        std::cerr << "setup_sock() failed: " << strerror(errno) << std::endl;
        return 1;
    }

    recvmsg_serv serv{sockfd};
    serv.run();
}
