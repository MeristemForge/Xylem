package main

import (
	"fmt"
	"net"
	"os"
	"runtime"
	"strconv"
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
	port := 9000
	workers := 4

	if len(os.Args) > 1 {
		if p, err := strconv.Atoi(os.Args[1]); err == nil {
			port = p
		}
	}
	if len(os.Args) > 2 {
		if w, err := strconv.Atoi(os.Args[2]); err == nil {
			workers = w
		}
	}

	runtime.GOMAXPROCS(workers)

	addr := fmt.Sprintf("0.0.0.0:%d", port)
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "listen error: %v\n", err)
		os.Exit(1)
	}

	fmt.Fprintf(os.Stderr, "go tcp echo server listening on %s (GOMAXPROCS=%d)\n", addr, workers)

	for {
		conn, err := ln.Accept()
		if err != nil {
			continue
		}
		go handleConn(conn)
	}
}
