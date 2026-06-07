use tokio::net::UdpSocket;

#[tokio::main(flavor = "current_thread")]
async fn main() {
    let port: u16 = std::env::args()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(9001);
    let addr = format!("0.0.0.0:{}", port);
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
