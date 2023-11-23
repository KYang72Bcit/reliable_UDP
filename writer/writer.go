package main

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"strconv"
	"time"
	"sync"
)

const (
	args = 3
	maxRetries = 5
	bufferSize = 1024 * 64
	packetBufferSize = 50
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

/////////////////////////define writer FSM///////////////////////////
type WriterState int
type WriterFSM struct {
	err error
	currentState WriterState
	ip net.IP
	port int
	maxRetries int
	udpcon *net.UDPConn
	stdinReader *bufio.Reader
	EOFchan chan struct{} //channel for EOF signal handling
	responseChan chan []byte //channel for response handling
	inputChan chan CustomPacket //channel for input handling
	errorChan chan error //channel for error handling between go routines
	resendChan chan struct{} //channel for resend handling
	stopChan chan struct{} //channel for notifying go routines to stop
	ackByResend chan bool //channel for ack handling by resend
	ack uint32
	seq uint32
	data string
	timeoutDuration time.Duration
	lastPacketMutex sync.Mutex
	lastPacket []byte
}

const (
	ValidateArgs WriterState = iota
	CreateSocket
	ReadyForTransmitting
	Transmitting
	ErrorHandling
	FatalError
	Termination
)

/////////////////////define Methods for WriterFSM for state transitions/////////////////////////
func NewWriterFSM() *WriterFSM {
	return &WriterFSM{
		currentState: ValidateArgs,
		maxRetries: maxRetries,
		stdinReader: bufio.NewReader(os.Stdin),
		responseChan: make(chan []byte),
		inputChan: make(chan CustomPacket, packetBufferSize),
		errorChan: make(chan error),
		resendChan: make(chan struct{}),
		EOFchan: make(chan struct{}),
		stopChan: make(chan struct{}),
		ack: 0,
		seq: 0,
		data: "",
		ackByResend: make(chan bool),
		timeoutDuration: 2 * time.Second,
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
	return ReadyForTransmitting
}

func (fsm *WriterFSM) ReadyForTransmittingState() WriterState {
	go fsm.readStdin()
	go fsm.listenResponse()
	go fsm.sendPacket()
	return Transmitting
}
/////////////////////////////////////////////Transmitting State////////////////////////////////////////

func (fsm *WriterFSM) TransmittingState() WriterState {
	for {
		select {
			case <- fsm.EOFchan:
				return Termination
			case <- fsm.resendChan:
				go fsm.resendPacket()
			case <- fsm.errorChan:
				return ErrorHandling
				
		}
	}
}




func (fsm *WriterFSM) ErrorHandlingState() WriterState {
		fmt.Println("Error:", fsm.err)
		fsm.stopChan <- struct{}{}
		return ReadyForTransmitting
	}



func (fsm *WriterFSM) FatalErrorState() WriterState {
	fmt.Println("Fatal Error:", fsm.err)
	return Termination
}



func (fsm *WriterFSM)TerminateState() {
	fsm.stopChan <- struct{}{}
	fsm.udpcon.Close()
	fmt.Println("Client Exiting...")
}

/////////////////////////////run function for WriterFSM////////////////////////////
func (fsm *WriterFSM) Run() {
	for {
		 select{
		 case  err := <-fsm.errorChan:
			  fsm.err = err
			  fsm.currentState = ErrorHandling

		 default:
			switch fsm.currentState {
			case ValidateArgs:
				fsm.currentState = fsm.ValidateArgsState()
			case CreateSocket:
				fsm.currentState = fsm.CreateSocketState()
			case ReadyForTransmitting:
				fsm.currentState = fsm.TransmittingState()
			case Transmitting:
				fsm.currentState = fsm.TransmittingState()
			case ErrorHandling:
				fsm.currentState = fsm.ErrorHandlingState()
			case FatalError:
				fsm.currentState = fsm.FatalErrorState()
			case Termination:
				fsm.TerminateState()
			}
		 }

	}
}

/////////////////////////go routines for FSM////////////////////////////

func (fsm *WriterFSM) readStdin() {


		for {
			inputBuffer := make([]byte, bufferSize)
			n, err := fsm.stdinReader.Read(inputBuffer)
			if n > 0 {
				fsm.seq += uint32(n)
				packet := createPacket(fsm.ack, fsm.seq, FLAG_DATA, string(inputBuffer[:n]))
				fsm.inputChan <- packet
			}
			if err != nil {
				if err == io.EOF {
					fsm.EOFchan <- struct{}{}
					return
				} 
				fsm.errorChan <- err
				return
			}
			select {
				case <- fsm.stopChan:
					return
			}
		}
	}


func (fsm *WriterFSM) sendPacket() {

	for rawPacket := range fsm.inputChan {
		packet, err := json.Marshal(rawPacket)
		if err != nil {
			fsm.errorChan <- err
			return
		}
		_, err = fsm.udpcon.Write(packet)
		if err != nil {
			fsm.errorChan <- err
			return
		}
		isValidAck := false
		for !isValidAck {
			select {
				case responseData := <- fsm.responseChan:
					if validPacket(responseData, FLAG_ACK, fsm.seq) {
						isValidAck = true
					} else {
						fsm.resendChan <- struct{}{}
						fsm.lastPacketMutex.Lock()
						fsm.lastPacket = packet
						fsm.lastPacketMutex.Unlock()
						isValidAck = <-fsm.ackByResend
					}
				case <- time.After(fsm.timeoutDuration):
					fsm.resendChan <- struct{}{}
					fsm.lastPacket = packet
					isValidAck = <-fsm.ackByResend

				case <- fsm.stopChan:
					return
			}

		}
		
	}
}


func (fsm *WriterFSM) listenResponse() {

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

	func (fsm *WriterFSM) resendPacket() {
		for  i := 0; i < fsm.maxRetries; i++ {
			fsm.lastPacketMutex.Lock()
			copyPacket := make([]byte, len(fsm.lastPacket))
			copy(fsm.lastPacket, copyPacket)
			fsm.lastPacketMutex.Unlock()
			_, err := fsm.udpcon.Write(copyPacket)
			if err != nil {
				fsm.errorChan <- err
			}
			select {
				case <- fsm.stopChan:
					return
				case response := <- fsm.responseChan:
					if validPacket(response, FLAG_ACK, fsm.seq) {
						fsm.ackByResend <- true
						return
					}
				case <- time.After(fsm.timeoutDuration):
					continue
				
			}
		}
	
	}


////////////////////////////////helper functions///////////////////////////////
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

func createPacket(ack uint32, seq uint32, flags byte, data string) CustomPacket {
	packet := CustomPacket{
		Header: Header{
			SeqNum: seq,
			AckNum: ack,
			Flags: flags,
		},
		Data: data,

	}
	return packet

}


func validPacket(response []byte, flags byte, seq uint32) bool {
	header,  err := parsePacket(response)
	if err != nil {
		return false
	}
	return header.Flags == flags && header.AckNum == seq
}

func parsePacket(response []byte) (*Header,  error) {
	var packet CustomPacket
	err := json.Unmarshal(response, &packet)
	if err != nil {
		return nil,  err
	}
	return &packet.Header, nil
}

/////////////////////////////main function//////////////////////////////////////

func main() {
	writerFSM := NewWriterFSM()
	writerFSM.Run()
}
