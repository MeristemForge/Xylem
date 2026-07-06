package main

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"math/big"
	"net"
	"testing"
	"time"
)

func testTLSConfig(t *testing.T) *tls.Config {
	t.Helper()

	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatalf("generate key: %v", err)
	}
	tmpl := x509.Certificate{
		SerialNumber: big.NewInt(1),
		Subject:      pkix.Name{CommonName: "localhost"},
		NotBefore:    time.Now().Add(-time.Hour),
		NotAfter:     time.Now().Add(time.Hour),
		KeyUsage:     x509.KeyUsageDigitalSignature,
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		DNSNames:     []string{"localhost"},
		IPAddresses:  []net.IP{net.ParseIP("127.0.0.1")},
	}
	der, err := x509.CreateCertificate(rand.Reader, &tmpl, &tmpl, &key.PublicKey, key)
	if err != nil {
		t.Fatalf("create cert: %v", err)
	}
	return &tls.Config{
		Certificates: []tls.Certificate{{
			Certificate: [][]byte{der},
			PrivateKey:  key,
		}},
	}
}

func TestEstablishTLSConnectionsRetriesUntilTargetCount(t *testing.T) {
	probe, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen probe: %v", err)
	}
	addr := probe.Addr().String()
	if err := probe.Close(); err != nil {
		t.Fatalf("close probe: %v", err)
	}

	const target = 3
	accepted := make(chan *tls.Conn, target)
	done := make(chan struct{})
	go func() {
		defer close(done)
		time.Sleep(100 * time.Millisecond)
		ln, err := tls.Listen("tcp", addr, testTLSConfig(t))
		if err != nil {
			t.Errorf("listen delayed: %v", err)
			return
		}
		defer ln.Close()
		for i := 0; i < target; i++ {
			c, err := ln.Accept()
			if err != nil {
				t.Errorf("accept: %v", err)
				return
			}
			tc := c.(*tls.Conn)
			if err := tc.Handshake(); err != nil {
				t.Errorf("handshake: %v", err)
				_ = tc.Close()
				return
			}
			accepted <- tc
		}
	}()

	conns := establishTLSConnections(addr, target)
	defer func() {
		for _, c := range conns {
			_ = c.Close()
		}
		for i := 0; i < target; i++ {
			_ = (<-accepted).Close()
		}
		<-done
	}()

	if len(conns) != target {
		t.Fatalf("got %d connections, want %d", len(conns), target)
	}
}
