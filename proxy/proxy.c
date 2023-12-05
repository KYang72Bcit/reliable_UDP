#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netdb.h>
#include <limits.h>
#include <pthread.h>
#include <poll.h>
#include <arpa/inet.h>
#include <time.h>
#include <errno.h>
#include <getopt.h>
#include <fcntl.h>
#include <sys/select.h>
#include <stdbool.h>
#include <signal.h>
#include <net/if.h>

// Define constants
#define UNKNOWN_OPTION_MESSAGE_LEN 24
#define BASE_TEN 10
#define BUFFER_SIZE 1024

typedef enum {
    INITIALIZE,
    PARSE_ARUGMENTS,
    SOCKET_CREATE,
    WAITING_FOR_DATA,
    FORWARDING_DATA,
    WAITING_FOR_ACK,
    FORWARDING_ACK,
    HANDLE_ERROR,
    TERMINATE
} ProxyState;

typedef struct {
    char *receiver_ip, **argv;
    in_port_t writer_port, receiver_port;
    float drop_data_chance, drop_ack_chance, delay_data_chance, delay_ack_chance;
    int argc, writer_sockfd, receiver_sockfd, data_forward, data_drop, data_delay, data_receive, ack_forward, ack_drop, ack_delay, ack_receive, current_packet_id, retransmission_count, total_retransmissions;
    struct sockaddr_storage writer_addr, receiver_addr;
    char data_buffer[BUFFER_SIZE], ack_buffer[BUFFER_SIZE];
    ssize_t numBytes;
    socklen_t writer_addr_len, receiver_addr_len;
    bool data_received, ack_received;
    const char *data, *ack;
    size_t data_len, ack_len;
} FSM;

static void parse_arguments(FSM *fsm);
static void handle_arguments(FSM *fsm);
in_port_t parse_in_port_t(FSM *fsm, const char *str);
static void convert_address(FSM *fsm);
static int socket_create(FSM *fsm);
static int socket_bind(int sockfd, in_port_t port);
static bool receive_data(FSM *fsm);
static bool receive_ack(FSM *fsm);
static bool drop_or_delay_data(FSM *fsm);
static bool drop_or_delay_ack(FSM *fsm);
static void forward_data(FSM *fsm);
static void forward_ack(FSM *fsm);
static void cleanup(FSM *fsm);
static int socket_close(int sockfd);
static void store_statistics(const char *filename, FSM *fsm);
static void signal_handler(int signum);
static void* write_statistics_periodically(void *arg);
const char* format_elapsed_time(double elapsed_seconds);

FSM *fsm;
time_t start_time;
pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;

