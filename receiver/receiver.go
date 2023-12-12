package main

import (
	"encoding/csv"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"os"
	"os/signal"
	"strconv"
	"sync"
	"syscall"
	"time"
)

/** FLAGS
	* 00000001 - ACK -> 1
	* 00000010 - SYN -> 2
	* 00000100 - FIN -> 4
	* 00001000 - DATA -> 8
**/

const (
	FLAG_ACK = 1 << iota
	FLAG_SYN
	FLAG_FIN
	FLAG_DATA
)

const (
	args = 3
	optionalGUIArgs = 5
	optionalIntervalArgs = 6
	bufferSize = 1024
	packetBufferSize = 50
	timeoutDuration	= 200 * time.Millisecond
)
var statistics []Statistics = make([]Statistics, 0)

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

//////////////////define Statistics structure /////////////////////
type Statistics struct {
	TimeStamp string `json:"Time Elapsed"`
	PacketSent int  `json:"PacketsSent"`
	PacketReceived int `json:"PacketsReceived"`
	CorrectPacket int  `json:"CorrectPackets"`
}

/////////////////////////define Receiver FSM///////////////////////////

type ReceiverState int

const (
	Init ReceiverState = iota
	CreateSocket
	SetGUIConnction
	ReadyForReceiving
	Receiving
	Recover
	HandleError
	FatalError
	Termination
)

type ReceiverFSM struct {
	currentState ReceiverState
	err error
	ip net.IP
	port int
	guiIP net.IP
	guiPort int
	guicon net.Conn
	udpcon *net.UDPConn
	seqNum uint32
	ackNum uint32
	stopChan chan struct{}
	errorChan chan error
	responseChan chan []byte
	outputChan chan CustomPacket
	quitChan chan os.Signal
	wg sync.WaitGroup
	clientAddr *net.UDPAddr
	timeInterval int
	packetSent int
	packetReceived int
	correctPacket int

}

////////////////////////define Receiver Functions //////////////////////
func NewReceiverFSM() *ReceiverFSM {

	return &ReceiverFSM{
		currentState: Init,
		seqNum: 0,
		ackNum: 0,
		stopChan: make(chan struct{}),
		errorChan: make(chan error),
		responseChan: make(chan []byte),
		outputChan: make(chan CustomPacket, packetBufferSize),
		quitChan: make(chan os.Signal, 1),
		packetSent: 0,
		packetReceived: 0,
		correctPacket: 0,

	}
}

func (fsm *ReceiverFSM) init_state() ReceiverState {
	signal.Notify(fsm.quitChan, syscall.SIGINT)
	go fsm.handleQuit()
	if (len(os.Args) != args && len(os.Args) != optionalGUIArgs && len(os.Args) != optionalIntervalArgs) {

		fsm.err = errors.New("invalid number of arguments, <IP> <port> [gui IP] [gui port] [transmission interval(milliseconds)]")
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

	if len(os.Args) == optionalGUIArgs || len(os.Args) == optionalIntervalArgs {
		fsm.guiIP, fsm.err = validateIP(os.Args[3])
		if fsm.err != nil {
			return FatalError
		}
		fsm.guiPort, fsm.err = validatePort(os.Args[4])
		if fsm.err != nil {
			return FatalError
		}
	}

	if len(os.Args) == optionalIntervalArgs {
		fsm.timeInterval, fsm.err = strconv.Atoi(os.Args[5])
		if fsm.err != nil {
			return FatalError
		}
	}
	return CreateSocket
}


func (fsm *ReceiverFSM) create_socket_state() ReceiverState {

	addr := &net.UDPAddr{IP: fsm.ip, Port: fsm.port}
	fsm.udpcon, fsm.err = net.ListenUDP("udp", addr)
	 if fsm.err != nil {
			return FatalError
	}
	
	fmt.Println("UDP server listening on", fsm.udpcon.LocalAddr().String())
	return SetGUIConnction
}

func (fsm *ReceiverFSM) set_gui_connection_state() ReceiverState {
	fmt.Println("Setting GUI connection")
	fsm.guicon, fsm.err = net.Dial("tcp", fsm.guiIP.String() + ":" + strconv.Itoa(fsm.guiPort))
	if fsm.err != nil {
		return FatalError
	}

	go fsm.recordStatistics(fsm.timeInterval)
	return ReadyForReceiving
}
func (fsm *ReceiverFSM) ready_for_receiving_state() ReceiverState {	
	fsm.wg.Add(3)
	go fsm.printToConsole()
	go fsm.listenResponse()
	go fsm.confirmPacket()
	return Receiving
}

func (fsm *ReceiverFSM) recover_state() ReceiverState {
	fmt.Println("Recovered from error, resuming...")
	fsm.stopChan = make(chan struct{})
	fsm.wg.Add(3)
	go fsm.printToConsole()
	go fsm.listenResponse()
	go fsm.confirmPacket()
	return Receiving
}

func (fsm *ReceiverFSM) receiving_state() ReceiverState {
	fmt.Println("Receiving...")
	
	for {
		select {
			case <-fsm.stopChan:
				return Termination
			case fsm.err = <- fsm.errorChan:
				return HandleError

		}
	}
}


func (fsm *ReceiverFSM) handle_error_state() ReceiverState{
	fmt.Println("Error:", fsm.err)
		close(fsm.stopChan)
		fsm.wg.Wait()
		return Recover

}

func (fsm *ReceiverFSM) fatal_error_state() ReceiverState{
	fmt.Println("Fatal Error:", fsm.err)
	return Termination

}

 
func (fsm *ReceiverFSM) termination_state() {
	fsm.wg.Wait()
	if fsm.udpcon != nil{
		fsm.udpcon.Close()
	}	

	if fsm.guicon != nil {
		fsm.guicon.Close()
	}
	fmt.Println("UDP server exiting...")	
}

func (fsm *ReceiverFSM) Run() {
	for {
		switch fsm.currentState {
			case Init:
				fsm.currentState = fsm.init_state()
			case CreateSocket:
				fsm.currentState = fsm.create_socket_state()
			case SetGUIConnction:
				fsm.currentState = fsm.set_gui_connection_state()
			case ReadyForReceiving:
				fsm.currentState = fsm.ready_for_receiving_state()
			case Receiving:
				fsm.currentState = fsm.receiving_state()
			case HandleError:
				fsm.currentState = fsm.handle_error_state()
			case Recover:
				fsm.currentState = fsm.recover_state()
			case FatalError:
				fsm.currentState = fsm.fatal_error_state()
			case Termination:
				fsm.termination_state()
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
				fsm.udpcon.SetReadDeadline(time.Now().Add(timeoutDuration))
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
					fsm.packetReceived++
					fsm.clientAddr = addr
					fsm.responseChan <- buffer[:n]

				}
		}
	}	
}

