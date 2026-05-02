use clap::Parser;
use rcgen::{generate_simple_self_signed, CertifiedKey};
use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer};
use rustls::ServerConfig;
use std::sync::Arc;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpListener;
use tokio_rustls::TlsAcceptor;

#[derive(Parser)]
struct Args {
    #[arg(default_value_t = 9443)]
    port: u16,
}

fn generate_config() -> Arc<ServerConfig> {
    let subject_alt_names = vec!["localhost".to_string()];
    let CertifiedKey { cert, key_pair } =
        generate_simple_self_signed(subject_alt_names).unwrap();

    let cert_der = CertificateDer::from(cert.der().to_vec());
    let key_der = PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(
        key_pair.serialize_der(),
    ));

    let config = ServerConfig::builder()
        .with_no_client_auth()
        .with_single_cert(vec![cert_der], key_der)
        .unwrap();

    Arc::new(config)
}

#[tokio::main(flavor = "current_thread")]
async fn main() {
    let args = Args::parse();
    let config = generate_config();
    let acceptor = TlsAcceptor::from(config);

    let addr = format!("0.0.0.0:{}", args.port);
    let listener = TcpListener::bind(&addr).await.unwrap();

    eprintln!("rust tls echo server listening on {} (single-thread)", addr);

    loop {
        let (stream, _) = match listener.accept().await {
            Ok(s) => s,
            Err(_) => continue,
        };

        let acceptor = acceptor.clone();
        tokio::spawn(async move {
            let mut tls_stream = match acceptor.accept(stream).await {
                Ok(s) => s,
                Err(_) => return,
            };
            let mut buf = vec![0u8; 65536];
            loop {
                let n = match tls_stream.read(&mut buf).await {
                    Ok(0) => return,
                    Ok(n) => n,
                    Err(_) => return,
                };
                if tls_stream.write_all(&buf[..n]).await.is_err() {
                    return;
                }
            }
        });
    }
}
