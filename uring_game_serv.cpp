#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <thread>

#include <iostream>
#include <liburing.h>
#include <netinet/in.h>
#include <sys/mman.h>

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

struct printf_debug_handler {
    template <typename... Args>
    void operator()(auto&&... args) const {
        fprintf(stderr, args...);
    }
    constexpr operator bool() const noexcept {
        return true;
    }
};

struct metrics_store {
    uint64_t packets_received = 0;
};

enum sqe_op : uint64_t {
    sqe_op_recvmsg = 1,
    sqe_op_sendmsg,
};

struct uring_settings {
    uint32_t sq_depth = 32;
    uint32_t cq_multiplier = 8;
    uint32_t batch_size_multiplier = 2;
    uint32_t buf_size = 4096;
};

template <auto V>
struct type_c {};

template <uring_settings settings, typename RH, typename DH = printf_debug_handler>
class io_uring_ctx {
public:
    static constexpr auto sq_depth = settings.sq_depth;
    static constexpr auto cq_depth = sq_depth * settings.cq_multiplier;
    static constexpr auto batch_size = cq_depth * settings.batch_size_multiplier;
    static constexpr auto buf_size = settings.buf_size;
    static constexpr auto buf_ring_size = (sizeof(io_uring_buf) + buf_size) * batch_size;

    io_uring_ctx(type_c<settings>, RH receive_handler, DH debug_handler = printf_debug_handler{}):
        receive_h(std::move(receive_handler)), debug(std::move(debug_handler)) {
        setup();
    }

    int register_files(int* fds, unsigned int count) {
        int rc = io_uring_register_files(&ring, fds, count);
        if (rc)
            debug("register file failed: %s\n", strerror(-rc));
        return rc;
    }

    void add_recv_request(int idx) {
        io_uring_sqe* sqe = next_sqe();
        io_uring_prep_recvmsg_multishot(sqe, idx, &msg, MSG_TRUNC);
        sqe->flags |= IOSQE_FIXED_FILE;
        sqe->flags |= IOSQE_BUFFER_SELECT;
        sqe->buf_group = 0;
        sqe->user_data = sqe_op_recvmsg;
    }

    void run() {
        while (true) {
            add_recv_request(0);
            auto rc = io_uring_submit_and_wait(&ring, 1);
            if (rc == -EINTR) {
                fprintf(stderr, "EINTR\n");
                continue;
            }

            if (rc < 0) {
                debug("io_uring_submit_and_wait() failed: %d\n", rc);
                break;
            }

            auto count = io_uring_peek_batch_cqe(&ring, cqes, cq_depth /* batch_size */);
            //fprintf(stderr, "batch: %zu\n", count);
            for (size_t i = 0; i < count; ++i)
                rc = process_cqe(cqes[i], 0);

            //buf_ring_advance(int(count));
            io_uring_cq_advance(&ring, count);
        }
    }

    uint8_t* buffer(size_t idx) {
        return ((uint8_t*)buf_ring + sizeof(io_uring_buf) * batch_size) + idx * buf_size;
    }

private:
    void setup() {
        io_uring_params params = {
            .cq_entries = cq_depth,
            .flags = IORING_SETUP_SUBMIT_ALL | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_CQSIZE,
        };
        auto rc = io_uring_queue_init_params(sq_depth, &ring, &params);
        if (rc < 0)
            throw std::runtime_error("queue_init failed: " + std::string(strerror(-rc)));

        try {
            setup_buffer();
        }
        catch (...) {
            io_uring_queue_exit(&ring);
            throw;
        }
    }

