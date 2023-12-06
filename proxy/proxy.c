#include <stdlib.h> // Include the standard library for basic functions like memory allocation.
#include <stdio.h> // Include standard input/output library for functions like printf.
#include <stdint.h> // Include the standard integer library for fixed-width integer types.
#include <string.h> // Include string handling functions.
#include <sys/socket.h> // Include functions for socket programming.
#include <unistd.h> // Include POSIX operating system API for functions like close().
#include <sys/types.h> // Include definitions of data types used in system calls.
#include <sys/stat.h> // Include definitions for file status.
#include <netdb.h> // Include definitions for network database operations.
#include <limits.h> // Include definitions of standard limits.
#include <pthread.h> // Include POSIX threads library.
#include <poll.h> // Include definitions for the poll() function.
#include <arpa/inet.h> // Include definitions for internet operations.
#include <time.h> // Include time handling functions.
#include <errno.h> // Include system error numbers.
#include <getopt.h> // Include functions to parse command-line options.
#include <fcntl.h> // Include file control options.
#include <sys/select.h> // Include select() function definitions.
#include <stdbool.h> // Include the standard boolean library.
#include <net/if.h> // Include definitions for network interface operations.
#include <signal.h> // Include signal handling functions.

// Define constants
#define UNKNOWN_OPTION_MESSAGE_LEN 24 // Define the length of unknown option message.
#define BASE_TEN 10 // Define base ten for numeric conversions.
#define BUFFER_SIZE 1024 // Define the size of the buffer used in the program.

static volatile sig_atomic_t shutdown_flag = 0; // Define a flag to handle program shutdown through signals.

typedef enum {
    INITIALIZE, // State for initialization.
    PARSE_ARUGMENTS, // State for parsing command-line arguments.
    SOCKET_CREATE, // State for creating a socket.
    WAITING_FOR_DATA, // State when waiting for data from the client.
    FORWARDING_DATA, // State when forwarding data to the server.
    WAITING_FOR_ACK, // State when waiting for an acknowledgement.
    FORWARDING_ACK, // State when forwarding an acknowledgement.
    HANDLE_ERROR, // State for handling errors.
    TERMINATE // State for program termination.
} ProxyState; // Define an enumeration for the states in the finite state machine.

typedef struct {
    ssize_t numBytes; // Number of bytes transmitted.
    time_t start_time; // Start time of the operation.
    ProxyState currentState; // Current state of the finite state machine.
    char *receiver_ip, *writer_ip, **argv; // Receiver IP address and argument vector.
    pthread_mutex_t stats_mutex; // Mutex for protecting shared data.
    in_port_t writer_port, receiver_port; // Ports for the writer and receiver.
    socklen_t writer_addr_len, receiver_addr_len; // Lengths of the writer and receiver addresses.
    struct sockaddr_storage writer_addr, receiver_addr; // Structures for storing writer and receiver addresses.
    char data_buffer[BUFFER_SIZE], ack_buffer[BUFFER_SIZE]; // Buffers for data and acknowledgements.
    float drop_data_chance, drop_ack_chance, delay_data_chance, delay_ack_chance; // Probabilities for dropping and delaying data/ack.
    int argc, sockfd, writer_sockfd, receiver_sockfd; // Argument count, socket file descriptors.
    int data_forwarded, data_dropped, data_delayed, data_received; // Counters for data operations.
    int ack_forwarded, ack_dropped, ack_delayed, ack_received, current_packet_id, retransmissioned, total_retransmissions; // Counters for ack operations and packet info.
} FSM; // Define a struct for the finite state machine.

