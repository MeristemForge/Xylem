use clap::Parser;
use openssl::asn1::Asn1Time;
use openssl::hash::MessageDigest;
use openssl::pkey::PKey;
use openssl::rsa::Rsa;
use openssl::ssl::{SslAcceptor, SslMethod, SslStream};
use openssl::x509::X509;
use std::io::{self, Read, Write};
use std::net::UdpSocket;
use std::sync::Arc;
use std::thread;

struct ConnectedUdp(UdpSocket);

impl Read for ConnectedUdp {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        self.0.recv(buf)
    }
}

impl Write for ConnectedUdp {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        self.0.send(buf)
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

#[derive(Parser)]
struct Args {
    #[arg(default_value_t = 9444)]
    port: u16,
}

fn build_acceptor() -> Arc<SslAcceptor> {
    let rsa = Rsa::generate(2048).unwrap();
    let pkey = PKey::from_rsa(rsa).unwrap();

    let mut x509_builder = X509::builder().unwrap();
    x509_builder.set_version(2).unwrap();
    x509_builder
        .set_not_before(&Asn1Time::days_from_now(0).unwrap())
        .unwrap();
    x509_builder
        .set_not_after(&Asn1Time::days_from_now(365).unwrap())
        .unwrap();
    x509_builder.set_pubkey(&pkey).unwrap();
    x509_builder.sign(&pkey, MessageDigest::sha256()).unwrap();
    let x509 = x509_builder.build();

    let mut builder = SslAcceptor::mozilla_intermediate(SslMethod::dtls()).unwrap();
    builder.set_private_key(&pkey).unwrap();
    builder.set_certificate(&x509).unwrap();

    Arc::new(builder.build())
}

fn handle_client(stream: &mut SslStream<ConnectedUdp>) {
    let mut buf = vec![0u8; 65536];
    loop {
        match stream.ssl_read(&mut buf) {
            Ok(0) => return,
            Ok(n) => {
                if stream.ssl_write(&buf[..n]).is_err() {
                    return;
                }
            }
            Err(_) => return,
        }
    }
}

fn main() {
    let args = Args::parse();
    let addr = format!("0.0.0.0:{}", args.port);

    let acceptor = build_acceptor();

    eprintln!("rust dtls echo server listening on {} (threaded)", addr);

    let socket = UdpSocket::bind(&addr).unwrap();
    let mut buf = vec![0u8; 65536];

    loop {
        let (n, peer) = match socket.recv_from(&mut buf) {
            Ok(r) => r,
            Err(_) => continue,
        };

        let local_socket = UdpSocket::bind("0.0.0.0:0").unwrap();
        local_socket.connect(peer).unwrap();
        local_socket.send(&buf[..n]).ok();

        let acceptor = acceptor.clone();
        thread::spawn(move || {
            match acceptor.accept(ConnectedUdp(local_socket)) {
                Ok(mut stream) => handle_client(&mut stream),
                Err(_) => {}
            }
        });
    }
}
