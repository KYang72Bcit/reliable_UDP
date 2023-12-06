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
	stopChan chan struct{} //channel for notifying go routines to stop
	serverCloseChan chan struct{} //channel for server close handling
	transmitChan chan struct{} //channel for notifying transmission
	reTransmitChan chan struct{} //channel for notifying retransmission
	ack uint32
	seq uint32
	data string
	timeoutDuration time.Duration
	lastPacketMutex sync.Mutex
	lastResponseMutex sync.Mutex
	lastPacket []byte
	wg sync.WaitGroup
	isStdInStarted bool
	isLastPacketACKReceived bool
}

const (
	Init WriterState = iota
	CreateSocket
	SyncronizeServer
	Transmitting
	ReTransmitting
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
		currentState: Init,
		maxRetries: maxRetries,
		stdinReader: bufio.NewReader(os.Stdin),
		responseChan: make(chan []byte),
		inputChan: make(chan CustomPacket, packetBufferSize),
		errorChan: make(chan error),
		EOFchan: make(chan struct{}),
		stopChan: make(chan struct{}),
		transmitChan: make(chan struct{}),
		reTransmitChan: make(chan struct{}),
		ack: 0,
		seq: 0,
		data: "",
		serverCloseChan: make(chan struct{}),
		lastPacket: make([]byte, 0),
		timeoutDuration: 2 * time.Second,
		isStdInStarted: false,

	}
}

func (fsm *WriterFSM) init_State() WriterState {
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

func (fsm *WriterFSM) create_socket_state() WriterState {
	addr := &net.UDPAddr{IP: fsm.ip, Port: fsm.port}
	fsm.udpcon, fsm.err = net.DialUDP("udp", nil, addr)
	if fsm.err != nil {
		return FatalError
	}
	return SyncronizeServer
}

func (fsm *WriterFSM) syncronize_server_state() WriterState {
	fsm.wg.Add(2)

	go fsm.listenResponse(fsm.timeoutDuration)
	go fsm.sendPacket()
	for {
		packet := createPacket(fsm.ack, fsm.seq, FLAG_SYN, "")
		fsm.inputChan <- packet
		select {
			case fsm.err = <- fsm.errorChan:
				return FatalError
			case responsePacket := <- fsm.responseChan:
				if isValidPacket(responsePacket, FLAG_ACK, fsm.seq){
					return Transmitting
				} else {
					continue
				}

			case <- time.After(fsm.timeoutDuration):
				continue
		}
	}

}

/////////////////////////////////////////////Transmitting State////////////////////////////////////////

func (fsm *WriterFSM) transmitting_state() WriterState {
	fmt.Println("Transmitting")
	if !fsm.isStdInStarted {
		go fsm.readStdin()
		fsm.isStdInStarted = true
	}
	for {
		select {
			case <- fsm.EOFchan:
				return Termination
			case <-fsm.serverCloseChan:
				return CloseConnectionByServer
			case fsm.err = <- fsm.errorChan:
				return ErrorHandling
			case <-fsm.reTransmitChan:
				return ReTransmitting
			case <-fsm.transmitChan:
				continue

		}
	}
}

func (fsm *WriterFSM) retransmitting_state() WriterState {
	fmt.Println("ReTransmitting")

	for i := 0; i < maxRetries; i++ {
		fsm.resendPacket()
		select {
			case <- fsm.EOFchan:
				return Termination
			case <-fsm.serverCloseChan:
				return CloseConnectionByServer
			case fsm.err = <- fsm.errorChan:
				return ErrorHandling
			case <-fsm.transmitChan:
				return Transmitting
			case <-fsm.reTransmitChan:
				continue

		}
	}
	return FatalError
}


func (fsm *WriterFSM) error_handling_state() WriterState {
	fmt.Println("Error:", fsm.err)
	close(fsm.stopChan) //notify all goroutines to stop
	fmt.Println("Error before wait ")
	fsm.wg.Wait()
	fmt.Println("Something goes wrong, start over again ")
	return Recover
}


func (fsm *WriterFSM) recover_state() WriterState {
	fsm.stopChan = make(chan struct{})
	fmt.Println("Recovering")
	return SyncronizeServer
}


func (fsm *WriterFSM) fatal_error_state() WriterState {
	fmt.Println("Fatal Error:", fsm.err)
	return Termination
}


func (fsm *WriterFSM) close_connection_by_server_state() WriterState {
	defer fsm.udpcon.Close()
	fmt.Println("Closing connection by server")
	close(fsm.stopChan)
	fsm.wg.Wait()
	fmt.Println("all goroutines closed")
	fsm.wg.Add(1)
	go fsm.listenResponse(5000*time.Millisecond)

	for  i := 0; i < maxRetries; i++ {
		packet, _ := json.Marshal(createPacket(fsm.ack, fsm.seq, FLAG_FIN|FLAG_ACK, ""))
		_, err := fsm.udpcon.Write(packet)
		if err != nil {
			
			return Exit
		}
		select {
			case response := <- fsm.responseChan:
				if isValidPacket(response, FLAG_FIN|FLAG_ACK, fsm.seq) {
					break

				}
			case <- time.After(fsm.timeoutDuration):
				continue

		}
	}

	for  i := 0; i < maxRetries; i++ {
		packet, _ := json.Marshal(createPacket(fsm.ack, fsm.seq, FLAG_FIN, ""))
		_, err := fsm.udpcon.Write(packet)
		if err != nil {

			return Exit
		}
		select {
			case response := <- fsm.responseChan:
				if isValidPacket(response, FLAG_FIN, fsm.seq) {
					return Exit

				}
			case <- time.After(fsm.timeoutDuration):
				continue
		}
	}
	return Exit
}

func (fsm *WriterFSM)terminate_state() WriterState {
	close(fsm.stopChan)
	fmt.Println("closing go routines")
	fsm.wg.Wait()
	fmt.Println("all goroutines closed")
	if fsm.udpcon != nil {
	fsm.udpcon.Close()
	}
	return Exit
}

func (fsm *WriterFSM) exit_state() {
	fmt.Println("Client Exiting...")
}

/////////////////////////////run function for WriterFSM////////////////////////////
func (fsm *WriterFSM) Run() {
	for {

			switch fsm.currentState {
			case Init:
				fsm.currentState = fsm.init_State()
			case CreateSocket:
				fsm.currentState = fsm.create_socket_state()
			case SyncronizeServer:
				fsm.currentState = fsm.syncronize_server_state()
			case Transmitting:
				fsm.currentState = fsm.transmitting_state()
			case ReTransmitting:
				fsm.currentState = fsm.retransmitting_state()
			case ErrorHandling:
				fsm.currentState = fsm.error_handling_state()
			case CloseConnectionByServer:
				fsm.currentState = fsm.close_connection_by_server_state()
			case Recover:
				fsm.currentState = fsm.recover_state()
			case FatalError:
				fsm.currentState = fsm.fatal_error_state()
			case Termination:
				fsm.currentState = fsm.terminate_state()
			case Exit:
				fsm.exit_state()
				return
			}

	}
}

/////////////////////////go routines for FSM////////////////////////////

func (fsm *WriterFSM) readStdin() {
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
			data = strings.Trim(data, "\n")
			packet := createPacket(fsm.ack, fsm.seq, FLAG_DATA, string(data))
				fsm.inputChan <- packet
				fsm.seq += uint32(len(data))
		}
	}
}


