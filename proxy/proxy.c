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
    int argc, writer_sockfd, receiver_sockfd;
    struct sockaddr_storage writer_addr, receiver_addr;
    char data_buffer[BUFFER_SIZE], ack_buffer[BUFFER_SIZE];
    ssize_t numBytes;
    socklen_t writer_addr_len, receiver_addr_len;
    bool data_received, ack_received;
} FSM;

static void parse_arguments(int argc, char *argv[], char **receiver_ip, in_port_t *writer_port, in_port_t *receiver_port, float *drop_data_chance, float *drop_ack_chance, float *delay_data_chance, float *delay_ack_chance);
static void handle_arguments(char *receiver_ip, in_port_t receiver_port, in_port_t writer_port);
in_port_t parse_in_port_t(const char *binary_name, const char *str);
static void convert_address(const char *address, struct sockaddr_storage *addr);
static int socket_create(int domain, int type, int protocol);
static int socket_bind(int sockfd, in_port_t port);
static bool receive_data(int writer_sockfd, struct sockaddr_storage writer_addr, socklen_t writer_addr_len, char *data_buffer, size_t *numBytes, FSM *fsm);
static bool receive_ack(int writer_sockfd, int receiver_sockfd, struct sockaddr_storage receiver_addr, socklen_t receiver_addr_len, FSM *fsm);
static bool drop_or_delay_data(float drop_chance, float delay_chance, char *data, size_t data_len);
static bool drop_or_delay_ack(float drop_chance, float delay_chance, char *ack, size_t ack_len);
static void forward_data(int receiver_sockfd, struct sockaddr_storage receiver_addr, socklen_t receiver_addr_len, const char *data, size_t data_len);
static void forward_ack(int writer_sockfd, struct sockaddr_storage writer_addr, socklen_t writer_addr_len, const char *ack, size_t ack_len, FSM *fsm);
static void cleanup(int writer_sockfd, int receiver_sockfd);
static void socket_close(int sockfd);

int main(int argc, char *argv[]) {
    FSM fsm_instance;
    FSM *fsm = &fsm_instance;
    memset(&fsm_instance, 0, sizeof(FSM));
    fsm->argc = argc;
    fsm->argv = argv;
    ProxyState currentState = INITIALIZE;
    fsm->writer_addr_len = sizeof(fsm->writer_addr);
    fsm->receiver_addr_len = sizeof(fsm->receiver_addr);
    srand(time(NULL));
    struct pollfd fds[2];
    int poll_result;
    int timeout =  3000; // Timeout in milliseconds

    while (currentState != TERMINATE) {
        if (currentState == INITIALIZE) {
            printf("Transitioned to INITIALIZE State.\n");
            if (argc != 8) {
                fprintf(stderr, "Expected usage: %s <Receiver IP> <Receiver Port> <Writer Port> <Drop Data %> <Drop Ack %> <Delay Data %> <Delay Ack %>\n", argv[0]);
                return EXIT_FAILURE;
            }

            parse_arguments(fsm->argc, fsm->argv, &fsm->receiver_ip, &fsm->writer_port, &fsm->receiver_port, &fsm->drop_data_chance, &fsm->drop_ack_chance, &fsm->delay_data_chance, &fsm->delay_ack_chance);
            handle_arguments(fsm->receiver_ip, fsm->receiver_port, fsm->writer_port);
            convert_address(fsm->receiver_ip, &fsm->receiver_addr);

            // Print out the parsed arguments
            printf("Receiver IP: %s\n", fsm->receiver_ip);
            printf("Receiver Port: %hu\n", fsm->receiver_port);
            printf("Writer Port: %hu\n", fsm->writer_port);
            printf("Drop Data Chance: %.2f%%\n", fsm->drop_data_chance);
            printf("Drop Ack Chance: %.2f%%\n", fsm->drop_ack_chance);
            printf("Delay Data Chance: %.2f%%\n", fsm->delay_data_chance);
            printf("Delay Ack Chance: %.2f%%\n\n", fsm->delay_ack_chance);
            
            fsm->writer_sockfd = socket_create(AF_INET, SOCK_DGRAM, 0);
            fsm->receiver_sockfd = socket_create(AF_INET, SOCK_DGRAM, 0);
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
            if (receive_data(fsm->writer_sockfd, fsm->writer_addr, fsm->writer_addr_len, fsm->data_buffer, &fsm->numBytes, fsm)) {
                currentState = FORWARDING_DATA;
            }
            fds[0].revents = 0; // Reset the revents field after handling the event
        }
        
        if (fds[1].revents & POLLIN) {
            if (receive_ack(fsm->writer_sockfd, fsm->receiver_sockfd, fsm->receiver_addr, fsm->receiver_addr_len, fsm)) {
                currentState = FORWARDING_ACK;
            }
            fds[1].revents = 0; // Reset the revents field after handling the event
        }

        switch(currentState) {
        case FORWARDING_DATA:
            printf("Forwarding data to receiver...\n");
            forward_data(fsm->receiver_sockfd, fsm->receiver_addr, fsm->receiver_addr_len, fsm->data_buffer, fsm->numBytes);
            currentState = WAITING_FOR_ACK;
            break;

        case FORWARDING_ACK:
            printf("Forwarding ACK to writer...\n");
            forward_ack(fsm->writer_sockfd, fsm->writer_addr, fsm->receiver_addr_len, fsm->ack_buffer, fsm->numBytes, fsm);
            currentState = WAITING_FOR_DATA;
            break;

        case HANDLE_ERROR:
            perror("Handling error");
            cleanup(fsm->writer_sockfd, fsm->receiver_sockfd);
            currentState = TERMINATE;
            break;

        case TERMINATE:
            break;
        }
    }
    cleanup(fsm->writer_sockfd, fsm->receiver_sockfd);
    return EXIT_SUCCESS;
}

