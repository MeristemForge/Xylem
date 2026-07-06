package main

import (
	"net"
	"testing"
	"time"
)

func TestEstablishTCPConnectionsRetriesUntilTargetCount(t *testing.T) {
	probe, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen probe: %v", err)
	}
	addr := probe.Addr().String()
	if err := probe.Close(); err != nil {
		t.Fatalf("close probe: %v", err)
	}

	const target = 3
	accepted := make(chan net.Conn, target)
	done := make(chan struct{})
	go func() {
		defer close(done)
		time.Sleep(100 * time.Millisecond)
		ln, err := net.Listen("tcp", addr)
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
			accepted <- c
		}
	}()

	conns := establishTCPConnections(addr, target)
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
