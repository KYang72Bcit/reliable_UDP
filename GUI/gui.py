import socket
import pandas as pd
import matplotlib.pyplot as plt
import json
import sys
import threading
import queue
import numpy as np

class ServerApplication:
    def __init__(self, proxy_ip, writer_ip, receiver_ip, tcp_port):
        self.proxy_ip = proxy_ip
        self.writer_ip = writer_ip
        self.receiver_ip = receiver_ip
        self.tcp_port = tcp_port
        self.threads = []
        self.plot_queue = queue.Queue()
        self.shutdown_flag = False
        self.active_clients = []
        self.df_lock = threading.Lock()
        self.initialize_dataframe()

        # Determine if the provided addresses are IPv4 or IPv6
        self.address_family = self.determine_address_family(proxy_ip, writer_ip, receiver_ip)

        # Create and configure the socket based on address family
        if self.address_family == socket.AF_INET:  # IPv4
            self.server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        elif self.address_family == socket.AF_INET6:  # IPv6
            self.server_socket = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
            self.server_socket.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)

        self.server_socket.bind(('', tcp_port))
        self.server_socket.listen(5)
        self.server_socket.settimeout(1.0)
    
    def determine_address_family(self, *addresses):
        """ Determine the address family based on the format of the provided addresses. """
        for address in addresses:
            try:
                socket.inet_pton(socket.AF_INET, address)
                return socket.AF_INET
            except socket.error:
                try:
                    socket.inet_pton(socket.AF_INET6, address)
                    return socket.AF_INET6
                except socket.error:
                    continue
        raise ValueError("Invalid IP address format")

    def initialize_dataframe(self):
        columns = ['Time Elapsed', 'Data Sent', 'Data Received', 'Correct Packets', 'ACK Sent', 'ACK Received',
                   'Data Dropped', 'Data Delayed', 'ACK Dropped', 'ACK Delayed']
        initial_data = {col: [0] if col != 'Time Elapsed' else ['00:00:00'] for col in columns}
        self.df = pd.DataFrame(initial_data)
        self.df_lock = threading.Lock()

    def parse_proxy_data(self, message):
        if message.strip() == "TERMINATE":
            return "TERMINATE"

        try:
            message = message.strip('\n')
            parts = [part for part in message.split(", ") if ": " in part]
            stats = dict(part.split(": ", 1) for part in parts)
            time_elapsed = self.format_time(stats.get('Time Elapsed', '00:00').strip())
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


    def parse_writer_receiver_data(self, client_name, message):
        try:
            stats = json.loads(message)
            new_row = {
                'Data Sent': stats.get('Data Sent', 0),
                'ACK Received': stats.get('ACK Received', 0),
                'Data Received': stats.get('Data Received', 0),
                'ACK Sent': stats.get('ACK Sent', 0),
                'Correct Packets': stats.get('Correct Packets', 0),
            }

            if 'Time Elapsed' in stats:
                time_elapsed = self.format_time(stats.get('Time Elapsed', '00:00').strip())
                new_row['Time Elapsed'] = time_elapsed

            return new_row
        except json.JSONDecodeError as e:
            print(f"Error parsing writer/receiver data: {e}")
            return None
    
    def format_time(self, time_str):
        if len(time_str.split(':')) == 2:
            time_str = '00:' + time_str
        return time_str

    def plot_final_graph(self, df):
        numeric_columns = [
            'Data Sent', 'Data Received', 'Correct Packets', 'ACK Sent',
            'ACK Received', 'Data Dropped', 'Data Delayed', 'ACK Dropped', 'ACK Delayed'
        ]
        for column in numeric_columns:
            df[column] = pd.to_numeric(df[column], errors='coerce').fillna(0).astype(int)

        if 'Time Elapsed' in df.columns:
            df['Time Elapsed'] = pd.to_timedelta(df['Time Elapsed']).dt.total_seconds()
            df.set_index('Time Elapsed', inplace=True)

        plt.figure(figsize=(10, 5))
        for i, column in enumerate(numeric_columns):
            plt.plot(df.index, df[column], label=column, marker='o', linestyle='-')

        # Set x-ticks to be every 10 seconds, but only label every 10 seconds
        x_ticks_major = np.arange(0, df.index.max() + 1, 10)  # Major ticks every 10 seconds
        x_tick_labels = [self.format_seconds_to_mmss(t) for t in x_ticks_major]

        ax = plt.gca()  # Get the current Axes instance
        ax.set_xticks(x_ticks_major)  # Set major ticks
        ax.set_xticklabels(x_tick_labels)  # Set major tick labels

        plt.xlabel('Time Elapsed (MM:SS)')
        plt.ylabel('Count')
        plt.title('Network Statistics Over Time')
        plt.legend()
        plt.tight_layout()
        plt.grid(True)
        plt.show()

    def format_seconds_to_mmss(self, seconds):
        """Convert seconds to MM:SS format."""
        return f"{int(seconds // 60):02d}:{int(seconds % 60):02d}"

    def get_client_name(self, client_ip):
        if client_ip == self.proxy_ip:
            return "Proxy"
        elif client_ip == self.writer_ip:
            return "Writer"
        elif client_ip == self.receiver_ip:
            return "Receiver"
        else:
            return "Unknown"

    def update_dataframe(self, client_name, new_data):
        with self.df_lock:
            time_elapsed = new_data['Time Elapsed']
            if time_elapsed in self.df['Time Elapsed'].values:
                for key, value in new_data.items():
                    if pd.isna(self.df.loc[self.df['Time Elapsed'] == time_elapsed, key]).any():
                        self.df.loc[self.df['Time Elapsed'] == time_elapsed, key] = value
                    else:
                        current_value = self.df.loc[self.df['Time Elapsed'] == time_elapsed, key].values[0]
                        if pd.isna(current_value) or current_value == 0:
                            self.df.loc[self.df['Time Elapsed'] == time_elapsed, key] = value
            else:
                new_df = pd.DataFrame([new_data])
                self.df = pd.concat([self.df, new_df], ignore_index=True)

            numeric_columns = ['Data Sent', 'Data Received', 'Correct Packets', 'ACK Sent',
                               'ACK Received', 'Data Dropped', 'Data Delayed', 'ACK Dropped', 'ACK Delayed']
            for column in numeric_columns:
                self.df[column] = pd.to_numeric(self.df[column], downcast='integer', errors='coerce').fillna(0).astype(int)

    def handle_client_connection(self, client_socket, client_ip):
        client_name = self.get_client_name(client_ip)

        while not self.shutdown_flag:
            try:
                data = client_socket.recv(1024)
                if not data:
                    print(f"Connection closed by {client_name}")
                    break

                message = data.decode('utf-8').strip()
                if "terminate" in message.lower():
                    with self.df_lock:
                        self.df.fillna(0, inplace=True)
                        numeric_columns = ['Data Sent', 'Data Received', 'Correct Packets', 'ACK Sent',
                                           'ACK Received', 'Data Dropped', 'Data Delayed', 'ACK Dropped', 'ACK Delayed']
                        for column in numeric_columns:
                            self.df[column] = self.df[column].astype(int)
                        self.plot_queue.put(self.df.copy())
                    self.shutdown_flag = True
                    for client in self.active_clients:
                        client.sendall("TERMINATE".encode('utf-8'))
                    break
                else:
                    new_row = self.parse_proxy_data(message) if client_name == "Proxy" else self.parse_writer_receiver_data(client_name, message)
                    if new_row:
                        self.update_dataframe(client_name, new_row)
                    else:
                        print(f"Error: Received malformed data from {client_name}")

            except Exception as e:
                print(f"Error receiving data from client {client_name}: {e}")
                break

        with self.df_lock:
            if client_socket in self.active_clients:
                self.active_clients.remove(client_socket)

    def run(self):
        print(f"Listening for incoming connections on port {self.tcp_port}")
        try:
            while not self.shutdown_flag:
                try:
                    client_socket, addr = self.server_socket.accept()
                    client_ip = addr[0]
                    print(f"IP : {client_ip}")
                    client_name = self.get_client_name(client_ip)
                    print(f"Connection from {client_name} at {client_ip}")

                    self.active_clients.append(client_socket)
                    client_thread = threading.Thread(target=self.handle_client_connection,
                                                     args=(client_socket, client_ip))
                    client_thread.start()
                    self.threads.append(client_thread)

                except socket.timeout:
                    continue
                except Exception as e:
                    print(f"Error accepting connections: {e}")
                    break
        finally:
            self.shutdown_server()

    def shutdown_server(self):
        print("Shutting down server...")
        for client in list(self.active_clients):
            try:
                client.sendall("TERMINATE".encode('utf-8'))
            except OSError as e:
                print(f"Error sending TERMINATE signal to client socket: {e}")
            finally:
                try:
                    client.close()
                except OSError as e:
                    print(f"Error closing client socket: {e}")
        
        with self.df_lock:
            self.active_clients.clear()

        for thread in self.threads:
            thread.join()

        with self.df_lock:
            self.df.fillna(0, inplace=True)
            self.plot_queue.put(self.df)
        
        try:
            final_df = self.plot_queue.get_nowait()
            print("\nFinal DataFrame Statistics:")
            print(final_df)
            self.plot_final_graph(final_df)
        except queue.Empty:
            print("No data to plot.")

        self.server_socket.close()

if __name__ == "__main__":
    if len(sys.argv) != 5:
        print("Usage: <script_name> <proxy ip> <writer ip> <receiver ip> <tcp port>")
        sys.exit(1)

    server_app = ServerApplication(sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]))
    server_app.run()