/// go routine for confirming the packet
func (fsm *ReceiverFSM) confirmPacket() {
	defer fsm.wg.Done()
	for {
		select {
			case <- fsm.stopChan:
				return
			
			case rawPacket := <- fsm.responseChan:
				packet, header, err := parsePacket(rawPacket)
				if err != nil {
					continue
				}
	
				if isSYNPacket(header){
					fsm.ackNum = header.SeqNum
				}
				if isValidPacket(header, fsm.ackNum, fsm.seqNum) {
					fsm.outputChan <- *packet
					fsm.correctPacket++
					fsm.ackNum += header.DataLen
				}
			
				if fsm.clientAddr != nil {
					sendPacket(fsm.ackNum, fsm.seqNum, FLAG_ACK, "", fsm.udpcon, fsm.clientAddr)
					fsm.packetSent++
				} else {
					fmt.Println("Client address not set, cannot send ACK")
				}
				
		}
	}

}

/// go routine for printing the packet to console
func (fsm *ReceiverFSM) printToConsole() {
	defer fsm.wg.Done()
	for {
		select {
			case <- fsm.stopChan:
				return
			case packet := <- fsm.outputChan:
				fmt.Print(packet.Data)
		}
	}
}

/// go routine for recording the statistics and send to GUI
func (fsm *ReceiverFSM) recordStatistics(timeinterval int) {
	start := time.Now()
	for {
		elapsed := int(time.Since(start).Seconds())
		elapsedTimestamp := time.Date(0, 1, 1, 0, 0, elapsed, 0, time.UTC)
		formattedTimestamp := elapsedTimestamp.Format("04:05")


		statistic := Statistics{
			TimeStamp: formattedTimestamp,
			PacketSent: fsm.packetSent,
			PacketReceived: fsm.packetReceived,
			CorrectPacket: fsm.correctPacket,
		}

		statistics = append(statistics, statistic)
		if fsm.guicon != nil {
			err := json.NewEncoder(fsm.guicon).Encode(statistic)
			if err != nil {
			return
			}
		}
		time.Sleep(time.Duration(timeinterval) * time.Millisecond)

	}

}


/// go routine for handling Ctrl+C
func (fsm *ReceiverFSM) handleQuit() {
		<- fsm.quitChan
		fmt.Println("Received Ctrl+C, shutting down...")
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
	if err != nil || portNo <= 0 || portNo > 65535 {
		return -1, errors.New("invalid port number")
	}
	return portNo, nil
}

func isValidPacket(header *Header , ackNum uint32, seqNum uint32) bool {
	return header.SeqNum == ackNum

}
func isSYNPacket(header *Header) bool {
	return header.Flags == FLAG_SYN
}



func parsePacket(response []byte) (*CustomPacket, *Header, error) {
	var packet CustomPacket
	//fmt.Println("Receive Packet" + string(response))
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
	return len(data), err

}

func exportToCSV(statistics []Statistics) error {
	file, err := os.Create("receiver_performance.csv")
	if err != nil {
		return err
	}
	defer file.Close()

	writer := csv.NewWriter(file)
	defer writer.Flush()

	writer.Write([]string{"Time Elapsed", "Packets Received", "Packets Sent", "Correct Packets"})

	for _, stat := range statistics {
		
		record := []string{
			stat.TimeStamp,
			strconv.Itoa(stat.PacketReceived),
			strconv.Itoa(stat.PacketSent),
			strconv.Itoa(stat.CorrectPacket),
			
		}
		writer.Write(record)
	}

	return nil
}


func main() {
	receiverFSM := NewReceiverFSM()
	receiverFSM.Run()
	exportToCSV(statistics)
}