static void setup_signal_handler(); // Declaration of a function to set up a signal handler.
static void initialize_fsm_instance(FSM *fsm, int argc, char *argv[], struct pollfd fds[]); // Declaration of a function to initialize the finite state machine instance.
static void start_polling_loop(FSM *fsm, struct pollfd fds[], pthread_t stats_thread); // Declaration of a function to start the polling loop.
static void finalize_fsm_instance(FSM *fsm, pthread_t stats_thread); // Declaration of a function to finalize the FSM instance.
static void signal_handler(int signum); // Declaration of a signal handler function.
static void parse_arguments(FSM *fsm); // Declaration of a function to parse command line arguments.
static void handle_arguments(FSM *fsm); // Declaration of a function to handle the parsed arguments.
in_port_t parse_in_port_t(FSM *fsm, const char *str); // Declaration of a function to parse a port number from a string.
static void convert_address(FSM *fsm); // Declaration of a function to convert address information.
static int socket_create(FSM *fsm); // Declaration of a function to create a socket.
static int socket_bind(int sockfd, const char* ip, in_port_t port);
ProxyState transition(FSM *fsm, struct pollfd fds[]); // Declaration of a function for state transitions in the FSM.
static bool receive_data(FSM *fsm); // Declaration of a function to receive data.
static bool drop_or_delay_data(FSM *fsm); // Declaration of a function to decide whether to drop or delay incoming data.
static void forward_data(FSM *fsm); // Declaration of a function to forward data.
static bool receive_ack(FSM *fsm); // Declaration of a function to receive an acknowledgement.
static bool drop_or_delay_ack(FSM *fsm); // Declaration of a function to decide whether to drop or delay acknowledgements.
static void forward_ack(FSM *fsm); // Declaration of a function to forward acknowledgements.
const char* format_elapsed_time(double elapsed_seconds); // Declaration of a function to format elapsed time.
static void* write_statistics_periodically(void *arg); // Declaration of a function to write statistics periodically to a file or console.
static bool store_statistics(const char *filename, FSM *fsm); // Declaration of a function to store statistics to a file.
static void cleanup(FSM *fsm); // Declaration of a function for cleaning up resources.
static int socket_close(int sockfd); // Declaration of a function to close a socket.

int main(int argc, char *argv[]) { // Main function of the program.
    FSM fsmInstance; // Instance of the FSM struct.
    struct pollfd fds[2]; // File descriptor array for polling.
    pthread_t stats_thread; // Thread for writing statistics.
    memset(&fsmInstance, 0, sizeof(FSM)); // Initialize fsmInstance to zeros.
    fsmInstance.argc = argc; // Store the argument count.
    fsmInstance.argv = argv; // Store the argument vector.
    fsmInstance.start_time = time(NULL); // Set the start time to current time.
    pthread_mutex_init(&fsmInstance.stats_mutex, NULL); // Initialize the mutex for the FSM instance.
    setup_signal_handler(); // Set up signal handling for the program.
    initialize_fsm_instance(&fsmInstance, argc, argv, fds); // Initialize the FSM instance with arguments.

    // Create a statistics thread.
    if (pthread_create(&stats_thread, NULL, write_statistics_periodically, (void *)&fsmInstance) != 0) {
        perror("Failed to create the statistics thread"); // Print an error if thread creation fails.
        exit(EXIT_FAILURE); // Exit with failure status if thread creation fails.
    }

    start_polling_loop(&fsmInstance, fds, stats_thread); // Start the main polling loop of the FSM.

    finalize_fsm_instance(&fsmInstance, stats_thread); // Finalize the FSM instance.

    return EXIT_SUCCESS; // Return successful exit status.
}

static void setup_signal_handler() {
    if (signal(SIGINT, signal_handler) == SIG_ERR) { // Set up the signal handler for SIGINT (Ctrl+C interrupt).
        perror("signal"); // Print an error message if signal setup fails.
        exit(EXIT_FAILURE); // Exit the program with a failure status if signal setup fails.
    }
}

