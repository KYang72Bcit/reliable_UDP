package main

import (
	"bufio"
	"net"
	"os"
	"errors"
	"strconv"
)


/**
	* 00000001 - ACK
	* 00000010 - SYN
	* 00000100 - FIN
	* 00001000 - DATA
**/

const (
	FLAG_ACK = 1 << iota
	FLAG_SYN
	FLAG_FIN
	FLAG_DATA
)

const (
	args = 3
	maxRetries = 5
	bufferSize = 512
)

//////////////////define custom packet structure//////////////////////
type CustomPacket struct {
	Header Header  `json:"header"`
	Data string    `json:"data"`
}


type Header struct {
	SeqNum uint32 `json:"seqNum"`
	AckNum uint32  `json:"ackNum"`
	Flags byte     `json:"flags"`
}

/////////////////////////define Receiver FSM///////////////////////////

type ReceiverState int

const (
	ValidateArgs ReceiverState = iota
	CreateSocket
	HandshakeInit
	ReadPacket
	SendPacket
	ResendPacket
	HandleError
	FatalError
	Termination
)

type ReceiverFSM struct {
	err error
	ip net.IP
	port int
	udpcon net.Conn
	seqNum uint32
	ackNum uint32
}

////////////////////////define Receiver Functions //////////////////////
func NewReceiverFSM() *ReceiverFSM {

	return &ReceiverFSM{}
}

func (fsm *ReceiverFSM) ValidateArgsState() ReceiverState {
	if (len(os.Args) != args) {

		fsm.err = errors.New("invalid number of arguments, <ip> <port>")
		return FatalError
	}
	fsm.ip, fsm.err = validateIP(os.Args[1])
	if fsm.err != nil {
		return FatalError
	}
	fsm.port, fsm.err = validatePort(os.Args[2])
	if fsm.err != nil {
		return FatalError
	}
	return CreateSocket
}

func (fsm *ReceiverFSM) CreateSocketState() ReceiverState {
	addr := &net.UDPAddr{IP: fsm.ip, Port: fsm.port}
	fsm.udpcon, fsm.err = net.ListenUDP("udp", addr)
	if fsm.err != nil {
		return FatalError
	}
	return HandshakeInit
}

func (fsm *ReceiverFSM) HandshakeInitState() ReceiverState {

}



//////////////////////////define helper functions//////////////////////


func validateIP(ip string) (net.IP, error){
	addr := net.ParseIP(ip)
	if addr == nil {
		return nil, errors.New("invalid ip address")
	}
	return addr, nil
}

func validatePort(port string) (int, error) {
	portNo, err := strconv.Atoi(port)
	if err != nil || portNo < 0 || portNo > 65535 {
		return -1, errors.New("invalid port number")
	}
	return portNo, nil
}