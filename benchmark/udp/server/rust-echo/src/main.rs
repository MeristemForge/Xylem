use clap::Parser;
use tokio::net::UdpSocket;

#[derive(Parser)]
struct Args {
    #[arg(default_value_t = 9001)]
    port: u16,
}

#[tokio::main(flavor = "current_thread")]
async fn main() {
    let args = Args::parse();
    let addr = format!("0.0.0.0:{}", args.port);
    let socket = UdpSocket::bind(&addr).await.unwrap();

    eprintln!("rust udp echo server listening on {} (single-thread)", addr);

    let mut buf = vec![0u8; 65536];
    loop {
        let (n, peer) = match socket.recv_from(&mut buf).await {
            Ok(r) => r,
            Err(_) => continue,
        };
        let _ = socket.send_to(&buf[..n], peer).await;
    }
}
