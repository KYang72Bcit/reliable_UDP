package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"os"
	"strconv"
	"time"
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
	Connected
	SendPacket
	ResendPacket
	HandleError
	FatalError
	Termination
)

type ReceiverFSM struct {
	currentState ReceiverState
	err error
	ip net.IP
	port int
	udpcon net.Conn
	seqNum uint32
	ackNum uint32
	stopChan chan struct{}
	errorChan chan error
	responseChan chan []byte
	resnedChan chan struct{}
	isResponseListened bool
	timeoutDuration time.Duration
}

////////////////////////define Receiver Functions //////////////////////
func NewReceiverFSM() *ReceiverFSM {

	return &ReceiverFSM{
		currentState: ValidateArgs,
		seqNum: 0,
		ackNum: 0,
		timeoutDuration: 2 * time.Second,
		stopChan: make(chan struct{}),
		errorChan: make(chan error),
		responseChan: make(chan []byte),
		resnedChan: make(chan struct{}),
		isResponseListened: false,
	}
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
	fmt.Println("UDP server listening on", fsm.udpcon.LocalAddr().String())
	return HandshakeInit
}

func (fsm *ReceiverFSM) HandshakeInitState() ReceiverState {
	if !fsm.isResponseListened  {
		go fsm.listenResponse()
		fsm.isResponseListened = true
	}

	for {
		var timeout *time.Timer
		select {
			case response := <- fsm.responseChan:
				if isValidPacket(response, fsm.ackNum, fsm.seqNum, FLAG_SYN) {
					sendPacket(fsm.ackNum, fsm.seqNum, FLAG_SYN | FLAG_ACK, "", fsm.udpcon)
					timeout = time.NewTimer(fsm.timeoutDuration)
					}
				if isValidPacket(response, fsm.ackNum, fsm.seqNum, FLAG_ACK) {
					return Connected
					}
			case <- timeout.C:
				return ResendPacket

			case <- fsm.stopChan:
				return Termination

		}

	}

}

func (fsm *ReceiverFSM) ConnectedState() ReceiverState {

}


func (fsm *ReceiverFSM) listenResponse() {
	fsm.udpcon.SetReadDeadline(time.Now().Add(fsm.timeoutDuration))

		for {
			select {
				case <- fsm.stopChan:
					return
				default:
					buffer := make ([]byte, bufferSize)
					n, err := fsm.udpcon.Read(buffer)
					if err != nil {
						fsm.errorChan <- err
						return
					}
					fsm.responseChan <- buffer[:n]
				}
			}
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

func isValidPacket(response []byte , ackNum uint32, seqNum uint32, FLAG byte) bool {
	header, err := parsePacket(response)
	if err != nil {
		return false
	}
	return header.AckNum == ackNum &&
	header.SeqNum == seqNum &&
	header.Flags == FLAG

}

func parsePacket(response []byte) (*Header, error) {
	var packet CustomPacket
	err := json.Unmarshal(response, &packet)
	if err != nil {
		return &Header{}, err
	}
	return &packet.Header, nil
}

func createPacket(ack uint32, seq uint32, flags byte, data string) ([]byte, error) {
	packet := CustomPacket{
		Header: Header{
			SeqNum: seq,
			AckNum: ack,
			Flags: flags,
		},
		Data: data,

	}
	return json.Marshal(packet)
}

func sendPacket(ack uint32, seq uint32, flags byte, data string, udpcon net.Conn) (int, error) {
	packet, err := createPacket(ack, seq, flags, data)
	if err != nil {
		return -1, err
	}
		_, err = udpcon.Write(packet)

	return len(packet), err

}