int main(int argc, char *argv[]) {
    fsm = malloc(sizeof(FSM));  // Allocate memory for fsm
    if (fsm == NULL) {
        // Handle memory allocation failure
        fprintf(stderr, "Failed to allocate memory for FSM\n");
        return EXIT_FAILURE;
    }
    memset(fsm, 0, sizeof(FSM));  // Initialize the fsm structure
    fsm->argc = argc;
    fsm->argv = argv;
    ProxyState currentState = INITIALIZE;
    fsm->writer_addr_len = sizeof(fsm->writer_addr);
    fsm->receiver_addr_len = sizeof(fsm->receiver_addr);
    srand(time(NULL));
    struct pollfd fds[2];
    int poll_result;
    int timeout =  3000; // Timeout in milliseconds

    signal(SIGINT, signal_handler);

    // Initialize the start time
    start_time = time(NULL);

    // Start the statistics writing thread
    pthread_t stats_thread;
    if (pthread_create(&stats_thread, NULL, write_statistics_periodically, (void *)fsm) != 0) {
        perror("Failed to create the statistics thread");
        return EXIT_FAILURE;
    }

    while (currentState != TERMINATE) {
        if (currentState == INITIALIZE) {
            printf("Transitioned to INITIALIZE State.\n");
            if (argc != 8) {
                fprintf(stderr, "Expected usage: %s <Receiver IP> <Receiver Port> <Writer Port> <Drop Data %> <Drop Ack %> <Delay Data %> <Delay Ack %>\n", argv[0]);
                return EXIT_FAILURE;
            }

            parse_arguments(fsm);
            handle_arguments(fsm);
            convert_address(fsm);

            // Print out the parsed arguments
            printf("Receiver IP: %s\n", fsm->receiver_ip);
            printf("Receiver Port: %hu\n", fsm->receiver_port);
            printf("Writer Port: %hu\n", fsm->writer_port);
            printf("Drop Data Chance: %.2f%%\n", fsm->drop_data_chance);
            printf("Drop Ack Chance: %.2f%%\n", fsm->drop_ack_chance);
            printf("Delay Data Chance: %.2f%%\n", fsm->delay_data_chance);
            printf("Delay Ack Chance: %.2f%%\n\n", fsm->delay_ack_chance);
            
            fsm->writer_sockfd = socket_create(fsm);
            fsm->receiver_sockfd = socket_create(fsm);
            socket_bind(fsm->writer_sockfd, fsm->writer_port);
            ((struct sockaddr_in*)&fsm->receiver_addr)->sin_port = htons(fsm->receiver_port);

            fds[0].fd = fsm->writer_sockfd;
            fds[0].events = POLLIN;
            fds[1].fd = fsm->receiver_sockfd;
            fds[1].events = POLLIN;

            currentState = WAITING_FOR_DATA;
            printf("Waiting to receive data...\n");
        }

        // Poll the file descriptors for events
        poll_result = poll(fds, 2, timeout);

        if (poll_result < 0) {
            perror("poll error");
            currentState = HANDLE_ERROR;  // Transition to error handling
            continue;
        }

        if (poll_result == 0) {
            printf("Waiting to receive data...\n");
            currentState = WAITING_FOR_DATA;
            continue;
        }

        if (fds[0].revents & POLLIN) {
            if (receive_data(fsm)) {
                currentState = FORWARDING_DATA;
            }
            fds[0].revents = 0; // Reset the revents field after handling the event
        }
        
        if (fds[1].revents & POLLIN) {
            if (receive_ack(fsm)) {
                currentState = FORWARDING_ACK;
            }
            fds[1].revents = 0; // Reset the revents field after handling the event
        }

        switch(currentState) {
        case FORWARDING_DATA:
            printf("Forwarding data to receiver...\n");
            forward_data(fsm);
            currentState = WAITING_FOR_ACK;
            break;

        case FORWARDING_ACK:
            printf("Forwarding ACK to writer...\n");
            forward_ack(fsm);
            currentState = WAITING_FOR_DATA;
            break;

        case HANDLE_ERROR:
            perror("Handling error");
            cleanup(fsm);
            currentState = TERMINATE;
            break;

        case TERMINATE:
            break;
        }
    }

    store_statistics("Proxy Statistics.csv", fsm);
    cleanup(fsm);

    pthread_join(stats_thread, NULL);

    return EXIT_SUCCESS;
}

static void signal_handler(int signum) {
    if (signum == SIGINT) {
        store_statistics("Proxy Statistics.csv", fsm);
        cleanup(fsm);
        exit(EXIT_SUCCESS);
    }
}

// Function to parse and validate command-line arguments
static void parse_arguments(FSM *fsm) {
    if (fsm->argc != 8) {
        fprintf(stderr, "Incorrect number of arguments.\n");
    }

    fsm->receiver_ip = fsm->argv[optind];
    printf("Parsing writer port...\n");
    fsm->receiver_port = parse_in_port_t(fsm, fsm->argv[optind + 1]);
    printf("Writer port parsed successfully.\n");
    printf("Parsing receiver port...\n");
    fsm->writer_port = parse_in_port_t(fsm, fsm->argv[optind + 2]);
    printf("Receiver port parsed successfully.\n");
    fsm->drop_data_chance = atof(fsm->argv[optind + 3]);
    fsm->drop_ack_chance = atof(fsm->argv[optind + 4]);
    fsm->delay_data_chance = atof(fsm->argv[optind + 5]);
    fsm->delay_ack_chance = atof(fsm->argv[optind + 6]);
}

// Function to process and validate the command-line arguments
static void handle_arguments(FSM *fsm) {
    printf("Checking arguments...\n");
    if (fsm->receiver_ip == NULL) {
        fprintf(stderr, "The receiver IP address is required.\n");
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


// Function to bind a socket to an address
static int socket_bind(int sockfd, in_port_t port) {
    struct sockaddr_in6 addr6;
    struct sockaddr_in addr;

    memset(&addr6, 0, sizeof(addr6));
    addr6.sin6_family = AF_INET6;
    addr6.sin6_port = htons(port);
    addr6.sin6_addr = in6addr_any;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    // Try IPv6 first
    if (bind(sockfd, (struct sockaddr *)&addr6, sizeof(addr6)) == -1) {
        // Try IPv4 if IPv6 fails
        if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
            perror("bind failed");
            return -1;
        }
    }

    return 0; // Success
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
        fsm->retransmission_count = 0;
    } else {
        fsm->retransmission_count++;
        fsm->total_retransmissions++;
    }

    fsm->data_buffer[fsm->numBytes] = '\0';
    printf("Received %zd bytes: %s\n", fsm->numBytes, fsm->data_buffer);
    fsm->data_receive++;

    if (!drop_or_delay_data(fsm)) {
        return true; // Data should be forwarded
    }
    return false; // Data is dropped or delayed
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
        fsm->retransmission_count = 0;
    }

    fsm->ack_buffer[fsm->numBytes] = '\0';
    printf("Received ACK: %s\n\n", fsm->ack_buffer);
    fsm->ack_receive++;

    if (!drop_or_delay_ack(fsm)) {
        return true;
    }
    return false;
}

