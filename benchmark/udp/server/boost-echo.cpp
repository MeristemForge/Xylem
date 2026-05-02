#include <boost/asio.hpp>
#include <cstdio>
#include <cstdlib>

using boost::asio::ip::udp;

class server {
public:
    server(boost::asio::io_context& io, unsigned short port)
        : socket_(io, udp::endpoint(udp::v4(), port)) {
        do_receive();
    }

private:
    void do_receive() {
        socket_.async_receive_from(
            boost::asio::buffer(buf_, sizeof(buf_)), remote_,
            [this](boost::system::error_code ec, std::size_t n) {
                if (!ec && n > 0) {
                    do_send(n);
                } else {
                    do_receive();
                }
            });
    }

    void do_send(std::size_t n) {
        socket_.async_send_to(
            boost::asio::buffer(buf_, n), remote_,
            [this](boost::system::error_code, std::size_t) {
                do_receive();
            });
    }

    udp::socket   socket_;
    udp::endpoint remote_;
    char          buf_[65536];
};

int main(int argc, char** argv) {
    int port = 9001;
    if (argc > 1) port = std::atoi(argv[1]);

    boost::asio::io_context io;
    server s(io, static_cast<unsigned short>(port));

    std::fprintf(stderr, "boost.asio udp echo server listening on 0.0.0.0:%d\n", port);
    io.run();
    return 0;
}
