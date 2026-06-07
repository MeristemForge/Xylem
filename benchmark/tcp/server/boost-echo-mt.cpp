/**
 * Boost.Asio multi-threaded TCP echo server.
 *
 * Model: N independent std::threads, each owns its own
 * boost::asio::io_context and an acceptor whose underlying socket has
 * SO_REUSEPORT. Matches the "one io_context per core" pattern that
 * Asio documents for maximum throughput on Linux.
 *
 * Fairness: TCP_NODELAY on accepted sockets, backlog 4096, same 64 KB
 * read buffer as the single-threaded variant.
 */
#include <boost/asio.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

using boost::asio::ip::tcp;

class session : public std::enable_shared_from_this<session> {
public:
    explicit session(tcp::socket socket) : socket_(std::move(socket)) {}
    void start() { do_read(); }
private:
    void do_read() {
        auto self = shared_from_this();
        socket_.async_read_some(
            boost::asio::buffer(buf_, sizeof(buf_)),
            [this, self](boost::system::error_code ec, std::size_t n) {
                if (!ec) do_write(n);
            });
    }
    void do_write(std::size_t n) {
        auto self = shared_from_this();
        boost::asio::async_write(
            socket_, boost::asio::buffer(buf_, n),
            [this, self](boost::system::error_code ec, std::size_t) {
                if (!ec) do_read();
            });
    }
    tcp::socket socket_;
    char buf_[65536];
};

class server {
public:
    server(boost::asio::io_context& io, unsigned short port)
        : acceptor_(io) {
        tcp::endpoint ep(tcp::v4(), port);
        acceptor_.open(ep.protocol());
        acceptor_.set_option(boost::asio::socket_base::reuse_address(true));

        /* Boost.Asio has no typed option for SO_REUSEPORT, use native level. */
#ifdef SO_REUSEPORT
        int yes = 1;
        ::setsockopt(acceptor_.native_handle(), SOL_SOCKET, SO_REUSEPORT,
                     &yes, sizeof(yes));
#endif
        acceptor_.bind(ep);
        acceptor_.listen(4096);
        do_accept();
    }
private:
    void do_accept() {
        acceptor_.async_accept(
            [this](boost::system::error_code ec, tcp::socket socket) {
                if (!ec) {
                    socket.set_option(tcp::no_delay(true));
                    std::make_shared<session>(std::move(socket))->start();
                }
                do_accept();
            });
    }
    tcp::acceptor acceptor_;
};

static void worker_main(unsigned short port) {
    boost::asio::io_context io;
    server s(io, port);
    io.run();
}

int main(int argc, char** argv) {
    int port    = 9000;
    int workers = 4;
    if (argc > 1) port    = std::atoi(argv[1]);
    if (argc > 2) workers = std::atoi(argv[2]);

    std::fprintf(stderr, "boost-mt tcp echo: port=%d workers=%d\n",
                 port, workers);

    std::vector<std::thread> ts;
    ts.reserve(static_cast<size_t>(workers));
    for (int i = 0; i < workers; i++) {
        ts.emplace_back(worker_main, static_cast<unsigned short>(port));
    }
    for (auto& t : ts) t.join();
    return 0;
}
