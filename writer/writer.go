package main

import (
	"bufio"
	"errors"
	"net"
	"os"
	"encoding/json"
	"time"
	"io"
	"strconv"
)

const (
	args = 3 
	maxRetries = 5
	bufferSize = 512
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

type WriterState int

type CustomPacket struct {
	header Header
	data string
}


type Header struct {
	SeqNum uint32
	AckNum uint32
	Flags byte
}

type WriterFSM struct {
	err error
	currentState WriterState
	ip net.IP
	port int
	maxRetries int
	udpcon net.Conn
	stdinReader *bufio.Reader 
	signalchan chan struct{} //channel for CtrlC signal handling
	responseChan chan []byte //channel for response handling
	inputChan chan []byte //channel for input handling
	errorChan chan error //channel for error handling
	resendChan chan struct{} //channel for resend handling
	ack uint32
	seq uint32
	data string
}


const (
	Initilized WriterState = iota
	ValidateArgs 
    CreateSocket 
    HandshakeInit
	ResendPacket
    Connected
	CloseConnection
	ErrorHandling
	FatalError
	Termination
)


func NewWriterFSM() *WriterFSM {
	return &WriterFSM{
		currentState: Initilized,
		maxRetries: maxRetries,
		stdinReader: bufio.NewReader(os.Stdin),
		responseChan: make(chan []byte),
		inputChan: make(chan []byte),
		errorChan: make(chan error),
		resendChan: make(chan struct{}),
		signalchan: make(chan struct{}),
		ack: 0,
		seq: 0,
		data: "",
	}
}


func (fsm *WriterFSM) ValidateArgsState() WriterState {
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


func (fsm *WriterFSM) CreateSocketState() WriterState {
	addr := &net.UDPAddr{IP: fsm.ip, Port: fsm.port}
	fsm.udpcon, fsm.err = net.DialUDP("udp", nil, addr)
	if fsm.err != nil {
		return FatalError
	}
	return HandshakeInit
}



func (fsm *WriterFSM) HandshakeInitState() WriterState {

	go fsm.lisenResponse()

	_, err := sendPacket(fsm.ack, fsm.seq, FLAG_SYN, fsm.data, fsm.udpcon)
	if err != nil {
		fsm.err = err
		return FatalError
	}
	timeout := time.NewTimer(2 * time.Second)

	select {
		case responseData := <- fsm.responseChan:
			if ValidPacket(responseData, FLAG_ACK, fsm.seq) {
				sendPacket(fsm.ack,fsm.seq, FLAG_ACK, fsm.data, fsm.udpcon)
				return Connected
			}
		case <- timeout.C:
			return ResendPacket
	}
	fsm.err = errors.New("Connection Error")
	return FatalError
}


func (fsm *WriterFSM) ResendPacketState(seq uint32, ack uint32, flags byte, data string, nextState WriterState) WriterState {

	for i := 0; i < fsm.maxRetries; i++ {
		_, err := sendPacket(seq, ack, flags, data, fsm.udpcon)
		if err != nil {
			fsm.err = err
			return FatalError
		}
		timeout := time.NewTimer(2 * time.Second)
		select {
			case responseData := <- fsm.responseChan:
				if ValidPacket(responseData, FLAG_ACK, fsm.seq) {
					return nextState
				}
			case <- timeout.C:
				continue
			
		}
	}

	fsm.err = errors.New("Connection Error")
	return FatalError	
}


func (fsm *WriterFSM) ConnectedState() WriterState {
	go fsm.readStdin()
	go fsm.sendPacket()
	for {
		select{
			case <- fsm.signalchan:
				return CloseConnection
			case err := <- fsm.errorChan:
				fsm.err = err
				return ErrorHandling
			case <-fsm.resendChan:
				return ResendPacket	
			
		}
	}
}


func (fsm *WriterFSM) readStdin() {
	inputBuffer := make([]byte, bufferSize)
	for {
		n, err := fsm.stdinReader.Read(inputBuffer)
		if err != nil {
			if err == io.EOF {
				fsm.signalchan <- struct{}{}
				break
			}
			fsm.errorChan <- err
			return	
		}
		fsm.inputChan <- inputBuffer[:n]

	}
}

func (fsm *WriterFSM) sendPacket() {

	for {
		timer := time.NewTimer(2 * time.Second)
		select {
			case input := <- fsm.inputChan:
				_, err := sendPacket(fsm.ack, fsm.seq, FLAG_DATA, string(input), fsm.udpcon)
				if err != nil {
					fsm.errorChan <- err
					return
				}

			case response := <- fsm.responseChan:
				if ValidPacket(response, FLAG_ACK, fsm.seq) {
					continue
				}

			case  <- timer.C:
				fsm.resendChan <- struct{}{}
				continue
			
		}

	}
}

func (fsm *WriterFSM) lisenResponse() {
	
		for {
			buffer := make ([]byte, bufferSize)
			n, err := fsm.udpcon.Read(buffer)
			if err != nil {
				fsm.err = err
				return
			}
			fsm.responseChan <- buffer[:n]		
	}
}

func (fsm *WriterFSM) CloseConnectionState() WriterState {

	_, err := sendPacket(fsm.ack, fsm.seq, FLAG_FIN, "", fsm.udpcon)
	if err != nil {
		fsm.err = err
		return FatalError
	}

	timeout := time.NewTimer(2 * time.Second)

	select {
		case responseData := <- fsm.responseChan:
			if ValidPacket(responseData, FLAG_FIN, fsm.seq) {
				sendPacket(fsm.ack,fsm.seq, FLAG_ACK, "", fsm.udpcon)
				return Termination
			}
		case <- timeout.C:
			return ResendPacket
	}
	fsm.err = errors.New("Connection Error")
	return FatalError

}

func (fsm *WriterFSM)TerminateState() WriterState {
	fsm.udpcon.Close()
	

}


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

func createPacket(ack uint32, seq uint32, flags byte, data string) ([]byte, error) {
	packet := CustomPacket{
		header: Header{
			SeqNum: seq,
			AckNum: ack,
			Flags: flags,
		},
		data: data,

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

func ValidPacket(response []byte, flags byte, seq uint32) bool {
	return true 
}

//////////////////////////////INPUT FSM///////////////////////////////////////



func main() {
	
}

