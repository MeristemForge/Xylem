#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#define CERT_FILE "bench-cert.pem"
#define KEY_FILE  "bench-key.pem"

using boost::asio::ip::tcp;
namespace ssl = boost::asio::ssl;

static int write_pem_to_file(const char* path,
                             int (*write_fn)(BIO*, void*),
                             void* obj) {
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio) return -1;
    if (write_fn(bio, obj) != 1) {
        BIO_free(bio);
        return -1;
    }
    char* data = nullptr;
    long  len  = BIO_get_mem_data(bio, &data);
    FILE* f    = std::fopen(path, "wb");
    if (!f) {
        BIO_free(bio);
        return -1;
    }
    std::fwrite(data, 1, static_cast<size_t>(len), f);
    std::fclose(f);
    BIO_free(bio);
    return 0;
}

static int write_cert_bio(BIO* bio, void* obj) {
    return PEM_write_bio_X509(bio, static_cast<X509*>(obj));
}

static int write_key_bio(BIO* bio, void* obj) {
    return PEM_write_bio_PrivateKey(bio, static_cast<EVP_PKEY*>(obj),
                                    nullptr, nullptr, 0, nullptr, nullptr);
}

static void ensure_cert() {
    FILE* f = std::fopen(CERT_FILE, "r");
    if (f) {
        std::fclose(f);
        return;
    }

    std::fprintf(stderr, "generating self-signed certificate...\n");

    EVP_PKEY* pkey = EVP_PKEY_new();
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    EVP_PKEY_keygen_init(pctx);
    EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048);
    EVP_PKEY_keygen(pctx, &pkey);
    EVP_PKEY_CTX_free(pctx);

    X509* x509 = X509_new();
    X509_set_version(x509, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 365 * 24 * 3600);
    X509_set_pubkey(x509, pkey);

    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               (const unsigned char*)"localhost", -1, -1, 0);
    X509_set_issuer_name(x509, name);
    X509_sign(x509, pkey, EVP_sha256());

    write_pem_to_file(CERT_FILE, write_cert_bio, x509);
    write_pem_to_file(KEY_FILE, write_key_bio, pkey);

    X509_free(x509);
    EVP_PKEY_free(pkey);
}

class session : public std::enable_shared_from_this<session> {
public:
    explicit session(ssl::stream<tcp::socket> socket)
        : socket_(std::move(socket)) {}

    void start() {
        auto self = shared_from_this();
        socket_.async_handshake(
            ssl::stream_base::server,
            [this, self](boost::system::error_code ec) {
                if (!ec) do_read();
            });
    }

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

    ssl::stream<tcp::socket> socket_;
    char buf_[65536];
};

class server {
public:
    server(boost::asio::io_context& io, ssl::context& ctx, unsigned short port)
        : acceptor_(io, tcp::endpoint(tcp::v4(), port)), ctx_(ctx) {
        do_accept();
    }

private:
    void do_accept() {
        acceptor_.async_accept([this](boost::system::error_code ec,
                                      tcp::socket socket) {
            if (!ec) {
                socket.set_option(tcp::no_delay(true));
                std::make_shared<session>(
                    ssl::stream<tcp::socket>(std::move(socket), ctx_))->start();
            }
            do_accept();
        });
    }

    tcp::acceptor acceptor_;
    ssl::context& ctx_;
};

int main(int argc, char** argv) {
    int port = 9443;
    if (argc > 1) port = std::atoi(argv[1]);

    ensure_cert();

    boost::asio::io_context io;
    ssl::context ctx(ssl::context::tls_server);
    SSL_CTX_use_certificate_chain_file(ctx.native_handle(), CERT_FILE);
    SSL_CTX_use_PrivateKey_file(ctx.native_handle(), KEY_FILE, SSL_FILETYPE_PEM);

    server s(io, ctx, static_cast<unsigned short>(port));

    std::fprintf(stderr, "boost.asio+openssl tls echo server listening on 0.0.0.0:%d\n", port);
    io.run();
    return 0;
}
