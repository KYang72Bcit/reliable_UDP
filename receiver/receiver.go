package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"os"
	"os/signal"
	"strconv"
	"sync"
	"sync/atomic"
	"syscall"
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
	packetBufferSize = 50
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
	DataLen uint32 `json:"dataLen"`
	
}

/////////////////////////define Receiver FSM///////////////////////////

type ReceiverState int

const (
	Initialization ReceiverState = iota
	CreateSocket
	ReadyForReceiving
	Receiving
	Recover
	HandleError
	FatalError
	CloseConnectionByClient //when server receive FIN from client 
	CloseConnectionByServer // server send FIN to client
	Termination
)

type ReceiverFSM struct {
	currentState ReceiverState
	err error
	ip net.IP
	port int
	udpcon *net.UDPConn
	seqNum uint32
	ackNum uint32
	stopChan chan struct{}
	errorChan chan error
	responseChan chan []byte
	outputChan chan CustomPacket
	resendChan chan struct{}
	quitChan chan os.Signal
	wg sync.WaitGroup
	shouldRun int32
	clientAddr *net.UDPAddr

}

////////////////////////define Receiver Functions //////////////////////
func NewReceiverFSM() *ReceiverFSM {

	return &ReceiverFSM{
		currentState: Initialization,
		seqNum: 0,
		ackNum: 0,
		shouldRun: 1,
		stopChan: make(chan struct{}),
		errorChan: make(chan error),
		responseChan: make(chan []byte),
		resendChan: make(chan struct{}),
		outputChan: make(chan CustomPacket, packetBufferSize),
		quitChan: make(chan os.Signal, 1), //channel for handling ctrl+c
	}
}

func (fsm *ReceiverFSM) InitializationState() ReceiverState {
	signal.Notify(fsm.quitChan, syscall.SIGINT)
	go fsm.handleQuit()
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

	if atomic.LoadInt32(&fsm.shouldRun) == 0 {
		return Termination
	}
	addr := &net.UDPAddr{IP: fsm.ip, Port: fsm.port}
	fsm.udpcon, fsm.err = net.ListenUDP("udp", addr)
	if fsm.err != nil {
		if opErr, ok := fsm.err.(*net.OpError); ok && (opErr.Op == "accept" || opErr.Op == "close") {
			fmt.Println("Server closed connection")
			return Termination
		}
	}
	fmt.Println("UDP server listening on", fsm.udpcon.LocalAddr().String())
	return ReadyForReceiving
}

func (fsm *ReceiverFSM) ReadyForReveivingState() ReceiverState {
	fsm.wg.Add(3)
	go fsm.printToConsole()
	go fsm.listenResponse()
	go fsm.confirmPacket()
	return Receiving

}

func (fsm *ReceiverFSM) recoverState() ReceiverState {
	fmt.Println("Recover")
	fsm.stopChan = make(chan struct{})
	fsm.wg.Add(3)
	go fsm.printToConsole()
	go fsm.listenResponse()
	go fsm.confirmPacket()
	return Receiving
}

func (fsm *ReceiverFSM) ReceivingState() ReceiverState {
	
	for {
		select {
			case <-fsm.stopChan:
				return Termination
			case fsm.err = <- fsm.errorChan:
				return HandleError

		}
	}


}

func (fsm *ReceiverFSM) HandleErrorState() ReceiverState{
	fmt.Println("Error:", fsm.err)
		close(fsm.stopChan)
		fsm.wg.Wait()
		return Recover

}

func (fsm *ReceiverFSM) FatalErrorState() ReceiverState{
	fmt.Println("Fatal Error:", fsm.err)
	return Termination

}
//CloseConnectionByClient //when server receive FIN from client 
//CloseConnectionByServer // server send FIN to client

func (fsm *ReceiverFSM) CloseConnectionByClientState() ReceiverState {
	
	return Termination
}

func (fsm *ReceiverFSM) CloseConnectionByServerState() ReceiverState {
	//send FIN to client
	//receive ACK from client
	//send FIN ACK to client
	//receive ACK from client
	
	
}
 
func (fsm *ReceiverFSM) TerminationState() {
	fsm.wg.Wait()
	for {
		sendPacket(fsm.ackNum, fsm.seqNum, FLAG_FIN, "", fsm.udpcon, fsm.clientAddr)
		fsm.udpcon.SetReadDeadline(time.Now().Add(2000 * time.Millisecond))
		buffer := make([]byte, bufferSize)
		n, _, err := fsm.udpcon.ReadFromUDP(buffer)
				if err != nil {
					if netErr, ok := err.(net.Error); ok && netErr.Timeout(){
						continue
					}
					fsm.errorChan <- err
					fmt.Println("listenResponse get error")
					return
				}
				if n > 0 {
				rawPacket := buffer[:n]
				_, header, _ := parsePacket(rawPacket)
				if isFINPacket(header) {
					break
				}
				}		
		
	}
	fsm.udpcon.Close()
	fmt.Println("UDP server exiting...")
	
}

