# reliable_UDP
create reliable udp protocol 

Proxy (C)
-Sits between the writer and the receiver
-Takes command line arguments for the IP address and port of the receiver, the port of the writer
-Indefinitely listen for data from the writer
-Indefinitely listen for ack from the receiver
-Takes the % chance to drop data, drop acks, delay data, and delay acks on the command line
-The delay is handled with fixed numbers
-Forwards the ack to writer after it has determined to either forward or drop or delay and forward the ack
-Forwards the data to receiver after it has determined to either forward or drop or delay and forward the data
-Maintains a list of statistics showing how many packets have been sent and received
-Stores the list of statistics in a file
-Terminates if writer and receiver programs are terminated

Writer (Go)
-Reads from the keyboard and writes to a UDP socket
-Takes command line arguments for the IP address of the proxy (or receiver if the proxy is removed), and the port
-Sends data to the proxy
-If no ack is received in a reasonable time after a packet has been sent, it resends the packet
-Maintains a list of statistics showing how many packets have been sent and received
-Stores the list of statistics in a file

Receiver (Go)
-Reads from a UDP socket and writes to the console
-Take a command line argument for the port to receive from
-Receives data from the proxy
-Sends an ack to the proxy after data is received
-Maintains a list of statistics showing how many packets have been sent and received
-Stores the list of statistics in a file

GUI (Python)
-Receives statistics from the proxy to graph
-Graphs the data on the writer, receiver, and proxy
