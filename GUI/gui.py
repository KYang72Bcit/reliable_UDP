import socket
import threading
import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation
import matplotlib.dates as mdates
import datetime
import matplotlib.ticker as ticker

# Global lists to store received data
packets_sent = []
packets_received = []
data_dropped = []
data_delayed = []
ack_sent = []
ack_received = []
ack_dropped = []
ack_delayed = []
times = []

# Create a lock
data_lock = threading.Lock()

def update_graph(frame):
    with data_lock:  # Acquire lock before accessing the shared resources
        # Make copies of the lists
        times_copy = times[:]
        sent_copy = packets_sent[:]
        received_copy = packets_received[:]
        dropped_copy = data_dropped[:]
        delayed_copy = data_delayed[:]
        ack_sent_copy = ack_sent[:]
        ack_received_copy = ack_received[:]
        ack_dropped_copy = ack_dropped[:]
        ack_delayed_copy = ack_delayed[:]
    
    # Clear and update the plot
    plt.cla()
    plt.plot(times_copy, sent_copy, label='Packets Sent', color='blue')
    plt.plot(times_copy, received_copy, label='Packets Received', color='green')
    plt.plot(times_copy, dropped_copy, label='Data Dropped', color='red')
    plt.plot(times_copy, delayed_copy, label='Data Delayed', color='orange')
    plt.plot(times_copy, ack_sent_copy, label='ACK Sent', color='purple')
    plt.plot(times_copy, ack_received_copy, label='ACK Received', color='brown')
    plt.plot(times_copy, ack_dropped_copy, label='ACK Dropped', color='pink')
    plt.plot(times_copy, ack_delayed_copy, label='ACK Delayed', color='gray')

    plt.legend(loc='upper left')
    plt.xlabel('Time')
    plt.ylabel('Count')
    plt.xticks(rotation=45)
    plt.tight_layout()

    # Set Y-axis to increment by 1
    plt.gca().yaxis.set_major_locator(ticker.MultipleLocator(1))
    # Set X-axis to display time in minutes with increments of 3 minutes
    plt.gca().xaxis.set_major_formatter(mdates.DateFormatter('%H:%M'))
    plt.gca().xaxis.set_major_locator(mdates.MinuteLocator(interval=3))

def udp_server():
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server_socket.bind(('localhost', 65432))

    while True:
        data, addr = server_socket.recvfrom(1024)
        stats = list(map(int, data.decode().split(',')))
        
        with data_lock:  # Acquire lock before accessing the shared resources
            packets_sent.append(stats[0])
            packets_received.append(stats[1])
            data_dropped.append(stats[2])
            data_delayed.append(stats[3])
            ack_sent.append(stats[4])
            ack_received.append(stats[5])
            ack_dropped.append(stats[6])
            ack_delayed.append(stats[7])
            times.append(datetime.datetime.now())

# Start UDP server in a new thread
threading.Thread(target=udp_server, daemon=True).start()

# Set up the graph to update with animation
plt.style.use('fivethirtyeight')
ani = FuncAnimation(plt.gcf(), update_graph, interval=1000)

# Show the plot
plt.tight_layout()
plt.show()