    void setup_buffer() {
        buf_ring =
            (io_uring_buf_ring*)mmap(nullptr, buf_ring_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
        if (buf_ring == MAP_FAILED)
            throw std::runtime_error("buffer ring mmap failed: " + std::string(strerror(errno)));

        io_uring_buf_ring_init(buf_ring);

        io_uring_buf_reg reg = {
            .ring_addr = (uint64_t)buf_ring,
            .ring_entries = batch_size,
            .bgid = 0,
        };

        auto rc = io_uring_register_buf_ring(&ring, &reg, 0);
        if (rc)
            throw std::runtime_error("buffer ring init failed: " + std::string(strerror(-rc)));

        for (uint16_t i = 0; i < batch_size; ++i)
            io_uring_buf_ring_add(buf_ring, buffer(i), buf_size, i, io_uring_buf_ring_mask(batch_size), i);

        buf_ring_advance(int(batch_size));
    }

    void buf_ring_recycle(size_t idx) {
        io_uring_buf_ring_add(buf_ring,
                              buffer(idx),
                              buf_size,
                              uint16_t(idx),
                              io_uring_buf_ring_mask(batch_size),
                              0);
    }

    void buf_ring_advance(int count) {
        io_uring_buf_ring_advance(buf_ring, count);
    }

    io_uring_sqe* next_sqe() {
        if (auto sqe = io_uring_get_sqe(&ring))
            return sqe;

        debug("cannot get SQE: SQ is full, trying submit it to get next SQE...\n");
        io_uring_submit(&ring);

        if (auto sqe = io_uring_get_sqe(&ring))
            return sqe;

        debug("cannot get SQE\n");
        return nullptr;
    }

    struct buf_scope {
        buf_scope(): ctx(nullptr) {}

        buf_scope(uint8_t* ipayload, size_t ilen, size_t iidx, io_uring_ctx* ictx):
            payload(ipayload), len(ilen), idx(iidx), ctx(ictx) {}

        buf_scope(buf_scope&& bs) noexcept: payload(bs.payload), len(bs.len), idx(bs.idx), ctx(bs.ctx) {
            bs.ctx = nullptr;
        }

        buf_scope& operator=(buf_scope&& bs) noexcept {
            if (&bs == this)
                return *this;

            payload = bs.payload;
            len = bs.len;
            idx = bs.idx;
            ctx = bs.ctx;
            bs.ctx = nullptr;
            return *this;
        }

        ~buf_scope() {
            if (ctx) {
                ctx->buf_ring_recycle(idx);
                ctx->buf_ring_advance(1);
            }
        }

        const uint8_t* data() const {
            return payload;
        }

        size_t size() const {
            return len;
        }

        uint8_t*      payload;
        size_t        len;
        size_t        idx;
        io_uring_ctx* ctx;
    };

    int process_cqe_recv(io_uring_cqe* cqe, int /*fdidx*/) {
        if (cqe->res == -ENOBUFS) {
            debug("no buffers available\n");
            return 0;
        }

        if (!(cqe->flags & IORING_CQE_F_BUFFER) || cqe->res < 0) {
            debug("recv CQE have a bad res: %d\n", cqe->res);
            return -55;
        }
        auto idx = cqe->flags >> 16;

        io_uring_recvmsg_out* out = io_uring_recvmsg_validate(buffer(idx), cqe->res, &msg);
        if (!out) {
            debug("bad recvmsg\n");
            return -2;
        }

        auto payload_len = io_uring_recvmsg_payload_length(out, cqe->res, &msg);
        if (out->flags & MSG_TRUNC) {
            debug("truncated msg need %u received %u\n", out->payloadlen, payload_len);
            buf_ring_recycle(idx);
            buf_ring_advance(1);
            return 0;
        }

        auto payload = io_uring_recvmsg_payload(out, &msg);
        auto src = (sockaddr_in*)io_uring_recvmsg_name(out);

        receive_h(src, buf_scope{(uint8_t*)payload, payload_len, idx, this});

        //ring_recycle(idx);
        //buf_ring_advance(1);

        return 0;
    }

    int process_cqe(io_uring_cqe* cqe, int fdidx) {
        if (cqe->user_data == sqe_op_recvmsg)
            return process_cqe_recv(cqe, fdidx);
        return -1;
    }

private:
    io_uring ring = {};
    io_uring_buf_ring* buf_ring;
    io_uring_cqe* cqes[cq_depth];

    msghdr msg = {.msg_namelen = sizeof(sockaddr_in)};

    RH receive_h;
    DH debug;
};

#include "rigtorp/SPSCQueue.h"

template <typename T>
class worker {
public:
    struct data {
        sockaddr_in src;
        T buf;
    };

    worker() {
        t = std::thread(&worker::run, this);
    }

    ~worker() {
        t.join();
    }

    void push(sockaddr_in src, T&& data) {
        spsc.emplace(src, std::move(data));
    }

    void run() {
        while (true) {
            auto data = spsc.front();
            if (!data) {
                std::this_thread::yield();
                //std::this_thread::sleep_for(std::chrono::microseconds(5));
                continue;
            }

            char str[INET_ADDRSTRLEN + 1] = {0};
            inet_ntop(AF_INET, &data->src.sin_addr, str, sizeof(str));
            printf("ipaddr: %s:%i\n", str, ntohs(data->src.sin_port));
            printf("receive: %.*s\n", int(data->buf.size()), data->buf.data());

            ++metrics.packets_received;
            if (metrics.packets_received % 1000 == 0)
                fprintf(stderr, "received: %zu\n", metrics.packets_received);

            spsc.pop();
        }
    }

    static worker& instance() {
        static worker w;
        return w;
    }

private:
    rigtorp::SPSCQueue<data> spsc{512};
    std::thread t;
    metrics_store metrics;
};

int main() {
    io_uring_ctx ctx(type_c<uring_settings{}>{}, [&](sockaddr_in* src, auto&& buf) {
        worker<std::remove_reference_t<decltype(buf)>>::instance().push(*src, std::move(buf));
    });

    auto sockfd = setup_sock(1337);
    if (sockfd == -1) {
        std::cerr << "setup_sock() failed: " << strerror(errno) << std::endl;
        return 1;
    }

    ctx.register_files(&sockfd, 1);
    ctx.run();
}