static void initialize_fsm_instance(FSM *fsm, int argc, char *argv[], struct pollfd fds[]) {
    fsm->currentState = INITIALIZE; // Set the current state of the FSM to INITIALIZE.
    fsm->writer_addr_len = sizeof(fsm->writer_addr); // Set the length of the writer's address structure.
    fsm->receiver_addr_len = sizeof(fsm->receiver_addr); // Set the length of the receiver's address structure.
    srand(time(NULL)); // Seed the random number generator with the current time.

    signal(SIGINT, signal_handler); // Set the signal handler for SIGINT (repeated to ensure it's set).

    if (argc != 9) { // Check if the number of arguments is not equal to 8.
        fprintf(stderr, "Expected usage: %s <Receiver IP> <Receiver Port> <Writer IP> <Writer Port> <Drop Data %> <Drop Ack %> <Delay Data %> <Delay Ack %>\n", argv[0]);
        exit(EXIT_FAILURE); // If not, print an error message and exit with failure status.
    }

    parse_arguments(fsm); // Parse the command-line arguments.
    handle_arguments(fsm); // Handle the parsed arguments.
    convert_address(fsm); // Convert address-related arguments to appropriate formats.

    // Print out the parsed and handled arguments.
    printf("Receiver IP: %s\n", fsm->receiver_ip);
    printf("Receiver Port: %hu\n", fsm->receiver_port);
    printf("Writer IP: %s\n", fsm->writer_ip);
    printf("Writer Port: %hu\n", fsm->writer_port);
    printf("Drop Data Chance: %.2f%%\n", fsm->drop_data_chance);
    printf("Drop Ack Chance: %.2f%%\n", fsm->drop_ack_chance);
    printf("Delay Data Chance: %.2f%%\n", fsm->delay_data_chance);
    printf("Delay Ack Chance: %.2f%%\n\n", fsm->delay_ack_chance);

    // Bind the writer's socket
    fsm->writer_sockfd = socket_create(fsm);
    if (socket_bind(fsm->writer_sockfd, fsm->writer_ip, fsm->writer_port) == -1) {
        perror("Failed to bind writer socket");
        exit(EXIT_FAILURE);
    }

    // Bind the receiver's socket
    fsm->receiver_sockfd = socket_create(fsm);
    if (socket_bind(fsm->receiver_sockfd, fsm->receiver_ip, fsm->receiver_port) == -1) {
        perror("Failed to bind receiver socket");
        exit(EXIT_FAILURE);
    }
    //((struct sockaddr_in*)&fsm->receiver_addr)->sin_port = htons(fsm->receiver_port); // Set the receiver's port in the address structure.

    fds[0].fd = fsm->writer_sockfd; // Set the file descriptor for the writer's socket in the polling array.
    fds[0].events = POLLIN; // Set the event type to POLLIN (there is data to read).
    fds[1].fd = fsm->receiver_sockfd; // Set the file descriptor for the receiver's socket in the polling array.
    fds[1].events = POLLIN; // Set the event type to POLLIN (there is data to read).

    fsm->currentState = WAITING_FOR_DATA; // Change the FSM's current state to WAITING_FOR_DATA.
    printf("Initialization complete.\n"); // Print a message indicating that initialization is complete.
}

static void start_polling_loop(FSM *fsm, struct pollfd fds[], pthread_t stats_thread) {
    int poll_result; // Variable to store the result of the poll function.
    int timeout = 3000; // Set the timeout for poll in milliseconds.

    while (fsm->currentState != TERMINATE && !shutdown_flag) { // Continue looping until FSM state is TERMINATE or shutdown_flag is set.
        poll_result = poll(fds, 2, timeout); // Call poll function on fds with a timeout.

        // Check if poll returned -1 (error) and errno is EINTR (interrupted by a signal).
        if (poll_result == -1 && errno == EINTR) {
            if (shutdown_flag) { // If shutdown_flag is set during the signal interrupt.
                break; // Break the loop to terminate.
            }
            continue; // Continue the loop if the poll was interrupted by a signal.
        } else if (poll_result == -1) { // If poll error is not due to an interrupt.
            perror("poll error"); // Print the poll error.
            fsm->currentState = HANDLE_ERROR; // Set the FSM state to HANDLE_ERROR.
            continue; // Continue the loop to handle the error.
        }

        if (poll_result == 0) { // If poll times out (no file descriptor is ready).
            printf("Waiting to receive data...\n"); // Print waiting message.
            fsm->currentState = WAITING_FOR_DATA; // Set FSM state to WAITING_FOR_DATA.
            continue; // Continue the loop.
        }

        if (fds[0].revents & POLLIN) { // If there's data to read on fds[0].
            if (receive_data(fsm)) { // If data is successfully received.
                fsm->currentState = FORWARDING_DATA; // Set FSM state to FORWARDING_DATA.
            }
            fds[0].revents = 0; // Reset the revents field for fds[0] after handling the event.
        }

        if (fds[1].revents & POLLIN) { // If there's data to read on fds[1].
            if (receive_ack(fsm)) { // If ACK is successfully received.
                fsm->currentState = FORWARDING_ACK; // Set FSM state to FORWARDING_ACK.
            }
            fds[1].revents = 0; // Reset the revents field for fds[1] after handling the event.
        }

        fsm->currentState = transition(fsm, fds); // Call the transition function to change FSM state based on events.
    }

    if (shutdown_flag) { // If shutdown_flag is set.
        // Try to cancel the statistics thread gracefully.
        if (pthread_cancel(stats_thread) != 0) {
            perror("pthread_cancel"); // Print error if pthread_cancel fails.
        }
        // Wait for the statistics thread to finish.
        if (pthread_join(stats_thread, NULL) != 0) {
            perror("pthread_join"); // Print error if pthread_join fails.
        }
    }
}

