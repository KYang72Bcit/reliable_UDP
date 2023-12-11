import socket
import pandas as pd
import matplotlib.pyplot as plt
from matplotlib.ticker import MaxNLocator
import json
import sys
import numpy as np
import threading 
import queue

if len(sys.argv) != 5:
    print("Usage: <script_name> <proxy ip> <writer ip> <receiver ip> <tcp port>")
    sys.exit(1)

PROXY_IP = sys.argv[1]
WRITER_IP = sys.argv[2]
RECEIVER_IP = sys.argv[3]
TCP_PORT = int(sys.argv[4])

threads = []
plot_queue = queue.Queue()
shutdown_flag = False

server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server_socket.bind(('', TCP_PORT)) 
server_socket.listen(5)
server_socket.settimeout(1.0)  

df = pd.DataFrame(columns=['Time Elapsed', 'Data Sent', 'Data Received', 'Correct Packets', 'ACK Sent', 'ACK Received',
                           'Data Dropped', 'Data Delayed', 'ACK Dropped', 'ACK Delayed'])
initial_data = {col: [0] for col in df.columns}
initial_data['Time Elapsed'] = ['00:00:00']
df = pd.DataFrame(initial_data)
df_lock = threading.Lock()

def parse_proxy_data(message):
    if message.strip() == "TERMINATE":
        return "TERMINATE"

    try:
        message = message.strip('\n')
        parts = [part for part in message.split(", ") if ": " in part]
        stats = dict(part.split(": ", 1) for part in parts)
        time_elapsed = format_time(stats.get('Time Elapsed', '00:00').strip())
        return {
            'Time Elapsed': time_elapsed,
            'Data Delayed': int(stats.get('Data Delayed', 0)),
            'Data Dropped': int(stats.get('Data Dropped', 0)),
            'ACK Delayed': int(stats.get('ACK Delayed', 0)),
            'ACK Dropped': int(stats.get('ACK Dropped', 0)),
        }
    except ValueError as e:
        print(f"Error in proxy data format: {e}")
        return None


def parse_writer_receiver_data(client_name, message):
    try:
        stats = json.loads(message)
        
        new_row = {
            'Data Sent': 0,
            'ACK Received': 0,
            'Data Received': 0,
            'ACK Sent': 0,
            'Correct Packets': 0,
        }

        if client_name == "Writer":
            new_row.update({
                'Data Sent': stats.get('Data Sent', 0),
                'ACK Received': stats.get('ACK Received', 0),
            })
        elif client_name == "Receiver":
            new_row.update({
                'Data Received': stats.get('Data Received', 0),
                'ACK Sent': stats.get('ACK Sent', 0),
                'Correct Packets': stats.get('Correct Packets', 0),
            })

        if 'Time Elapsed' in stats:
            time_elapsed = format_time(stats.get('Time Elapsed', '00:00').strip())
            new_row['Time Elapsed'] = time_elapsed

        return new_row
    except json.JSONDecodeError as e:
        print(f"Error parsing writer/receiver data: {e}")
        return None

    
def format_time(time_str):
    # Pad the time string with '00:' if it doesn't contain hours.
    if len(time_str.split(':')) == 2:
        time_str = '00:' + time_str
    return time_str

# Function to plot the final graph
def plot_final_graph(df):
    numeric_columns = ['Data Sent', 'Data Received', 'Correct Packets', 'ACK Sent', 
                       'ACK Received', 'Data Dropped', 'Data Delayed', 'ACK Dropped', 'ACK Delayed']
    for column in numeric_columns:
        df[column] = pd.to_numeric(df[column], errors='coerce').fillna(0).astype(int)
    
    if 'Time Elapsed' in df.columns:
        df['Time Elapsed'] = pd.to_timedelta(df['Time Elapsed'])
    else:
        print("'Time Elapsed' column is missing from the DataFrame")
    min_time = df['Time Elapsed'].min()
    df['Time Elapsed'] = df['Time Elapsed'] - min_time
    df['Time Elapsed'] = df['Time Elapsed'].dt.total_seconds().apply(lambda x: f"{int(x // 60):02d}:{int(x % 60):02d}")

    df.set_index('Time Elapsed', inplace=True)
    
    plt.figure(figsize=(10, 5))
    for column in numeric_columns:
        plt.plot(df.index, df[column], label=column, marker='o', linestyle='-')
    
    plt.xlabel('Time Elapsed (MM:SS)')
    plt.ylabel('Count')
    plt.title('Network Statistics Over Time')
    plt.legend()
    plt.tight_layout()
    plt.show()

