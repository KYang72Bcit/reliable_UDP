package main

import (
  "bufio"
  "fmt"
  "log"
  "net"
  "strings"
  
)

func handle(conn net.Conn) {
  defer conn.Close()

  s := bufio.NewScanner(conn)
  for s.Scan() {
    fmt.Fprintf(conn, "%s\n", strings.ToUpper(s.Text()))
  }
}

func main() {
  ln, err := net.Listen("tcp", ":6666")
  if err != nil {
    log.Fatalln(err)
  }

  for {
    conn, err := ln.Accept()
    if err != nil {
      log.Fatalln(err)
    }

    go handle(conn)
  }
}
