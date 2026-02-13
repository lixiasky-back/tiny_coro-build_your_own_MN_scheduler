package main

import (
	"fmt"
	"net"
	"os"
	"runtime"
)

var rawResponse = []byte(
	"HTTP/1.1 200 OK\r\n" +
		"Content-Type: text/plain\r\n" +
		"Content-Length: 13\r\n" +
		"Connection: keep-alive\r\n" +
		"\r\n" +
		"Hello, World!")

func handleConn(conn net.Conn) {

	defer conn.Close()

	buf := make([]byte, 1024)

	for {

		n, err := conn.Read(buf)
		if err != nil {
			return
		}

		if n > 0 {

			_, err := conn.Write(rawResponse)
			if err != nil {
				return
			}
		}
	}
}

func main() {

	runtime.GOMAXPROCS(runtime.NumCPU())


	ln, err := net.Listen("tcp", "0.0.0.0:8080")
	if err != nil {
		fmt.Printf("Listen failed: %v\n", err)
		os.Exit(1)
	}

	fmt.Printf("🚀 Go Benchmark Server running on :8080 using %d cores\n", runtime.NumCPU())

	for {

		conn, err := ln.Accept()
		if err != nil {
			continue
		}

		go handleConn(conn)
	}
}