static void finalize_fsm_instance(FSM *fsm, pthread_t stats_thread) {
    store_statistics("Proxy Statistics.csv", fsm);
    cleanup(fsm);
    pthread_cancel(stats_thread); // Make sure to cancel the thread if it's still running
    pthread_join(stats_thread, NULL); // Wait for the statistics thread to finish
}

static void signal_handler(int signum) {
    if (signum == SIGINT) {
        shutdown_flag = 1;
    }
}

// Function to parse and validate command-line arguments
static void parse_arguments(FSM *fsm) {
    if (fsm->argc != 9) {
        fprintf(stderr, "Incorrect number of arguments.\n");
        exit(EXIT_FAILURE);
    }

    fsm->receiver_ip = fsm->argv[1];
    printf("Parsing receiver port...\n");
    fsm->receiver_port = parse_in_port_t(fsm, fsm->argv[2]);
    printf("Receiver port parsed successfully.\n");
    fsm->writer_ip = fsm->argv[3];
    printf("Parsing writer port...\n");
    fsm->writer_port = parse_in_port_t(fsm, fsm->argv[4]);
    printf("Writer port parsed successfully.\n");
    fsm->drop_data_chance = atof(fsm->argv[5]);
    fsm->drop_ack_chance = atof(fsm->argv[6]);
    fsm->delay_data_chance = atof(fsm->argv[7]);
    fsm->delay_ack_chance = atof(fsm->argv[8]);
}


// Function to process and validate the command-line arguments
static void handle_arguments(FSM *fsm) {
    printf("Checking arguments...\n");
    if (fsm->receiver_ip == NULL) {
        fprintf(stderr, "The receiver IP address is required.\n");
        exit(EXIT_FAILURE);
    }

    if (fsm->writer_ip == NULL) {
        fprintf(stderr, "The writer IP address is required.\n");
        exit(EXIT_FAILURE);
    }

    // Validate receiver port
    if (fsm->receiver_port > UINT16_MAX || fsm->receiver_port == 0) {
        fprintf(stderr, "Invalid receiver port: %hu\n", fsm->receiver_port);
        exit(EXIT_FAILURE);
    }

    // Validate writer port
    if (fsm->writer_port > UINT16_MAX || fsm->writer_port == 0) {
        fprintf(stderr, "Invalid writer port: %hu\n", fsm->writer_port);
        exit(EXIT_FAILURE);
    }
    printf("Arguments checked.\n");
}

// Function to parse port from string
in_port_t parse_in_port_t(FSM *fsm, const char *str) {
    char *endptr;
    uintmax_t parsed_value;
    errno = 0;
    parsed_value = strtoull(str, &endptr, BASE_TEN);
    if (errno != 0) {
        perror("Error parsing in_port_t");
        exit(EXIT_FAILURE);
    }
    if (*endptr != '\0') {
        fprintf(stderr, "%s: Invalid characters in input for port parsing.\n", fsm->argv[0]);
        exit(EXIT_FAILURE);
    }
    if (parsed_value > UINT16_MAX) {
        fprintf(stderr, "%s: in_port_t value out of range.\n", fsm->argv[0]);
        exit(EXIT_FAILURE);
    }
    return (in_port_t)parsed_value;
}

