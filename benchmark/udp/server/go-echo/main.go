package main

import (
	"flag"
	"fmt"
	"net"
	"os"
	"runtime"
)

func main() {
	port := flag.Int("port", 9001, "listen port")
	flag.Parse()

	runtime.GOMAXPROCS(1)

	addr := fmt.Sprintf("0.0.0.0:%d", *port)
	conn, err := net.ListenPacket("udp", addr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "listen error: %v\n", err)
		os.Exit(1)
	}
	defer conn.Close()

	fmt.Fprintf(os.Stderr, "go udp echo server listening on %s (GOMAXPROCS=1)\n", addr)

	buf := make([]byte, 65536)
	for {
		n, remoteAddr, err := conn.ReadFrom(buf)
		if err != nil {
			continue
		}
		conn.WriteTo(buf[:n], remoteAddr)
	}
}