func (fsm *WriterFSM) listenResponse(timout time.Duration) {
	defer fmt.Println("closing listen response")
	defer fsm.wg.Done()
	for {
		select {
		case <-fsm.stopChan:
			return
		default:
			fsm.udpcon.SetReadDeadline(time.Now().Add(500*time.Millisecond))
			buffer := make([]byte, bufferSize)
			n, err := fsm.udpcon.Read(buffer)
			if err != nil {
				if netErr, ok := err.(net.Error); ok && netErr.Timeout() {
					continue
				} else {
					fsm.errorChan <- err
						return
				}
			}

			if n > 0 {
				if fsm.currentState == Transmitting || fsm.currentState == ReTransmitting {
				if isValidPacket(buffer[:n], FLAG_ACK, fsm.seq) {
					fsm.lastResponseMutex.Lock()
					fsm.isLastPacketACKReceived = true
					fsm.lastResponseMutex.Unlock()
					fsm.transmitChan <- struct{}{}
				}
				if isFINPacket(buffer[:n]) {
					fsm.serverCloseChan <- struct{}{}
				}

			} else {
				fsm.responseChan <- buffer[:n]
			}
		}

		}
	}
}


func (fsm *WriterFSM) sendPacket() {
	defer fmt.Println("closing send packet")
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
			
			fsm.lastResponseMutex.Lock()
			fsm.isLastPacketACKReceived = false
			fsm.lastResponseMutex.Unlock()

			_, err = fsm.udpcon.Write(packet)
			fmt.Println("packet sent: ", string(packet))
			if err != nil {
				fsm.errorChan <- err
				return
			}

			if fsm.currentState == Transmitting || fsm.currentState == ReTransmitting {
			fsm.lastPacketMutex.Lock()
			fsm.lastPacket = packet
			fsm.lastPacketMutex.Unlock()

			go func() {
				time.Sleep(fsm.timeoutDuration)

				fsm.lastPacketMutex.Lock()
				if !fsm.isLastPacketACKReceived {
					fsm.reTransmitChan <- struct{}{}
				}
				fsm.lastPacketMutex.Unlock()
			}()
		}
	}


	}
}


func (fsm *WriterFSM) resendPacket() {
	fsm.lastPacketMutex.Lock()
					packet := make([]byte, len(fsm.lastPacket))
					copy(packet, fsm.lastPacket)
					fsm.lastPacketMutex.Unlock()
					_, err := fsm.udpcon.Write(packet)
					if err != nil {
						fsm.errorChan <- err
						return
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
