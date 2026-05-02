package main

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"flag"
	"fmt"
	"math/big"
	"net"
	"os"
	"runtime"
	"time"

	"github.com/pion/dtls/v2"
)

func generateSelfSignedCert() (*tls.Certificate, error) {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return nil, err
	}

	tmpl := &x509.Certificate{
		SerialNumber: big.NewInt(1),
		Subject:      pkix.Name{CommonName: "localhost"},
		NotBefore:    time.Now(),
		NotAfter:     time.Now().Add(365 * 24 * time.Hour),
		KeyUsage:     x509.KeyUsageDigitalSignature | x509.KeyUsageKeyEncipherment,
	}

	certDER, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
	if err != nil {
		return nil, err
	}

	certPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: certDER})
	keyDER, err := x509.MarshalECPrivateKey(key)
	if err != nil {
		return nil, err
	}
	keyPEM := pem.EncodeToMemory(&pem.Block{Type: "EC PRIVATE KEY", Bytes: keyDER})

	cert, err := tls.X509KeyPair(certPEM, keyPEM)
	if err != nil {
		return nil, err
	}
	return &cert, nil
}

func main() {
	port := flag.Int("port", 9444, "listen port")
	flag.Parse()

	runtime.GOMAXPROCS(1)

	cert, err := generateSelfSignedCert()
	if err != nil {
		fmt.Fprintf(os.Stderr, "generate cert error: %v\n", err)
		os.Exit(1)
	}

	addr, err := net.ResolveUDPAddr("udp", fmt.Sprintf("0.0.0.0:%d", *port))
	if err != nil {
		fmt.Fprintf(os.Stderr, "resolve addr error: %v\n", err)
		os.Exit(1)
	}

	config := &dtls.Config{
		Certificates:       []tls.Certificate{*cert},
		InsecureSkipVerify: true,
	}

	listener, err := dtls.Listen("udp", addr, config)
	if err != nil {
		fmt.Fprintf(os.Stderr, "listen error: %v\n", err)
		os.Exit(1)
	}
	defer listener.Close()

	fmt.Fprintf(os.Stderr, "go dtls echo server listening on 0.0.0.0:%d (GOMAXPROCS=1)\n", *port)

	for {
		conn, err := listener.Accept()
		if err != nil {
			continue
		}
		go func(c net.Conn) {
			defer c.Close()
			buf := make([]byte, 65536)
			for {
				n, err := c.Read(buf)
				if err != nil {
					return
				}
				_, err = c.Write(buf[:n])
				if err != nil {
					return
				}
			}
		}(conn)
	}
}