// Function to convert a string IP address to a sockaddr structure
static void convert_address(FSM *fsm) {
    printf("Converting address...\n");
    memset(&fsm->receiver_addr, 0, sizeof(fsm->receiver_addr));
    char tmp_address[INET6_ADDRSTRLEN];
    strncpy(tmp_address, fsm->receiver_ip, sizeof(tmp_address));
    tmp_address[sizeof(tmp_address) - 1] = '\0';

    struct sockaddr_in6 *addr_v6 = (struct sockaddr_in6 *)&fsm->receiver_addr;
    char *percent_sign = strchr(tmp_address, '%');
    if (percent_sign != NULL) {
        *percent_sign = '\0';
        char *interface_name = percent_sign + 1;
        addr_v6->sin6_scope_id = if_nametoindex(interface_name);
        if (addr_v6->sin6_scope_id == 0) {
            perror("Invalid interface for IPv6 scope ID");
            exit(EXIT_FAILURE);
        }
    }

    if (inet_pton(AF_INET, tmp_address, &(((struct sockaddr_in *)&fsm->receiver_addr)->sin_addr)) == 1) {
        fsm->receiver_addr.ss_family = AF_INET;
    } else if (inet_pton(AF_INET6, tmp_address, &addr_v6->sin6_addr) == 1) {
        fsm->receiver_addr.ss_family = AF_INET6;
    } else {
        fprintf(stderr, "%s is not a valid IPv4 or IPv6 address\n", fsm->receiver_ip);
        exit(EXIT_FAILURE);
    }
    printf("Address converted successfully.\n\n");
}

// Function to create a socket and set options
static int socket_create(FSM *fsm) {
    int sockfd;
    int yes = 1;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0); // Assuming you always want to create an AF_INET, SOCK_DGRAM socket
    if (sockfd == -1) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Allow the socket to be quickly reused after the application closes
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
        perror("setsockopt failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // Set the socket to non-blocking mode
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    flags |= O_NONBLOCK;
    if (fcntl(sockfd, F_SETFL, flags) == -1) {
        perror("fcntl F_SETFL O_NONBLOCK");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    return sockfd;
}

// Function to bind a socket to an IP address and port
static int socket_bind(int sockfd, const char *ip, in_port_t port) {
    struct sockaddr_storage addr_storage = {0};
    struct sockaddr *addr = (struct sockaddr *)&addr_storage;
    socklen_t addr_len;

    // Check if IP is IPv4 or IPv6 and prepare the sockaddr structure
    if (strchr(ip, ':')) {  // IPv6
        struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&addr_storage;
        addr6->sin6_family = AF_INET6;
        addr6->sin6_port = htons(port);
        if (inet_pton(AF_INET6, ip, &addr6->sin6_addr) <= 0) {
            perror("inet_pton - IPv6");
            return -1;
        }
        addr_len = sizeof(struct sockaddr_in6);
    } else {  // IPv4
        struct sockaddr_in *addr4 = (struct sockaddr_in *)&addr_storage;
        addr4->sin_family = AF_INET;
        addr4->sin_port = htons(port);
        if (inet_pton(AF_INET, ip, &addr4->sin_addr) <= 0) {
            perror("inet_pton - IPv4");
            return -1;
        }
        addr_len = sizeof(struct sockaddr_in);
    }

    // Bind the socket to the address and port
    if (bind(sockfd, addr, addr_len) == -1) {
        perror("bind");
        return -1;
    }

    return 0;  // Success
}

ProxyState transition(FSM *fsm, struct pollfd fds[]) {
    switch(fsm->currentState) {
    case FORWARDING_DATA:
        printf("Forwarding data to receiver...\n");
        forward_data(fsm);
        return WAITING_FOR_ACK;

    case FORWARDING_ACK:
        printf("Forwarding ACK to writer...\n");
        forward_ack(fsm);
        return WAITING_FOR_DATA;

    case HANDLE_ERROR:
        perror("Handling error");
        cleanup(fsm);
        return TERMINATE;

    case TERMINATE:
        return TERMINATE;

    default: 
        return fsm->currentState;
    }
}

static bool receive_data(FSM *fsm) {
    int received_packet_id;
    fsm->numBytes = recvfrom(fsm->writer_sockfd, fsm->data_buffer, BUFFER_SIZE - 1, 0, (struct sockaddr *)&fsm->writer_addr, &fsm->writer_addr_len);
    if (fsm->numBytes == -1) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            // No data available yet
            return false;
        }
        perror("recvfrom failed");
        exit(EXIT_FAILURE);
    }
    fsm->writer_addr = fsm->writer_addr;
    fsm->writer_addr_len =fsm-> writer_addr_len;

    if (fsm->current_packet_id != received_packet_id) {
        fsm->current_packet_id = received_packet_id;
        fsm->retransmissioned = 0;
    } else {
        fsm->retransmissioned++;
        fsm->total_retransmissions++;
    }

    fsm->data_buffer[fsm->numBytes] = '\0';
    //printf("Received %zd bytes: %s\n", fsm->numBytes, fsm->data_buffer);
    printf("Received data.\n");
    fsm->data_received++;

    if (!drop_or_delay_data(fsm)) {
        return true; // Data should be forwarded
    }
    return false; // Data is dropped or delayed
}

