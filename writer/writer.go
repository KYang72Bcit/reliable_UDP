package main

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	args = 3
	maxRetries = 2
	bufferSize = 1024 * 64
	packetBufferSize = 2
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
	DataLen uint32 `json:"dataLen"`
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
	wg sync.WaitGroup
	// packetSent int
	// packetReceived int
}

const (
	ValidateArgs WriterState = iota
	CreateSocket
	SyncronizeServer
	Transmitting
	Recover
	ErrorHandling
	FatalError
	CloseConnectionByServer
	Termination
	Exit
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
		lastPacket: make([]byte, 0),
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
	return SyncronizeServer
}

func (fsm *WriterFSM) SyncronizeServerState() WriterState {
	fsm.wg.Add(2)

	go fsm.listenResponse(500 * time.Millisecond)
	go fsm.sendPacket()
	for {
		packet := createPacket(fsm.ack, fsm.seq, FLAG_SYN, "")
		fsm.inputChan <- packet
		select {
			case fsm.err = <- fsm.errorChan:
				return FatalError
			case <- fsm.responseChan: //need to check if the response is valid? 
				return Transmitting
			case <- time.After(fsm.timeoutDuration):
				continue
		}
	}

}

/////////////////////////////////////////////Transmitting State////////////////////////////////////////

func (fsm *WriterFSM) TransmittingState() WriterState {
	fsm.wg.Add(2)
	go fsm.readStdin()
	go fsm.resendPacket()
	for {
		select {
			case <- fsm.EOFchan:
				return Termination
			case fsm.err = <- fsm.errorChan:
				return ErrorHandling

		}
	}
}

func (fsm *WriterFSM) ErrorHandlingState() WriterState {
	fmt.Println("Error:", fsm.err)
	close(fsm.stopChan) //notify all goroutines to stop 
	fsm.wg.Wait()
	fmt.Println("Something goes wrong, start over again ")
	return Recover
}


func (fsm *WriterFSM) RecoverState() WriterState {
	fsm.stopChan = make(chan struct{})
	fmt.Println("Recovering")
	return SyncronizeServer
}




func (fsm *WriterFSM) FatalErrorState() WriterState {
	fmt.Println("Fatal Error:", fsm.err)
	return Termination
}



func (fsm *WriterFSM) CloseConnectionByServerState1() WriterState {
	//on receive FIN from server, 
	fmt.Println("Closing connection by server")
	close(fsm.stopChan)
	fsm.wg.Wait()
	go fsm.listenResponse(500*time.Millisecond)
	
	for {
		packet, _ := json.Marshal(createPacket(fsm.ack, fsm.seq, FLAG_ACK, ""))
		_, err := fsm.udpcon.Write(packet)
		if err != nil {

			return Exit
		}
		select {
			case response := <- fsm.responseChan:
				if isValidPacket(response, FLAG_FIN|FLAG_ACK, fsm.seq) {
					return Exit
				}
			case <- time.After(fsm.timeoutDuration):
				continue

		}
	}
}

func (fsm *WriterFSM)TerminateState() WriterState {
	close(fsm.stopChan)
	fsm.wg.Wait()
	fsm.udpcon.Close()
	return Exit
}

func (fsm *WriterFSM) ExitState() {
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
			case SyncronizeServer:
				fsm.currentState = fsm.SyncronizeServerState()
			case Transmitting:
				fsm.currentState = fsm.TransmittingState()
			case Recover:
				fsm.currentState = fsm.RecoverState()
			case ErrorHandling:
				fsm.currentState = fsm.ErrorHandlingState()
			case FatalError:
				fsm.currentState = fsm.FatalErrorState()
			case Termination:
				fsm.currentState = fsm.TerminateState() 
			case Exit:
				return
			}
		 }
	}
}

/////////////////////////go routines for FSM////////////////////////////

