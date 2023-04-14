#include <arpa/inet.h>
#include <cstring>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>

int main(int argc, char** argv) {

    sockaddr_in serv_addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(1337),
    };

    inet_pton(AF_INET, "localhost", &serv_addr.sin_addr);

    auto sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == -1) {
        std::cerr << "socket() syscall failed: " << strerror(errno) << std::endl;
        return -1;
    }

    auto data = argv[1];
    auto len  = sendto(sock, data, strlen(data), 0, (sockaddr*)&serv_addr, sizeof(serv_addr));
    std::cout << "send time: " << std::chrono::steady_clock::now().time_since_epoch() << std::endl;
    if (len == -1)
        std::cerr << "sento() syscall failed: " << strerror(errno) << std::endl;
    else
        std::cout << "successfully sended " << len << " bytes" << std::endl;

    close(sock);
    return 0;
}
