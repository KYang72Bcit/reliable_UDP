import socket
import threading
import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation

# Global lists to store received data
packets_sent = []
packets_received = []

# Create a lock
data_lock = threading.Lock()

def update_graph(frame):
    with data_lock:  # Acquire lock before accessing the shared resources
        # Make copies of the lists
        sent = packets_sent[:]
        received = packets_received[:]
    
    # Clear and update the plot
    plt.cla()
    plt.plot(sent, label='Packets Sent')
    plt.plot(received, label='Packets Received')
    plt.legend(loc='upper left')
    plt.tight_layout()

def udp_server():
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server_socket.bind(('localhost', 65432))

    while True:
        data, addr = server_socket.recvfrom(1024)
        sent, received = map(int, data.decode().split(','))
        
        with data_lock:  # Acquire lock before accessing the shared resources
            packets_sent.append(sent)
            packets_received.append(received)

# Start UDP server in a new thread
threading.Thread(target=udp_server, daemon=True).start()

# Set up the graph to update with animation
plt.style.use('fivethirtyeight')
ani = FuncAnimation(plt.gcf(), update_graph, interval=1000)

# Show the plot
plt.tight_layout()
plt.show()