static bool drop_or_delay_data(FSM *fsm) {
    float rand_percent = ((float)rand() / RAND_MAX) * 100;
    if (rand_percent < fsm->drop_data_chance) {
        printf("Data dropped.\n\n");
        fsm->data_drop++;
        return true;
    } else if (rand_percent < fsm->drop_data_chance + fsm->delay_data_chance) {
        // Delay the data packet
        int delay_time_ms = (int)((rand_percent - fsm->drop_data_chance) / fsm->delay_data_chance * 1000);
        printf("Delaying data for %d ms.\n\n", delay_time_ms);
        fsm->data_delay++;
        usleep(delay_time_ms * 1000); // Convert milliseconds to microseconds for usleep
    }
    return false;
}

static bool drop_or_delay_ack(FSM *fsm) {
    float rand_percent = ((float)rand() / RAND_MAX) * 100;
    if (rand_percent < fsm->drop_ack_chance) {
        printf("ACK dropped.\n\n");
        fsm->ack_drop++;
        return true;
    } else if (rand_percent < fsm->drop_ack_chance + fsm->delay_ack_chance) {
        // Delay the ACK packet
        int delay_time_ms = (int)((rand_percent - fsm->drop_ack_chance) / fsm->delay_ack_chance * 1000);
        printf("Delaying ACK for %d ms.\n\n", delay_time_ms);
        fsm->ack_delay++;
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
    fsm->data_forward++;
    printf("Data forwarded to receiver.\n\n");
}

static void forward_ack(FSM *fsm) {
    ssize_t numBytes = sendto(fsm->writer_sockfd, fsm->ack_buffer, fsm->numBytes, 0, (struct sockaddr *)&fsm->writer_addr, fsm->writer_addr_len);
    if (numBytes < 0) {
        perror("sendto failed for ACK");
        printf("Error code: %d\n", errno);
        exit(EXIT_FAILURE);
    }
    fsm->ack_forward++;
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
    time_t last_write_time = start_time; // Initialize last write time as start time
    while (1) {
        // Calculate how much time to sleep to wake up at the next 30-second mark
        time_t current_time = time(NULL);
        int seconds_since_last_write = difftime(current_time, last_write_time);
        int sleep_time = 10 - (seconds_since_last_write % 10);
        sleep(sleep_time);
        
        pthread_mutex_lock(&stats_mutex);
        store_statistics("Proxy Statistics.csv", fsm);
        pthread_mutex_unlock(&stats_mutex);
        
        last_write_time = time(NULL); // Update last write time to current
    }
    return NULL;
}

// Modify this function to include elapsed time
static void store_statistics(const char *filename, FSM *fsm) {
    FILE *file = fopen(filename, "a"); // Open the file in append mode
    if (file == NULL) {
        perror("Failed to open statistics file");
        return;
    }

    // Check if the file is empty and write the header if it is
    fseek(file, 0, SEEK_END);
    size_t size = ftell(file);
    if (size == 0) {
        fprintf(file, "Time Elapsed,Data Forwarded,Data Dropped,Data Delayed,Data Received,ACK Forwarded,ACK Received,ACK Dropped,ACK Delayed,Retransmissions\n");
    }
    
    // Calculate elapsed time
    time_t now = time(NULL);
    double elapsed = difftime(now, start_time);
    const char* elapsed_time_str = format_elapsed_time(elapsed);
    
    // Include elapsed time in the CSV output
    fprintf(file, "%s,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
        elapsed_time_str,
        fsm->data_forward,
        fsm->data_drop,
        fsm->data_delay,
        fsm->data_receive,
        fsm->ack_forward,
        fsm->ack_receive,
        fsm->ack_drop,
        fsm->ack_delay,
        fsm->total_retransmissions
    );
    
    fclose(file);
    printf("Statistics stored successfully in CSV format.\n\n");
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
    free(fsm);
    // It's safe to free fsm here if it's no longer needed.
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
