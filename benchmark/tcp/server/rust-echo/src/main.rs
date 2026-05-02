use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpSocket;

#[tokio::main(flavor = "current_thread")]
async fn main() {
    let port: u16 = std::env::args()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(9000);

    let addr: std::net::SocketAddr = format!("0.0.0.0:{}", port).parse().unwrap();

    let socket = TcpSocket::new_v4().unwrap();
    socket.set_reuseaddr(true).unwrap();
    socket.bind(addr).unwrap();
    let listener = socket.listen(4096).unwrap();

    eprintln!("rust tcp echo server listening on {} (single-thread)", addr);

    loop {
        let (mut socket, _) = match listener.accept().await {
            Ok(s) => s,
            Err(_) => continue,
        };

        tokio::spawn(async move {
            socket.set_nodelay(true).ok();
            let mut buf = vec![0u8; 65536];
            loop {
                let n = match socket.read(&mut buf).await {
                    Ok(0) => return,
                    Ok(n) => n,
                    Err(_) => return,
                };
                if socket.write_all(&buf[..n]).await.is_err() {
                    return;
                }
            }
        });
    }
}