// Function to parse and validate command-line arguments
static void parse_arguments(int argc, char *argv[], char **receiver_ip, in_port_t *writer_port, in_port_t *receiver_port, float *drop_data_chance, float *drop_ack_chance, float *delay_data_chance, float *delay_ack_chance) {
    if (argc != 8) {
        fprintf(stderr, "Incorrect number of arguments.\n");
    }

    *receiver_ip = argv[optind];
    printf("Parsing writer port...\n");
    *receiver_port = parse_in_port_t(argv[0], argv[optind + 1]);
    printf("Writer port parsed successfully.\n");
    printf("Parsing receiver port...\n");
    *writer_port = parse_in_port_t(argv[0], argv[optind + 2]);
    printf("Receiver port parsed successfully.\n");
    *drop_data_chance = atof(argv[optind + 3]);
    *drop_ack_chance = atof(argv[optind + 4]);
    *delay_data_chance = atof(argv[optind + 5]);
    *delay_ack_chance = atof(argv[optind + 6]);
}

// Function to process and validate the command-line arguments
static void handle_arguments(char *receiver_ip, in_port_t receiver_port, in_port_t writer_port) {
    printf("Checking arguments...\n");
    if (receiver_ip == NULL) {
        fprintf(stderr, "The receiver IP address is required.\n");
    }

    // Validate receiver port
    if (receiver_port > UINT16_MAX || receiver_port == 0) {
        fprintf(stderr, "Invalid receiver port.\n");
    }
    receiver_port = receiver_port;

    // Validate writer port
    if (writer_port > UINT16_MAX || writer_port == 0) {
        fprintf(stderr, "Invalid writer port.\n");
    }
    writer_port = writer_port;
    printf("Arguments checked.\n");
}

// Function to parse port from string
in_port_t parse_in_port_t(const char *binary_name, const char *str) {
  char *endptr;
  uintmax_t parsed_value;
  errno = 0;
  parsed_value = strtoull(str, &endptr, BASE_TEN);
  if (errno != 0) {
    perror("Error parsing in_port_t");
    exit(EXIT_FAILURE);
  }
  if (*endptr != '\0') {
    fprintf(stderr, "Invalid characters in input.\n");
  }
  if (parsed_value > UINT16_MAX) {
    fprintf(stderr, "in_port_t value out of range.\n");
  }
  return (in_port_t)parsed_value;
}

// Function to convert a string IP address to a sockaddr structure
static void convert_address(const char *address, struct sockaddr_storage *addr) {
    printf("Converting address...\n");
    memset(addr, 0, sizeof(*addr));
    char tmp_address[INET6_ADDRSTRLEN];
    strncpy(tmp_address, address, sizeof(tmp_address));
    tmp_address[sizeof(tmp_address) - 1] = '\0';

    struct sockaddr_in6 *addr_v6 = (struct sockaddr_in6 *)addr;
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

    if (inet_pton(AF_INET, tmp_address, &(((struct sockaddr_in *)addr)->sin_addr)) == 1) {
        addr->ss_family = AF_INET;
    } else if (inet_pton(AF_INET6, tmp_address, &addr_v6->sin6_addr) == 1) {
        addr->ss_family = AF_INET6;
    } else {
        fprintf(stderr, "%s is not a valid IPv4 or IPv6 address\n", address);
        exit(EXIT_FAILURE);
    }
    printf("Address converted successfully.\n\n");
}