static bool drop_or_delay_data(FSM *fsm) {
    float rand_percent = ((float)rand() / RAND_MAX) * 100;
    if (rand_percent < fsm->drop_data_chance) {
        printf("Data dropped.\n\n");
        fsm->data_dropped++;
        return true;
    } else if (rand_percent < fsm->drop_data_chance + fsm->delay_data_chance) {
        // Delay the data packet
        int delay_time_ms = (int)((rand_percent - fsm->drop_data_chance) / fsm->delay_data_chance * 1000);
        printf("Delaying data for %d ms.\n\n", delay_time_ms);
        fsm->data_delayed++;
        usleep(delay_time_ms * 1000); // Convert milliseconds to microseconds for usleep
    }
    return false;
}

static void forward_data(FSM *fsm) {
    // Attempt to send the data
    ssize_t numBytes = sendto(fsm->receiver_sockfd, fsm->data_buffer, fsm->numBytes, 0, (struct sockaddr *)&fsm->receiver_addr, fsm->receiver_addr_len);
    if (numBytes < 0) {
        perror("sendto failed - error");
        printf("Error code: %d\n", errno);
        exit(EXIT_FAILURE);
    }
    fsm->data_forwarded++;
    printf("Data forwarded to receiver.\n\n");
}

static bool receive_ack(FSM *fsm) {
    int acked_packet_id;
    printf("Waiting to receive ACK...\n");
    fsm->numBytes = recvfrom(fsm->receiver_sockfd, fsm->ack_buffer, BUFFER_SIZE - 1, 0, (struct sockaddr *)&fsm->receiver_addr, &fsm->receiver_addr_len);
    if (fsm->numBytes == -1) {
        perror("recvfrom failed for ACK");
        exit(EXIT_FAILURE);
    }

    if (fsm->current_packet_id == acked_packet_id) {
        fsm->current_packet_id = -1; // Reset current packet ID
        fsm->retransmissioned = 0;
    }

    fsm->ack_buffer[fsm->numBytes] = '\0';
    //printf("Received ACK: %s\n\n", fsm->ack_buffer);
    printf("Received ACK.\n");
    fsm->ack_received++;

    if (!drop_or_delay_ack(fsm)) {
        return true;
    }
    return false;
}

static bool drop_or_delay_ack(FSM *fsm) {
    float rand_percent = ((float)rand() / RAND_MAX) * 100;
    if (rand_percent < fsm->drop_ack_chance) {
        printf("ACK dropped.\n\n");
        fsm->ack_dropped++;
        return true;
    } else if (rand_percent < fsm->drop_ack_chance + fsm->delay_ack_chance) {
        // Delay the ACK packet
        int delay_time_ms = (int)((rand_percent - fsm->drop_ack_chance) / fsm->delay_ack_chance * 1000);
        printf("Delaying ACK for %d ms.\n\n", delay_time_ms);
        fsm->ack_delayed++;
        usleep(delay_time_ms * 1000); // Convert milliseconds to microseconds for usleep
    }
    return false;
}

