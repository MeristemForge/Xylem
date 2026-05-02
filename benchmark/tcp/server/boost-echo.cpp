#include <boost/asio.hpp>
#include <cstdio>
#include <cstdlib>
#include <memory>

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
        : acceptor_(io, tcp::endpoint(tcp::v4(), port)) {
        acceptor_.listen(4096);
        do_accept();
    }

private:
    void do_accept() {
        acceptor_.async_accept([this](boost::system::error_code ec,
                                      tcp::socket socket) {
            if (!ec) {
                socket.set_option(tcp::no_delay(true));
                std::make_shared<session>(std::move(socket))->start();
            }
            do_accept();
        });
    }

    tcp::acceptor acceptor_;
};

int main(int argc, char** argv) {
    int port = 9000;
    if (argc > 1) port = std::atoi(argv[1]);

    boost::asio::io_context io;
    server s(io, static_cast<unsigned short>(port));

    std::fprintf(stderr, "boost.asio tcp echo server listening on 0.0.0.0:%d\n", port);
    io.run();
    return 0;
}
