package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"os"
	"strconv"
	"os/signal"
	"syscall"
	"sync"
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
	outputChan chan CustomPacket
	resendChan chan struct{}
	quitChan chan os.Signal
	wg sync.WaitGroup

}

////////////////////////define Receiver Functions //////////////////////
func NewReceiverFSM() *ReceiverFSM {

	return &ReceiverFSM{
		currentState: Initialization,
		seqNum: 0,
		ackNum: 0,
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
	addr := &net.UDPAddr{IP: fsm.ip, Port: fsm.port}
	fsm.udpcon, fsm.err = net.ListenUDP("udp", addr)
	if fsm.err != nil {
		return FatalError
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

func (fsm *ReceiverFSM) ReceivingState() ReceiverState {
	for {
		select {
			case <- fsm.quitChan:
				return Termination
			case <- fsm.errorChan:
				return HandleError

		}
	}


}

func (fsm *ReceiverFSM) HandleErrorState() ReceiverState{
	fmt.Println("Error:", fsm.err)
		fsm.stopChan <- struct{}{}
		fsm.wg.Wait()
		return ReadyForReceiving

}

func (fsm *ReceiverFSM) FatalErrorState() ReceiverState{
	fmt.Println("Fatal Error:", fsm.err)
	return Termination

}
 
func (fsm *ReceiverFSM) TerminationState() {
	fsm.stopChan <- struct{}{} 
	fsm.wg.Wait()
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


func (fsm *ReceiverFSM) confirmPacket() {
	defer fsm.wg.Done()
	for {
		select {
			case <- fsm.stopChan:
				return
			default:
				rawPacket := <- fsm.responseChan
				packet, header, err := parsePacket(rawPacket)
				if err != nil {
					fsm.errorChan <- err
				}
				if isValidPacket(header, fsm.ackNum, fsm.seqNum, FLAG_DATA) {
					fsm.outputChan <- *packet
					fsm.ackNum += header.DataLen
				}
				 sendPacket(fsm.ackNum, fsm.seqNum, FLAG_ACK, "", fsm.udpcon)
		}
	}

}

//////go routine for printing to the console /////////

func (fsm *ReceiverFSM) printToConsole() {
	defer fsm.wg.Done()
	for packet := range fsm.outputChan {
		select {
			case <- fsm.stopChan:
				return
			default:
				fmt.Println(packet.Data)
		}

	}
}

////////////go routine for resend packet /////////////



func (fsm *ReceiverFSM) handleQuit() {
		<- fsm.quitChan
		fsm.stopChan <- struct{}{}
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

func isValidPacket(header *Header , ackNum uint32, seqNum uint32, FLAG byte) bool {
	return header.AckNum == ackNum &&
	header.SeqNum == seqNum &&
	header.Flags == FLAG

}

func parsePacket(response []byte) (*CustomPacket, *Header, error) {
	var packet CustomPacket
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

func sendPacket(ack uint32, seq uint32, flags byte, data string, udpcon net.Conn) (int, error) {
	packet, err := createPacket(ack, seq, flags, data)
	if err != nil {
		return -1, err
	}
		_, err = udpcon.Write(packet)

	return len(data), err

}
