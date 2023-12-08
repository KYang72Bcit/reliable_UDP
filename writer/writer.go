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
	maxRetries = 10
	maxDataLength = 128
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
	stopListenResponseChan chan struct{} //channel for notifying transmission
	ack uint32
	seq uint32
	data string
	timeoutDuration time.Duration
	mutex sync.Mutex
	lastPacket CustomPacket
	wg sync.WaitGroup
	isStdInStarted bool
	isLastPacketACKReceived bool
	packetQueue *DQueue
}

const (
	Init WriterState = iota
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
		currentState: Init,
		maxRetries: maxRetries,
		stdinReader: bufio.NewReader(os.Stdin),
		responseChan: make(chan []byte),
		inputChan: make(chan CustomPacket, packetBufferSize),
		errorChan: make(chan error),
		EOFchan: make(chan struct{}),
		stopChan: make(chan struct{}),
		stopListenResponseChan: make(chan struct{}),
		ack: 0,
		seq: 0,
		data: "",
		serverCloseChan: make(chan struct{}),
		lastPacket: CustomPacket{},
		timeoutDuration: 2 * time.Second,
		isStdInStarted: false,
		packetQueue: NewDQueue(),

	}
}

func (fsm *WriterFSM) init_State() WriterState {
	fmt.Println("Initializing")
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
	fmt.Println("Creating socket")
	addr := &net.UDPAddr{IP: fsm.ip, Port: fsm.port}
	fsm.udpcon, fsm.err = net.DialUDP("udp", nil, addr)
	if fsm.err != nil {
		return FatalError
	}
	return SyncronizeServer
}

func (fsm *WriterFSM) syncronize_server_state() WriterState {
	fmt.Println("Syncronizing server")
	fsm.wg.Add(2)

	go fsm.listenResponse(fsm.timeoutDuration)
	go fsm.sendPacket()
	for {
		packet := createPacket(fsm.ack, fsm.seq, FLAG_SYN, "")
		fsm.packetQueue.PushBack(packet)
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

		}
	}
}


func (fsm *WriterFSM) error_handling_state() WriterState {
	fmt.Println("Error:", fsm.err)
	fsm.stopChan <- struct{}{}
	fsm.wg.Wait()
	return Recover
}


func (fsm *WriterFSM) recover_state() WriterState {
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
	fsm.stopChan <- struct{}{}
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
	fsm.stopChan <- struct{}{}
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
		inputBuffer := make([]byte, maxDataLength)
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
			fsm.packetQueue.PushBack(packet)
				fsm.seq += uint32(len(data))
		}
	}
}

func (fsm *WriterFSM) listenResponse(timout time.Duration) {
	defer fmt.Println("listening to response closed")
	defer fsm.wg.Done()
	for {
		select {
		case <-fsm.stopListenResponseChan:
			return
		default:
			fsm.udpcon.SetReadDeadline(time.Now().Add(500*time.Millisecond))
			buffer := make([]byte, maxDataLength)
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
				if fsm.currentState != SyncronizeServer {
					if isValidPacket(buffer[:n], FLAG_ACK, fsm.lastPacket.Header.SeqNum + fsm.lastPacket.Header.DataLen) {
						fsm.mutex.Lock()
						fsm.isLastPacketACKReceived = true
						fsm.mutex.Unlock()
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
	defer fmt.Println("sending packet closed")
	defer fsm.wg.Done()
	shouldStop := false

	for {
		select {
			case <- fsm.stopChan:
				if !shouldStop {
					shouldStop = true
					fmt.Println("Receive EOF from keyboard")
				}

			default:

				if fsm.packetQueue.Len() > 0 {
					rawPacket := fsm.packetQueue.PopFront()
					packet, _ := json.Marshal(rawPacket)
					_, err := fsm.udpcon.Write(packet)
					if err != nil {
						fsm.errorChan <- err
						return
					}
					if fsm.currentState != SyncronizeServer {
						fsm.mutex.Lock()
						fsm.isLastPacketACKReceived = false
						fsm.lastPacket = rawPacket
						fsm.mutex.Unlock()
						time.Sleep(fsm.timeoutDuration)
						fsm.mutex.Lock()
						if !fsm.isLastPacketACKReceived {
							fsm.packetQueue.PushFront(rawPacket)
						}
						fsm.mutex.Unlock()
					}

				}  else if shouldStop {
					fmt.Println("stop sending packet")
					fsm.stopListenResponseChan <- struct{}{}
					return

				} else  {
					time.Sleep(100*time.Millisecond)
				}

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

///////////////////thread safe dqueue/////////////
type DQueue struct {
	queue    []CustomPacket
	mutex    sync.Mutex
	len      int
	notEmpty *sync.Cond
}

func NewDQueue() *DQueue {
	q := new(DQueue)
	q.queue = make([]CustomPacket, 0)
	q.len = 0
	q.notEmpty = sync.NewCond(&q.mutex)
	return q
}

func (q *DQueue) PushFront(packet CustomPacket) {
	q.mutex.Lock()
	defer q.mutex.Unlock()
	q.queue = append([]CustomPacket{packet}, q.queue...)
	q.len++
	q.notEmpty.Signal()
}

func (q *DQueue) PushBack(packet CustomPacket) {
	q.mutex.Lock()
	defer q.mutex.Unlock()
	q.queue = append(q.queue, packet)
	q.len++
	q.notEmpty.Signal()
}

func (q *DQueue) PopFront() CustomPacket {
	q.mutex.Lock()
	defer q.mutex.Unlock()
	for q.len == 0 {
		q.notEmpty.Wait()
	}
	packet := q.queue[0]
	q.queue = q.queue[1:]
	q.len--
	return packet
}

func (q *DQueue) Len() int {
	q.mutex.Lock()
	defer q.mutex.Unlock()
	return q.len
}