func (fsm *ReceiverFSM) Run() {
	for {
		switch fsm.currentState {
			case Initialization:
				fsm.currentState = fsm.InitializationState()
			case CreateSocket:
				fsm.currentState = fsm.CreateSocketState()
			case ReadyForReceiving:
				fsm.currentState = fsm.ReadyForReveivingState()
			case Receiving:
				fsm.currentState = fsm.ReceivingState()
			case HandleError:
				fsm.currentState = fsm.HandleErrorState()
			case Recover:
				fsm.currentState = fsm.recoverState()
			case FatalError:
				fsm.currentState = fsm.FatalErrorState()
			case Termination:
				fsm.TerminationState()
				return
		}
	}
}


/////////////////////////go routine ////////////////////////

////go routine for listening to incoming packets
func (fsm *ReceiverFSM) listenResponse() {
	defer fsm.wg.Done()
	for {
		select {
			case <- fsm.stopChan:
				return
			default:
				fsm.udpcon.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
				buffer := make([]byte, bufferSize)
				n, addr, err := fsm.udpcon.ReadFromUDP(buffer)
				if err != nil {
					if netErr, ok := err.(net.Error); ok && netErr.Timeout(){
						continue
					}
					fsm.errorChan <- err
					fmt.Println("listenResponse get error")
					return
				}
				if n > 0 {
					fsm.clientAddr = addr
					fsm.responseChan <- buffer[:n]

				}
				
		}
	}	
}


func (fsm *ReceiverFSM) confirmPacket() {
	defer fsm.wg.Done()
	for {
		select {
			case <- fsm.stopChan:
				return
			
			case rawPacket := <- fsm.responseChan:
				packet, header, err := parsePacket(rawPacket)
				if err != nil {
					fsm.errorChan <- err
				}
				if isSYNPacket(header){
					fsm.ackNum = header.SeqNum
				}
				if isValidPacket(header, fsm.ackNum, fsm.seqNum) {
					fsm.outputChan <- *packet
					fsm.ackNum += header.DataLen
				}
			
				if fsm.clientAddr != nil {
					sendPacket(fsm.ackNum, fsm.seqNum, FLAG_ACK, "", fsm.udpcon, fsm.clientAddr)
				} else {
					fmt.Println("Client address not set, cannot send ACK")
				}
				
		}
	}

}


func (fsm *ReceiverFSM) printToConsole() {
	defer fsm.wg.Done()
	for {
		select {
			case <- fsm.stopChan:
				return
			case packet := <- fsm.outputChan:
				fmt.Println(packet.Data)
		}
	}
}

////////////go routine for resend packet /////////////


func (fsm *ReceiverFSM) handleQuit() {
		<- fsm.quitChan
		fmt.Println("Received Ctrl+C, shutting down...")
		atomic.StoreInt32(&fsm.shouldRun, 0)
		close(fsm.stopChan)
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

func isValidPacket(header *Header , ackNum uint32, seqNum uint32) bool {
	fmt.Println("expected seqNum: ", fmt.Sprint(ackNum))
	fmt.Println("actual seqNum: ", fmt.Sprint(header.SeqNum))
	return header.SeqNum == ackNum

}
func isSYNPacket(header *Header) bool {
	return header.Flags == FLAG_SYN
}

func isFINPacket(header *Header) bool {
	return header.Flags == FLAG_FIN
}

func parsePacket(response []byte) (*CustomPacket, *Header, error) {
	var packet CustomPacket
	fmt.Println("Receive Packet" + string(response))
	err := json.Unmarshal(response, &packet)
	if err != nil {
		return &CustomPacket{}, &Header{}, err
	}
	return &packet, &packet.Header, nil
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

func sendPacket(ack uint32, seq uint32, flags byte, data string, udpcon *net.UDPConn, addr *net.UDPAddr) (int, error) {
	packet, err := createPacket(ack, seq, flags, data)
	if err != nil {
		return -1, err
	}
		_, err = udpcon.WriteTo(packet, addr)
		if err != nil {
			fmt.Println(err)
		}
	fmt.Println("Send Packet" + string(packet))
	return len(data), err

}

func main() {
	receiverFSM := NewReceiverFSM()
	receiverFSM.Run()
}