static void forward_ack(FSM *fsm) {
    ssize_t numBytes = sendto(fsm->writer_sockfd, fsm->ack_buffer, fsm->numBytes, 0, (struct sockaddr *)&fsm->writer_addr, fsm->writer_addr_len);
    if (numBytes < 0) {
        perror("sendto failed for ACK");
        printf("Error code: %d\n", errno);
        exit(EXIT_FAILURE);
    }
    fsm->ack_forwarded++;
    printf("ACK forwarded to writer.\n");
}

// Function to convert elapsed time in seconds to HH:MM:SS format
const char* format_elapsed_time(double elapsed_seconds) {
    static char buffer[32];
    int minutes = ((int)elapsed_seconds % 3600) / 60;
    int seconds = (int)elapsed_seconds % 60;
    snprintf(buffer, 32, "%02d:%02d", minutes, seconds);
    return buffer;
}

// Function to write statistics periodically
static void* write_statistics_periodically(void *arg) {
    FSM *fsm = (FSM *)arg;
    while (!shutdown_flag) {
        sleep(10); // Adjust the sleep time as necessary for your needs

        pthread_mutex_lock(&fsm->stats_mutex);
        if (shutdown_flag) {
            // If shutdown_flag is set during the sleep, exit the loop immediately
            pthread_mutex_unlock(&fsm->stats_mutex);
            break;
        }
        bool success = store_statistics("Proxy Statistics.csv", fsm);
        pthread_mutex_unlock(&fsm->stats_mutex);
        if (!success) {
            break; // If storing statistics failed, exit the loop
        }
    }
    return NULL; // Exit the thread
}

// Modify this function to handle file operation errors and return success status
static bool store_statistics(const char *filename, FSM *fsm) {
    FILE *file = fopen(filename, "a");
    if (!file) {
        perror("Failed to open statistics file");
        return false; // Indicate failure
    }

    // Check if the file is empty and write the header if it is
    fseek(file, 0, SEEK_END);
    size_t size = ftell(file);
    if (size == 0) {
        fprintf(file, "Time Elapsed,Data Forwarded,Data Dropped,Data Delayed,Data Received,ACK Forwarded,ACK Received,ACK Dropped,ACK Delayed,Retransmissions\n");
    }
    
    // Calculate elapsed time
    time_t now = time(NULL);
    double elapsed = difftime(now, fsm->start_time);
    const char* elapsed_time_str = format_elapsed_time(elapsed);
    
    // Include elapsed time in the CSV output
    fprintf(file, "%s,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
        elapsed_time_str,
        fsm->data_forwarded,
        fsm->data_dropped,
        fsm->data_delayed,
        fsm->data_received,
        fsm->ack_forwarded,
        fsm->ack_received,
        fsm->ack_dropped,
        fsm->ack_delayed,
        fsm->total_retransmissions
    );
    
    if (fclose(file) == EOF) {
        perror("fclose failed");
        return false; // Indicate failure
    }

    printf("Statistics stored successfully.\n\n");
    return true; // Indicate success
}

// Function to perform cleanup, particularly closing sockets
static void cleanup(FSM *fsm) {
    printf("Cleaning up...\n");
    if (fsm->writer_sockfd != -1 && fcntl(fsm->writer_sockfd, F_GETFD) != -1) {
        printf("Closing writer socket...\n");
        if (socket_close(fsm->writer_sockfd) == -1) {
            printf("Error closing writer socket\n");
        }
    }
    if (fsm->receiver_sockfd != -1 && fcntl(fsm->receiver_sockfd, F_GETFD) != -1) {
        printf("Closing receiver socket...\n");
        if (socket_close(fsm->receiver_sockfd) == -1) {
            printf("Error closing receiver socket\n");
        }
    }
}

// Function to close a socket
static int socket_close(int sockfd) {
    if (close(sockfd) == -1) {
        perror("Error closing socket");
        return -1;
    }
    printf("Socket closed.\n");
    return 0;
}