def get_client_name(client_ip):
    if client_ip == PROXY_IP:
        return "Proxy"
    elif client_ip == WRITER_IP:
        return "Writer"
    elif client_ip == RECEIVER_IP:
        return "Receiver"
    else:
        return "Unknown"

def update_dataframe(client_name, new_data):
    global df
    with df_lock:
        time_elapsed = new_data['Time Elapsed']

        if time_elapsed in df['Time Elapsed'].values:
            for key, value in new_data.items():
                if pd.isna(df.loc[df['Time Elapsed'] == time_elapsed, key]).any():
                    df.loc[df['Time Elapsed'] == time_elapsed, key] = value
                else:
                    current_value = df.loc[df['Time Elapsed'] == time_elapsed, key].values[0]
                    if pd.isna(current_value) or current_value == 0:
                        df.loc[df['Time Elapsed'] == time_elapsed, key] = value
                    else:
                        #handle non-NaN, non-zero values
                        pass
        else:
            new_df = pd.DataFrame([new_data])
            df = pd.concat([df, new_df], ignore_index=True)

        numeric_columns = ['Data Sent', 'Data Received', 'Correct Packets', 'ACK Sent', 
                           'ACK Received', 'Data Dropped', 'Data Delayed', 'ACK Dropped', 'ACK Delayed']
        for column in numeric_columns:
            df[column] = pd.to_numeric(df[column], downcast='integer', errors='coerce').fillna(0).astype(int)

def handle_client_connection(client_socket, client_ip):
    global shutdown_flag, active_clients
    client_name = get_client_name(client_ip)

    while not shutdown_flag:
        try:
            data = client_socket.recv(1024)
            if not data:
                print(f"Connection closed by {client_name}")
                break

            message = data.decode('utf-8').strip()
            if "terminate" in message.lower():
                with df_lock:
                    df.fillna(0, inplace=True) 
                    numeric_columns = ['Data Sent', 'Data Received', 'Correct Packets', 'ACK Sent', 
                                       'ACK Received', 'Data Dropped', 'Data Delayed', 'ACK Dropped', 'ACK Delayed']
                    for column in numeric_columns:
                        df[column] = df[column].astype(int)
                    plot_queue.put(df.copy())  
                shutdown_flag = True
                for client in active_clients:
                    client.sendall("TERMINATE".encode('utf-8'))
                break
            else:
                new_row = None
                if client_name == "Proxy":
                    new_row = parse_proxy_data(message)
                elif client_name in ["Writer", "Receiver"]:
                    new_row = parse_writer_receiver_data(client_name, message)
                
                if new_row:
                    update_dataframe(client_name, new_row)
                    #print("\nCurrent DataFrame Statistics after recent update:")
                    #print(df)
                else:
                    print(f"Error: Received malformed data from {client_name}")

        except Exception as e:
            print(f"Error receiving data from client {client_name}: {e}")
            break

    with df_lock:
        if client_socket in active_clients:
            active_clients.remove(client_socket)

try:
    print(f"Listening for incoming connections on port {TCP_PORT}")
    active_clients = []
    while not shutdown_flag:
        try:
            client_socket, addr = server_socket.accept()
            client_ip = addr[0]
            client_name = get_client_name(client_ip)
            print(f"Connection from {client_name} at {client_ip}")
            
            active_clients.append(client_socket)  
            
            client_thread = threading.Thread(target=handle_client_connection, args=(client_socket, client_ip))
            client_thread.start()
            threads.append(client_thread)

        except socket.timeout:
            continue
        except Exception as e:
            print(f"Error accepting connections: {e}")
            break

finally:
    for client in list(active_clients):
        try:
            client.sendall("TERMINATE".encode('utf-8'))
        except OSError as e:
            print(f"Error sending TERMINATE signal to client socket: {e}")
        try:
            client.close()
        except OSError as e:
            print(f"Error closing client socket: {e}")
        finally:
            with df_lock:
                if client in active_clients:
                    active_clients.remove(client)  

    for thread in threads:
        thread.join()

    with df_lock:
        df.fillna(0, inplace=True)
        plot_queue.put(df)

    try:
        final_df = plot_queue.get_nowait()
        print("\nFinal DataFrame Statistics:")
        print(final_df)
        plot_final_graph(final_df)
    except queue.Empty:
        print("No data to plot.")

    server_socket.close()
    print("Server shutdown.")