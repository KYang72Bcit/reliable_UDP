package main

import (
	"bufio"
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

const (
	args = 3
	optionalGUIArgs = 5
	optionalIntervalArgs = 6
	maxRetries = 64
	maxDataLength = 216
	packetTTL = 64
	portLimit = 65535
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

var statistics []Statistics = make([]Statistics, 0)

//////////////////define custom packet structure//////////////////////
type CustomPacket struct {
	Header Header  `json:"header"`
	Data string    `json:"data"`
}


type Header struct {
	SeqNum uint32 `json:"seqNum"`
	AckNum uint32  `json:"ackNum"`
	DataLen uint32 `json:"dataLen"`
	TTL     int     `json:"ttl"` // how many times this packet has been sent
	Flags byte     `json:"flags"`
}

type Statistics struct {
	TimeStamp string `json:"Time Elapsed"`
	PacketSent int  `json:"PacketSent"`
	PacketAcked int `json:"PacketAcked"`
}




/////////////////////////define writer FSM///////////////////////////
type WriterState int
type WriterFSM struct {
	err error
	currentState WriterState
	ip net.IP
	port int
	guiIP net.IP
	guiPort int
	udpcon *net.UDPConn
	guicon net.Conn
	stdinReader *bufio.Reader
	quitChan chan os.Signal //channel for quit signal handling
	EOFchan chan struct{} //channel for EOF signal handling
	responseChan chan []byte //channel for response handling
	errorChan chan error //channel for error handling between go routines
	stopSendPacketChan chan struct{} //channel for notifying go routines to stop
	stopListenResponseChan chan struct{} //channel for notifying transmission
	ack uint32
	seq uint32
	timeoutDuration time.Duration
	timeInterval int
	mutex sync.Mutex
	lastPacket CustomPacket
	wg sync.WaitGroup
	isGoroutinesStarted bool
	isStdInStarted bool
	isLastPacketACKReceived bool
	packetQueue *DQueue
	packetSent int
	packetAcked int
}

const (
	Init WriterState = iota
	CreateSocket
	SetGUIConnction
	SyncronizeServer
	Transmitting
	Recover
	ErrorHandling
	FatalError
	Termination
	Exit
)

/////////////////////define Methods for WriterFSM for state transitions/////////////////////////
func NewWriterFSM() *WriterFSM {
	return &WriterFSM{
		currentState: Init,
		stdinReader: bufio.NewReader(os.Stdin),
		responseChan: make(chan []byte),
		errorChan: make(chan error),
		EOFchan: make(chan struct{}),
		stopSendPacketChan: make(chan struct{}),
		stopListenResponseChan: make(chan struct{}),
		quitChan: make(chan os.Signal, 1),
		ack: 0,
		seq: 0,
		lastPacket: CustomPacket{},
		timeoutDuration: 500*time.Millisecond,
		timeInterval: 10000,
		isGoroutinesStarted: false,
		isStdInStarted: false,
		packetQueue: NewDQueue(),
		packetSent: 0,
		packetAcked: 0,
	}
}

func (fsm *WriterFSM) init_State() WriterState {
	fmt.Println("Initializing")
	signal.Notify(fsm.quitChan, syscall.SIGINT)
	if (len(os.Args) != args && len(os.Args) != optionalGUIArgs && len(os.Args) != optionalIntervalArgs) {

		fsm.err = errors.New("invalid number of arguments, <IP> <port> [gui IP] [gui port] [transmission interval (milliseconds)]")
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

func (fsm *WriterFSM) create_socket_state() WriterState {
	go fsm.handleQuit()
	fmt.Println("Creating socket")
	addr := &net.UDPAddr{IP: fsm.ip, Port: fsm.port}
	fsm.udpcon, fsm.err = net.DialUDP("udp", nil, addr)
	if fsm.err != nil {
		return FatalError
	}
	if fsm.guiIP != nil && fsm.guiPort != 0 {
		return SetGUIConnction
	}
	return SyncronizeServer
}

func (fsm *WriterFSM) set_gui_connection_state() WriterState {
	fmt.Println("Setting GUI connection")
	fsm.guicon, fsm.err = net.Dial("tcp", fsm.guiIP.String() + ":" + strconv.Itoa(fsm.guiPort))
	if fsm.err != nil {
		return FatalError
	}
	go fsm.recordStatistics(fsm.timeInterval)
	return SyncronizeServer
}

func (fsm *WriterFSM) syncronize_server_state() WriterState {
	fmt.Println("Syncronizing server")
	fsm.isGoroutinesStarted = true
	if fsm.guicon == nil {
		go fsm.recordStatistics(fsm.timeInterval)
	}
	fsm.wg.Add(2)
	go fsm.listenResponse(fsm.timeoutDuration)
	go fsm.sendPacket()
	for i := 0; i < maxRetries; i++ {
		packet := createPacket(fsm.ack, fsm.seq, FLAG_SYN, "")
		fsm.packetQueue.PushBack(packet)
		select {
			case fsm.err = <- fsm.errorChan:
				return FatalError
			case responsePacket := <- fsm.responseChan:
				if isValidPacket(responsePacket, FLAG_ACK, fsm.seq){
					fsm.packetAcked++
					return Transmitting
				} else {
					continue
				}

			case <- time.After(fsm.timeoutDuration):
				continue
		}
	}
	fsm.err = errors.New("max retries to connect server exceeded")
	return FatalError

}

/////////////////////////////////////////////Transmitting State////////////////////////////////////////

func (fsm *WriterFSM) transmitting_state() WriterState {
	fmt.Println("Transmitting")
	fmt.Println("reading stdin, press ctrl + D to exit")
	if !fsm.isStdInStarted {
		go fsm.readStdin()
		fsm.isStdInStarted = true
	}

	for {
		select {
			case <- fsm.EOFchan:
				return Termination
			case fsm.err = <- fsm.errorChan:
				return ErrorHandling

		}
	}
}


func (fsm *WriterFSM) error_handling_state() WriterState {
	fmt.Println("Error:", fsm.err)
	fsm.wg.Wait()
	return Recover
}


func (fsm *WriterFSM) recover_state() WriterState {
	fmt.Println("Recovering")
	return SyncronizeServer
}


func (fsm *WriterFSM) fatal_error_state() WriterState {
	fmt.Println("Fatal Error:", fsm.err)
	if fsm.isGoroutinesStarted{
		return Termination
	}
	return Exit
}



func (fsm *WriterFSM) terminate_state() WriterState {
	fmt.Println("Terminating")
	fsm.stopSendPacketChan <- struct{}{}


	done := make(chan struct{})
	go func() {
		fsm.wg.Wait()
		close(done)
	}()

	for {
		select {
		case <-done:
			fmt.Println("all goroutines closed")
			fsm.udpcon.Close()
			return Exit
		case err := <-fsm.errorChan:
			fmt.Println("Error:", err)
		}
	}
}


func (fsm *WriterFSM) exit_state() {
	if fsm.udpcon != nil {
		fsm.udpcon.Close()
	}

	if fsm.guicon != nil {
		fsm.guicon.Close()
	}
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
			case SetGUIConnction:
				fsm.currentState = fsm.set_gui_connection_state()
			case SyncronizeServer:
				fsm.currentState = fsm.syncronize_server_state()
			case Transmitting:
				fsm.currentState = fsm.transmitting_state()
			case ErrorHandling:
				fsm.currentState = fsm.error_handling_state()
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
	for {
		inputBuffer := make([]byte, maxDataLength)
		n, err := fsm.stdinReader.Read(inputBuffer)
		if err != nil {
			if err.Error() == "EOF" {
				fmt.Println("EOF received from standard input")
				fsm.EOFchan <- struct{}{}
				return
			}
			fsm.errorChan <- err
			return
		}
		if n > 0 {
			data := string(inputBuffer[:n])
			packet := createPacket(fsm.ack, fsm.seq, FLAG_DATA, string(data))
			fsm.packetQueue.PushBack(packet)
				fsm.seq += uint32(len(data))
		}
	}
}

//go routine for listening response from server
func (fsm *WriterFSM) listenResponse(timout time.Duration) {
	defer fsm.wg.Done()
	for {
		select {
		case <-fsm.stopListenResponseChan:
			return
		default:
			fsm.udpcon.SetReadDeadline(time.Now().Add(300*time.Millisecond))
			buffer := make([]byte, maxDataLength)
			n, err := fsm.udpcon.Read(buffer)
			if err != nil {
				if netErr, ok := err.(net.Error); ok && netErr.Timeout() {
					continue
				} else {
					fmt.Println("listenResponse get error")
					fsm.EOFchan <- struct{}{}
					fsm.isGoroutinesStarted = false
					return

				}
			}
			if n > 0 {

				if fsm.currentState != SyncronizeServer {
					if isValidPacket(buffer[:n], FLAG_ACK, fsm.lastPacket.Header.SeqNum + fsm.lastPacket.Header.DataLen) {
						fsm.packetAcked++
						fsm.mutex.Lock()
						fsm.isLastPacketACKReceived = true
						fsm.mutex.Unlock()
					}

			} else {
				fsm.responseChan <- buffer[:n]
			}
		}

		}
	}
}
func (fsm *WriterFSM) handleQuit() {
	<-fsm.quitChan
	fmt.Println("Ctrl + C received")
	fsm.EOFchan <- struct{}{}
}


//go routine for sending packets
func (fsm *WriterFSM) sendPacket() {

	defer fsm.wg.Done()
	shouldStop := false

	for {
		select {
			case <- fsm.stopSendPacketChan:
				if !shouldStop {
					shouldStop = true
				}

			default:
				if fsm.packetQueue.Len() > 0 {
					rawPacket := fsm.packetQueue.PopFront()
					if rawPacket.Header.TTL < 0 {

						fsm.errorChan <- errors.New("max retries to send packet exceeded")
						fsm.stopListenResponseChan <- struct{}{}
						return
					}
					packet, _ := json.Marshal(rawPacket)
					_, err := fsm.udpcon.Write(packet)
					if err != nil {
						fsm.errorChan <- err
					}

					fsm.packetSent++
					if fsm.currentState != SyncronizeServer {
						fsm.mutex.Lock()
						fsm.isLastPacketACKReceived = false
						fsm.lastPacket = rawPacket
						fsm.mutex.Unlock()
						time.Sleep(fsm.timeoutDuration)
						fsm.mutex.Lock()
						if !fsm.isLastPacketACKReceived {
							rawPacket.Header.TTL--
							fsm.packetQueue.PushFront(rawPacket)
						}
						fsm.mutex.Unlock()
					}

				}  else if shouldStop {
					fsm.stopListenResponseChan <- struct{}{}
					return

				} else  {
					time.Sleep(100*time.Millisecond)
				}
		}

		}
	}

	//go routine for recording statistics and send to GUI
	func (fsm *WriterFSM) recordStatistics(timeInterval int) {
		start := time.Now()
		for {
			elapsed := int(time.Since(start).Seconds())
			elapsedTimestamp := time.Date(0, 1, 1, 0, 0, elapsed, 0, time.UTC)
			formattedTimestamp := elapsedTimestamp.Format("04:05")


			statistic := Statistics{
				TimeStamp: formattedTimestamp,
				PacketSent: fsm.packetSent,
				PacketAcked: fsm.packetAcked,
			}

			if fsm.guicon != nil {
				err := json.NewEncoder(fsm.guicon).Encode(statistic)
				if err != nil {
				return
				}
			}

			statistics = append(statistics, statistic)
			time.Sleep(time.Duration(timeInterval)*time.Millisecond)
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
	if err != nil || portNo <= 0 || portNo > portLimit {
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
			TTL: packetTTL,
		},
		Data: data,

	}
	return packet

}

//check if the received packet is valid
func isValidPacket(response []byte, flags byte, seq uint32) bool {
	header,  err := parsePacket(response)
	if err != nil {
		return false
	}
	return header.Flags == flags && header.AckNum == seq
}

//parse the received packet to header
func parsePacket(response []byte) (*Header,  error) {
	var packet CustomPacket
	err := json.Unmarshal(response, &packet)
	if err != nil {
		return nil,  err
	}
	return &packet.Header, nil
}

//export statistics to csv file
func exportToCSV(statistics []Statistics) error {
	file, err := os.Create("writer_performance.csv")
	if err != nil {
		return err
	}
	defer file.Close()

	writer := csv.NewWriter(file)
	defer writer.Flush()

	writer.Write([]string{"Time Elapsed", "Data Sent", "ACK Received"})

	for _, stat := range statistics {

		record := []string{
			stat.TimeStamp,
			strconv.Itoa(stat.PacketSent),
			strconv.Itoa(stat.PacketAcked),
		}
		writer.Write(record)
	}

	return nil
}



/////////////////////////////main function//////////////////////////////////////

func main() {
	writerFSM := NewWriterFSM()
	writerFSM.Run()
	exportToCSV(statistics)
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
