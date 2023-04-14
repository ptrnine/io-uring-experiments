#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>

#include <iostream>
#include <sys/mman.h>
#include <netinet/in.h>
#include <liburing.h>

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
    operator bool() const noexcept {
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

template <typename F = printf_debug_handler>
class io_uring_ctx {
public:
    io_uring_ctx(uint32_t isq_depth      = 256,
                 uint32_t ibatch_size    = 2048,
                 uint32_t ibuf_shift     = 12,
                 F        idebug_handler = printf_debug_handler{}):
        sq_depth(isq_depth), batch_size(ibatch_size), buf_shift(12), debug_handler(idebug_handler) {
        setup();
    }

    int register_files(int* fds, unsigned int count) {
        int rc = io_uring_register_files(&ring, fds, count);
        if (debug_handler && rc)
            debug_handler("register file failed: %s", strerror(-rc));
        return rc;
    }

    void add_recv_request(int idx) {
        auto sqe = next_sqe();
        io_uring_prep_recvmsg_multishot(sqe, idx, &_msg, MSG_TRUNC);
        sqe->flags |= IOSQE_FIXED_FILE;
        sqe->flags |= IOSQE_BUFFER_SELECT;
        sqe->buf_group = 0;
        sqe->user_data = sqe_op_recvmsg;
    }

    void run() {
        io_uring_cqe* cqes[batch_size];
        add_recv_request(0);

        while (true) {
            auto rc = io_uring_submit_and_wait(&ring, 1);
            add_recv_request(0);
            if (rc == -EINTR)
                continue;

            if (rc < 0) {
                if (debug_handler)
                    debug_handler("io_uring_submit_and_wait() failed: %d\n", rc);
                break;
            }

            auto count = io_uring_peek_batch_cqe(&ring, cqes, batch_size);
            printf("batch count: %u\n", count);
            for (size_t i = 0; i < count; ++i)
                rc = process_cqe(cqes[i], 0);

            //buf_ring_advance(int(count));
            io_uring_cq_advance(&ring, count);
        }
    }

    uint32_t buffer_size() {
        return 1U << buf_shift;
    }

    uint8_t* buffer(size_t idx) {
        return buf_base + (idx << buf_shift);
    }

private:
    void setup() {
        io_uring_params params = {
            .cq_entries = sq_depth * 8,
            .flags      = IORING_SETUP_SUBMIT_ALL | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_CQSIZE,
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
        io_uring_buf_reg reg = {
            .ring_addr    = 0,
            .ring_entries = batch_size,
            .bgid         = 0,
        };

        buf_ring_size = (sizeof(io_uring_buf) + buffer_size()) * batch_size;
        buf_ring = (io_uring_buf_ring*)mmap(
            nullptr, buf_ring_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
        if (buf_ring == MAP_FAILED)
            throw std::runtime_error("buffer ring mmap failed: " + std::string(strerror(errno)));

        io_uring_buf_ring_init(buf_ring);

        reg = io_uring_buf_reg{
            .ring_addr = (uint64_t)buf_ring,
            .ring_entries = batch_size,
            .bgid = 0,
        };
        buf_base = (uint8_t*)buf_ring + sizeof(io_uring_buf) * batch_size;

        auto rc = io_uring_register_buf_ring(&ring, &reg, 0);
        if (rc)
            throw std::runtime_error("buffer ring init failed: " + std::string(strerror(-rc)));

        for (uint16_t i = 0; i < batch_size; ++i)
            io_uring_buf_ring_add(
                buf_ring, buffer(i), buffer_size(), i, io_uring_buf_ring_mask(batch_size), i);

        buf_ring_advance(int(batch_size));
    }

    void ring_recycle(size_t idx) {
        io_uring_buf_ring_add(buf_ring,
                              buffer(idx),
                              buffer_size(),
                              uint16_t(idx),
                              io_uring_buf_ring_mask(batch_size),
                              0);
    }

    void buf_ring_advance(int count) {
        io_uring_buf_ring_advance(buf_ring, count);
    }

    io_uring_sqe* next_sqe() {
        auto sqe = io_uring_get_sqe(&ring);
        if (!sqe) {
            if (debug_handler)
                debug_handler("cannot get SQE: SQ is full, trying submit it to get next SQE...\n");
            io_uring_submit(&ring);
            sqe = io_uring_get_sqe(&ring);
        }
        if (!sqe && debug_handler)
                debug_handler("cannot get SQE\n");
        return sqe;
    }

    struct buf_scope {
        ~buf_scope() {
            ctx->ring_recycle(idx);
            ctx->buf_ring_advance(1);
        }

        uint8_t*      payload;
        size_t        len;
        size_t        idx;
        io_uring_ctx* ctx;
    };

    int process_cqe_recv(io_uring_cqe* cqe, int fdidx) {
        if (cqe->res == -ENOBUFS) {
            if (debug_handler)
                debug_handler("no buffers available\n");
            return 0;
        }

        if (!(cqe->flags & IORING_CQE_F_BUFFER) || cqe->res < 0) {
            if (debug_handler)
                debug_handler("recv CQE have a bad res: %d\n", cqe->res);
            return -55;
        }
        auto idx = cqe->flags >> 16;

        auto out = io_uring_recvmsg_validate(buffer(idx), cqe->res, &_msg);
        if (!out) {
            if (debug_handler)
                debug_handler("bad recvmsg\n");
            return -2;
        }

        auto payload_len = io_uring_recvmsg_payload_length(out, cqe->res, &_msg);
        if (out->flags & MSG_TRUNC) {
            if (debug_handler)
                debug_handler("truncated msg need %u received %u\n", out->payloadlen, payload_len);
            ring_recycle(idx);
            buf_ring_advance(1);
            return 0;
        }

        ++_metrics.packets_received;

        auto payload = io_uring_recvmsg_payload(out, &_msg);

        printf("received: %lu\n", _metrics.packets_received);

        buf_scope sc{(uint8_t*)payload, payload_len, idx, this};

        ring_recycle(idx);
        buf_ring_advance(1);

        return 0;
    }

    int process_cqe(io_uring_cqe* cqe, int fdidx) {
        if (cqe->user_data == sqe_op_recvmsg)
            return process_cqe_recv(cqe, fdidx);
        return -1;
    }

private:
    unsigned int sq_depth;
    unsigned int batch_size;
    unsigned int buf_shift;
    uint8_t* buf_base;

    io_uring ring = {};
    size_t buf_ring_size;
    io_uring_buf_ring* buf_ring;


    msghdr _msg = {};
    F debug_handler;

    metrics_store _metrics;
};

int main() {
    io_uring_ctx<> ctx(256, 4096, 16);

    auto sockfd = setup_sock(1337);
    if (sockfd == -1) {
        std::cerr << "setup_sock() failed: " << strerror(errno) << std::endl;
        return 1;
    }

    ctx.register_files(&sockfd, 1);
    ctx.run();
}
