package main 

import (
	"fmt"
	"net"
)

const (
	args = 3 
)

type WriterState int

type WriterFSM struct {
	err error
	currentState WriterState
	ip net.IP
	port int
	con net.Conn
	reader 
}

const (
	Initilized WriterState = iota
	ValidateArgs 
    ParseIP 
    CreateSocket 
    HandshakeInit
    Connected
	CloseConnection
	ErrorHandling
	FatalError
	Termination
)

func NewWriterFSM() *WriterFSM {
	return &WriterFSM{currentState: Initilized}
}