// Function to create a socket and set options
static int socket_create(int domain, int type, int protocol) {
    int sockfd;
    int yes = 1;

    sockfd = socket(domain, type, protocol);
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

// Function to perform cleanup, particularly closing sockets
static void cleanup(int writer_sockfd, int receiver_sockfd) {
    printf("Cleaning up...\n");
    if (writer_sockfd != -1 && fcntl(writer_sockfd, F_GETFD) != -1) {
        socket_close(writer_sockfd);
        writer_sockfd = -1;
    }
    if (receiver_sockfd != -1 && fcntl(receiver_sockfd, F_GETFD) != -1) {
        socket_close(receiver_sockfd);
        receiver_sockfd = -1;
    }
    printf("Cleaned up.\n");
}

// Function to close a socket
static void socket_close(int sockfd) {
  printf("Closing socket...\n");
  if(close(sockfd) == -1)
  {
    perror("Error closing socket");
    exit(EXIT_FAILURE);
  }
  printf("Socket closed.\n");
}

static bool receive_data(int writer_sockfd, struct sockaddr_storage writer_addr, socklen_t writer_addr_len, char *data_buffer, size_t *numBytes, FSM *fsm) {
    *numBytes = recvfrom(writer_sockfd, data_buffer, BUFFER_SIZE - 1, 0, (struct sockaddr *)&writer_addr, &writer_addr_len);
    if (*numBytes == -1) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            // No data available yet
            return false;
        }
        perror("recvfrom failed");
        exit(EXIT_FAILURE);
    }
    fsm->writer_addr = writer_addr;
    fsm->writer_addr_len = writer_addr_len;

    data_buffer[*numBytes] = '\0';
    printf("Received %zd bytes: %s\n", *numBytes, data_buffer);

    if (!drop_or_delay_data(fsm->drop_data_chance, fsm->delay_data_chance, data_buffer, *numBytes)) {
        return true; // Data should be forwarded
    }
    return false; // Data is dropped or delayed
}


static bool receive_ack(int writer_sockfd, int receiver_sockfd, struct sockaddr_storage receiver_addr, socklen_t receiver_addr_len, FSM *fsm) {
    printf("Waiting to receive ACK...\n");
    fsm->numBytes = recvfrom(receiver_sockfd, fsm->ack_buffer, BUFFER_SIZE - 1, 0, (struct sockaddr *)&receiver_addr, &receiver_addr_len);
    if (fsm->numBytes == -1) {
        perror("recvfrom failed for ACK");
        exit(EXIT_FAILURE);
    }

    fsm->ack_buffer[fsm->numBytes] = '\0';
    printf("Received ACK: %s\n\n", fsm->ack_buffer);

    if (!drop_or_delay_ack(fsm->drop_ack_chance, fsm->delay_ack_chance, fsm->ack_buffer, fsm->numBytes)) {
        return true;
    }
    return false;
}

static bool drop_or_delay_data(float drop_chance, float delay_chance, char *data, size_t data_len) {
    float rand_percent = ((float)rand() / RAND_MAX) * 100;
    if (rand_percent < drop_chance) {
        printf("Data dropped.\n\n");
        return true;
    } else if (rand_percent < drop_chance + delay_chance) {
        // Delay the data packet
        int delay_time_ms = (int)((rand_percent - drop_chance) / delay_chance * 1000);
        printf("Delaying data for %d ms.\n\n", delay_time_ms);
        usleep(delay_time_ms * 1000); // Convert milliseconds to microseconds for usleep
    }
    return false;
}

static bool drop_or_delay_ack(float drop_chance, float delay_chance, char *ack, size_t ack_len) {
    float rand_percent = ((float)rand() / RAND_MAX) * 100;
    if (rand_percent < drop_chance) {
        printf("ACK dropped.\n\n");
        return true;
    } else if (rand_percent < drop_chance + delay_chance) {
        // Delay the ACK packet
        int delay_time_ms = (int)((rand_percent - drop_chance) / delay_chance * 1000);
        printf("Delaying ACK for %d ms.\n\n", delay_time_ms);
        usleep(delay_time_ms * 1000); // Convert milliseconds to microseconds for usleep
    }
    return false;
}

static void forward_data(int receiver_sockfd, struct sockaddr_storage receiver_addr, socklen_t receiver_addr_len, const char *data, size_t data_len) {
    // Attempt to send the data
    ssize_t numBytes = sendto(receiver_sockfd, data, data_len, 0, (struct sockaddr *)&receiver_addr, receiver_addr_len);
    if (numBytes < 0) {
        perror("sendto failed - error");
        printf("Error code: %d\n", errno);
        exit(EXIT_FAILURE);
    }

    printf("Data forwarded to receiver.\n\n");
}

static void forward_ack(int writer_sockfd, struct sockaddr_storage writer_addr, socklen_t writer_addr_len, const char *ack, size_t ack_len, FSM *fsm) {
    ssize_t numBytes = sendto(writer_sockfd, ack, ack_len, 0, (struct sockaddr *)&fsm->writer_addr, fsm->writer_addr_len);
    if (numBytes < 0) {
        perror("sendto failed for ACK");
        printf("Error code: %d\n", errno);
        exit(EXIT_FAILURE);
    }
    printf("ACK forwarded to writer.\n\n");
}