func (fsm *WriterFSM) readStdin() {
	defer fsm.wg.Done()
	fmt.Println("reading stdin, press ctrl + D to exit")
	for {
		inputBuffer := make([]byte, bufferSize)
		n, err := fsm.stdinReader.Read(inputBuffer)
		if err != nil {
			if err.Error() == "EOF" {
				fsm.EOFchan <- struct{}{}
				return
			}
			fsm.errorChan <- err
			return
		}
		if n > 0 {
			data := string(inputBuffer[:n])
			packet := createPacket(fsm.ack, fsm.seq, FLAG_DATA, string(data))
				fsm.inputChan <- packet
				fsm.seq += uint32(len(data))

		}
	}
}



func (fsm *WriterFSM) sendPacket() {
	defer fsm.wg.Done()
	for {
	select {
		case <- fsm.stopChan:
			return
		case rawPacket, ok := <- fsm.inputChan:
			if !ok {
				return
			}
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
			} 
 
			go func(seq uint32) {
				select {
				case response := <-fsm.responseChan:
					if isFINPacket(response) {
						fsm.EOFchan <- struct{}{}
						return
					}
					if !isValidPacket(response, FLAG_ACK, seq) {
						fsm.resendChan <- struct{}{}
					}
				case <-time.After(fsm.timeoutDuration):
					fsm.resendChan <- struct{}{}
				}
			}(fsm.seq)
	}
}

func (fsm *WriterFSM) listenResponse(timout time.Duration) {
	defer fsm.wg.Done()
	for {
		select {
		case <-fsm.stopChan:
			return
		default:
			fsm.udpcon.SetReadDeadline(time.Now().Add(timout))
			buffer := make([]byte, bufferSize)
			n, err := fsm.udpcon.Read(buffer)
			if err != nil {
				if netErr, ok := err.(net.Error); ok && netErr.Timeout() {
					continue
				}
				 if netErr, ok := err.(net.Error);ok && net.ErrClosed == netErr {
					fmt.Println("connection closed")
					fsm.errorChan <- err
					return
				}
				if netErr, ok := err.(net.Error); ok && strings.Contains(netErr.Error(), "connection refused"){
					fmt.Println("connection refused")
					fsm.errorChan <- err
					return

				}
				fsm.errorChan <- err
				return
			}

			fsm.responseChan <- buffer[:n]
		}
	}
}



func (fsm *WriterFSM) resendPacket() {
	defer fsm.wg.Done()
	select {
		case <- fsm.stopChan:
			return
		case <- fsm.resendChan:
			for i := 0; i < fsm.maxRetries; i++ {

				fsm.lastPacketMutex.Lock()
				packet := make([]byte, len(fsm.lastPacket))
				copy(packet, fsm.lastPacket)
				fsm.lastPacketMutex.Unlock()
				_, err := fsm.udpcon.Write(packet)
				if err != nil {
					fsm.errorChan <- err
					return
				}
				select {
				case response := <-fsm.responseChan:
					if isFINPacket(response) {
						fsm.EOFchan <- struct{}{}
						return 
					}
					if isValidPacket(response, FLAG_ACK, fsm.seq) {
						return
					}
				case <-time.After(fsm.timeoutDuration):
					continue
			}
		}
		fsm.errorChan <- errors.New("max retries reached")
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
			DataLen: uint32(len(data)),
			Flags: flags,
		},
		Data: data,

	}
	return packet

}

func isValidPacket(response []byte, flags byte, seq uint32) bool {
	header,  err := parsePacket(response)
	if err != nil {
		return false
	}
	fmt.Println("expected ackNum: ", fmt.Sprint(seq))
	fmt.Println("actual ackNum: ", fmt.Sprint(header.AckNum))
	return header.Flags == flags && header.AckNum == seq
}
func isFINPacket(response []byte) bool {
	header,  err := parsePacket(response)
	if err != nil {
		return false
	}
	return header.Flags == FLAG_FIN
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
