package main

import (
	"flag"
	"fmt"
	"net"
	"os"
	"runtime"
)

func handleConn(conn net.Conn) {
	defer conn.Close()
	buf := make([]byte, 65536)
	for {
		n, err := conn.Read(buf)
		if err != nil {
			return
		}
		_, err = conn.Write(buf[:n])
		if err != nil {
			return
		}
	}
}

func main() {
	port := flag.Int("port", 9000, "listen port")
	flag.Parse()

	if flag.NArg() > 0 {
		if p, err := fmt.Sscanf(flag.Arg(0), "%d", port); p == 1 && err == nil {
		}
	}

	runtime.GOMAXPROCS(1)

	addr := fmt.Sprintf("0.0.0.0:%d", *port)
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "listen error: %v\n", err)
		os.Exit(1)
	}

	fmt.Fprintf(os.Stderr, "go tcp echo server listening on %s (GOMAXPROCS=1)\n", addr)

	for {
		conn, err := ln.Accept()
		if err != nil {
			continue
		}
		go handleConn(conn)
	}
}
