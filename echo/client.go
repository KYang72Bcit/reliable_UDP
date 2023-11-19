package main

import (
  "bufio"
  "fmt"
  "log"
  "net"
  "os"
  "sync"
  
)



func main() {
  conn, err := net.Dial("tcp", "localhost:6666")
  if err != nil {
    log.Fatalln(err)
  }

  defer conn.Close()

  stdinScanner := bufio.NewScanner(os.Stdin)
  connScanner := bufio.NewScanner(conn)

  var wg sync.WaitGroup
  wg.Add(2)

  // Thread for sending message to the port from stdin
  go func() {
    defer wg.Done()
    for {
      fmt.Print("> ")
      if !stdinScanner.Scan() {
        break
      }
      fmt.Fprintf(conn, "%s\n", stdinScanner.Text())
    }
  }()

  // Thread for receiving message and print it out in std out
  go func() {
    defer wg.Done()
    for {
      if !connScanner.Scan() {
        break
      }
      fmt.Println(connScanner.Text())
    }
  }()

  wg.Wait()


}
