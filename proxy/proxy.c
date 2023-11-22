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

// Define constants 
#define UNKNOWN_OPTION_MESSAGE_LEN 24
#define BASE_TEN 10
#define BUFFER_SIZE 1024
#define GUI_IP "127.0.0.1" // Replace with the IP of the machine running the Python GUI
#define GUI_PORT 65432     // Replace with the port on which the Python GUI is listening

volatile sig_atomic_t signal_received = 0;

// Enum for FSM states
typedef enum {
  INITIALIZE,
  SOCKET_CREATE,
  BINDING,
  THREAD_CREATE,
  THREAD_JOIN,
  IDLE,
  CLEAN_UP,
  HANDLE_ERROR
} ProxyState;

typedef struct {
    ProxyState currentState;
    int argc;
    char **argv, *receiver_ip, *receiver_port, *writer_port;
    in_port_t port;                    
    struct sockaddr_storage addr;       
    pthread_t t_idle, t_data, t_ack; 
    ProxyData proxy_data;  
} FSM;

typedef struct {
    struct sockaddr_storage receiver_addr, writer_addr;
    float drop_data_chance, delay_data_chance, drop_ack_chance, delay_ack_chance;
    int writer_sockfd, receiver_sockfd, delay_ack_decision, delay_data_decision, drop_ack_decision, drop_data_decision;
    Statistics *stats;
    FSM *fsm;
} ProxyData;

typedef struct {
    unsigned long data_sent, data_received, data_dropped, data_delayed, ack_sent, ack_received, ack_dropped, ack_delayed;         
    pthread_mutex_t stats_mutex;   
} Statistics;

static void parse_arguments(FSM *fsm, ProxyData *proxy_data);
static void handle_arguments(const char *binary_name, FSM *fsm);
in_port_t parse_in_port_t(const char *binary_name, const char *str);
_Noreturn static void usage(const char *program_name, int exit_code, const char *message);
static void convert_address(const char *address, struct sockaddr_storage *addr);
static int socket_create(int domain, int type, int protocol);
static void socket_bind(int sockfd, struct sockaddr_storage *addr, in_port_t port);
static void transition(FSM *fsm, ProxyData *proxy_data);
static void *idle_handler(void *arg);
static void *ack_handler(void *arg);
static void *data_handler(void *arg);
static void receive_data(ProxyData *proxy_data);
static void receive_ack(ProxyData *proxy_data);
static void drop_or_delay(ProxyData *proxy_data);
static void forward_data(ProxyData *proxy_data, const char *data, size_t data_len);
static void forward_ack(ProxyData *proxy_data, const char *data, size_t data_len);
static void store_statistics(const char *filename, Statistics *stats);
static void send_statistics(ProxyData *proxy_data);
static void cleanup(ProxyData *proxy_data);
static void socket_close(int sockfd);

int main(int argc, char *argv[]) {
    FSM fsm; 
    fsm.currentState = INITIALIZE; 
    fsm.argc = argc;              
    fsm.argv = argv;              
    Statistics stats = {0, 0, PTHREAD_MUTEX_INITIALIZER}; 
    ProxyData proxy_data = {0};    
    proxy_data.stats = &stats;     

    // Check for minimum required arguments
    if (argc < 8) {
        usage(argv[0], EXIT_FAILURE, "Usage: <Receiver IP> <Receiver to send to> <Writer Port> <Drop Data %> <Drop Ack %> <Delay Data %> <Delay Ack %>");
        exit(EXIT_FAILURE);
    }

    // Register signal handler, Handle Ctrl+C
    signal(SIGINT, handle_signal);  
    // Handle termination signal
    signal(SIGTERM, handle_signal); 

    cleanup(&proxy_data); 

    return fsm.currentState == CLEAN_UP ? EXIT_SUCCESS : EXIT_FAILURE;
}

// Signal handler function
static void handle_signal(int sig) {
    signal_received = 1;
}

// Function to parse and validate command-line arguments
static void parse_arguments(FSM *fsm, ProxyData *proxy_data) {
    int opt;
    opterr = 0;

    while((opt = getopt(fsm->argc, fsm->argv, "h")) != -1) {
        switch(opt) {
        case 'h':
            usage(fsm->argv[0], EXIT_SUCCESS, NULL);
            break;
        case '?':
            char message[UNKNOWN_OPTION_MESSAGE_LEN];
            snprintf(message, sizeof(message), "Unknown option '-%c'.", optopt);
            usage(fsm->argv[0], EXIT_FAILURE, message);
            break;
        default:
            usage(fsm->argv[0], EXIT_FAILURE, NULL);
            break;
        }
    }

    // Check for correct number of arguments
    if (fsm->argc < 8) {
        usage(fsm->argv[0], EXIT_FAILURE, "Incorrect number of arguments.");
    }

    *fsm->receiver_ip = fsm->argv[optind];
    *fsm->receiver_port = parse_in_port_t(fsm->argv[0], fsm->argv[optind + 1]);
    *fsm->writer_port = parse_in_port_t(fsm->argv[0], fsm->argv[optind + 2]);
    proxy_data->drop_data_chance = atof(fsm->argv[optind + 3]);
    proxy_data->drop_ack_chance = atof(fsm->argv[optind + 4]);
    proxy_data->delay_data_chance = atof(fsm->argv[optind + 5]);
    proxy_data->delay_ack_chance = atof(fsm->argv[optind + 6]);
}

// Function to process and validate the command-line arguments
static void handle_arguments(const char *binary_name, FSM *fsm) {
    // Check if receiver IP address is provided
    if (fsm->receiver_ip == NULL) {
        usage(binary_name, EXIT_FAILURE, "The receiver IP address is required.");
    }

    // Validate receiver port
    if (fsm->receiver_port > UINT16_MAX || fsm->receiver_port == 0) {
        usage(binary_name, EXIT_FAILURE, "Invalid receiver port.");
    }
    fsm->receiver_port = fsm->receiver_port;

    // Validate writer port
    if (fsm->writer_port > UINT16_MAX || fsm->writer_port == 0) {
        usage(binary_name, EXIT_FAILURE, "Invalid writer port.");
    }
    fsm->writer_port = fsm->writer_port;
}

// Function to parse port from string
in_port_t parse_in_port_t(const char *binary_name, const char *str) {
  char *endptr;
  uintmax_t parsed_value;
  errno = 0;
  parsed_value = strtoumax(str, &endptr, BASE_TEN);
  if (errno != 0) {
    perror("Error parsing in_port_t");
    exit(EXIT_FAILURE);
  }
  if (*endptr != '\0') {
    usage(binary_name, EXIT_FAILURE, "Invalid characters in input.");
  }
  if (parsed_value > UINT16_MAX) {
    usage(binary_name, EXIT_FAILURE, "in_port_t value out of range.");
  }
  return (in_port_t)parsed_value;
}

// Function to display usage information and exit
_Noreturn static void usage(const char *program_name, int exit_code, const char *message) {
    if(message) {
        fprintf(stderr, "%s\n", message);
    }
    fprintf(stderr, "Usage: %s [-h] <Receiver IP> <Receiver Port> <Writer Port> <Drop Data %> <Drop Ack %> <Delay Data %> <Delay Ack %>\n", program_name);
    fputs("Options:\n", stderr);
    fputs("  -h  Display this help message\n", stderr);
    exit(exit_code);
}

// Function to convert a string IP address to a sockaddr structure
static void convert_address(const char *address, struct sockaddr_storage *addr) {
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
}

// Function to create a socket
static int socket_create(int domain, int type, int protocol) {
  int sockfd;
  sockfd = socket(domain, type, protocol);
  if (sockfd == -1) {
    perror("Socket creation failed");
    exit(EXIT_FAILURE);
  }
  return sockfd;
}

// Function to bind a socket to an address
static void socket_bind(int sockfd, struct sockaddr_storage *addr, in_port_t port) {
  char addr_str[INET6_ADDRSTRLEN];
  socklen_t addr_len;
  void *vaddr;
  in_port_t net_port;
  net_port = htons(port);

  // Set up the correct address structure based on the family (IPv4 or IPv6)
  if(addr->ss_family == AF_INET) {
    struct sockaddr_in *ipv4_addr;
    ipv4_addr = (struct sockaddr_in *)addr;
    addr_len = sizeof(*ipv4_addr);
    ipv4_addr->sin_port = net_port;
    vaddr = (void *)&(((struct sockaddr_in *)addr)->sin_addr);
  }
  else if(addr->ss_family == AF_INET6) {
    struct sockaddr_in6 *ipv6_addr;
    ipv6_addr = (struct sockaddr_in6 *)addr;
    addr_len = sizeof(*ipv6_addr);
    ipv6_addr->sin6_port = net_port;
    vaddr = (void *)&(((struct sockaddr_in6 *)addr)->sin6_addr);
  }
  else {
    fprintf(stderr, "Internal error: addr->ss_family must be AF_INET or AF_INET6, was: %d\n", addr->ss_family);
    exit(EXIT_FAILURE);
  }
  // Convert binary address to string for display
  if(inet_ntop(addr->ss_family, vaddr, addr_str, sizeof(addr_str)) == NULL) {
    perror("inet_ntop");
    exit(EXIT_FAILURE);
  }
  printf("Binding to: %s:%u\n", addr_str, port);

  // Bind the socket
  if(bind(sockfd, (struct sockaddr *)addr, addr_len) == -1) {
    perror("Binding failed");
    fprintf(stderr, "Error code: %d\n", errno);
    exit(EXIT_FAILURE);
  }

  printf("Bound to socket: %s:%u\n", addr_str, port);
}

// State transition function for FSM
static void transition(FSM *fsm, ProxyData *proxy_data) {
  switch(fsm->currentState) {
  case INITIALIZE:
    // Parse and handle arguments to set receiver IP and ports
    parse_arguments(fsm, proxy_data); 
    handle_arguments(fsm, fsm->proxy_data);
    fsm->currentState = SOCKET_CREATE;
    break;
    case SOCKET_CREATE:
      // Create sockets for writer and receiver
      proxy_data->writer_sockfd = socket_create(AF_INET, SOCK_DGRAM, 0);
      proxy_data->receiver_sockfd = socket_create(AF_INET, SOCK_DGRAM, 0);
      fsm->currentState = BINDING;
      break;
    case BINDING:
      // Convert receiver address and bind receiver socket
      convert_address(fsm->receiver_ip, &(fsm->addr));
      socket_bind(proxy_data->receiver_sockfd, &(fsm->addr), fsm->receiver_port);
      fsm->currentState = THREAD_CREATE;
      break;
  case THREAD_CREATE:
    // Thread creation for idle, resend, and ack handlers
    if (pthread_create(&fsm->t_idle, NULL, idle_handler, &fsm->proxy_data) != 0) {
        perror("Failed to create idle handler thread");
        exit(EXIT_FAILURE);
    }
    if (pthread_create(&fsm->t_data, NULL, data_handler, &fsm->proxy_data) != 0) {
        perror("Failed to create data handler thread");
        exit(EXIT_FAILURE);
    }
    if (pthread_create(&fsm->t_ack, NULL, ack_handler, &fsm->proxy_data) != 0) {
        perror("Failed to create ack handler thread");
        exit(EXIT_FAILURE);
    }
    fsm->currentState = THREAD_JOIN;
    break;
  case THREAD_JOIN:
    // Joining threads
    pthread_join(fsm->t_idle, NULL);
    pthread_join(fsm->t_data, NULL);
    pthread_join(fsm->t_ack, NULL);
    fsm->currentState = IDLE;
    break;
  case IDLE:
    idle_handler(&(fsm->proxy_data));
    break;
  case CLEAN_UP:
    cleanup(proxy_data);
    break;
  case HANDLE_ERROR:
    fsm->currentState = CLEAN_UP;
    break;
  default:
    fprintf(stderr, "Unexpected FSM state\n");
    fsm->currentState = HANDLE_ERROR;
    break;
  }
}

static void *idle_handler(void *arg) {
    ProxyData *proxy_data = (ProxyData *)arg;
    fd_set readfds;
    struct timeval timeout;
    int max_sd;

    fcntl(proxy_data->writer_sockfd, F_SETFL, O_NONBLOCK);
    fcntl(proxy_data->receiver_sockfd, F_SETFL, O_NONBLOCK);
    
    while (1) {
        FD_ZERO(&readfds);

        FD_SET(proxy_data->writer_sockfd, &readfds);
        FD_SET(proxy_data->receiver_sockfd, &readfds);
        max_sd = (proxy_data->writer_sockfd > proxy_data->receiver_sockfd) ? proxy_data->writer_sockfd : proxy_data->receiver_sockfd;

        timeout.tv_sec = 1; 
        timeout.tv_usec = 0;

        int activity = select(max_sd + 1, &readfds, NULL, NULL, &timeout);

        if ((activity < 0) && (errno != EINTR)) {
            perror("select error");
            break;
        }

        if (FD_ISSET(proxy_data->writer_sockfd, &readfds)) {
            receive_data(proxy_data);
        }

        if (FD_ISSET(proxy_data->receiver_sockfd, &readfds)) {
            receive_ack(&(proxy_data->receiver_addr));
        }

        // Check if signal received
        if (signal_received) {
            printf("Signal received, transitioning to cleanup.\n");
            proxy_data->fsm->currentState = CLEAN_UP;
            break;
        }
    }

    cleanup(proxy_data);

    return NULL;
}

// Thread function for handling acknowledgment
static void *ack_handler(void *arg) {
    ProxyData *proxy_data = (ProxyData *)arg;

    while (1) {
        drop_or_delay(proxy_data);

        if (proxy_data->drop_ack_decision) {
            continue;
        }

        if (proxy_data->delay_ack_decision) {
            sleep(5);
        }
        receive_ack(proxy_data);
    }

    return NULL;
}

// Thread function for handling data
static void *data_handler(void *arg) {
    ProxyData *proxy_data = (ProxyData *)arg;

    while (1) {
        drop_or_delay(proxy_data); 

        if (proxy_data->drop_data_decision) {
            continue;
        }

        if (proxy_data->delay_data_decision) {
            sleep(5);
        }

        receive_data(proxy_data);
    }

    return NULL;
}

// Function to indefinitely listen for data sent from the writer
static void receive_data(ProxyData *proxy_data) {
    char buffer[BUFFER_SIZE];
    socklen_t addr_len = sizeof(proxy_data->writer_addr);

    while (1) {
        ssize_t numBytes = recvfrom(proxy_data->writer_sockfd, buffer, BUFFER_SIZE, 0, &proxy_data->writer_addr, &addr_len);
        if (numBytes < 0) {
            perror("recvfrom failed");
            continue;
        } else {
            pthread_mutex_lock(&proxy_data->stats->stats_mutex);
            proxy_data->stats->data_received++;
            pthread_mutex_unlock(&proxy_data->stats->stats_mutex);
        }

        forward_data(proxy_data, buffer, numBytes);
    }
}

// Function to indefinitely listen for ack sent from the receiver
static void receive_ack(ProxyData *proxy_data) {
    char buffer[BUFFER_SIZE];
    socklen_t addr_len = sizeof(proxy_data->writer_addr);

    int drop_ack_decision, delay_ack_decision;

    while (1) {
        ssize_t numBytes = recvfrom(proxy_data->receiver_sockfd, buffer, BUFFER_SIZE, 0, &proxy_data->writer_addr, &addr_len);
        if (numBytes < 0) {
            perror("recvfrom failed");
            continue;
        } else {
            pthread_mutex_lock(&proxy_data->stats->stats_mutex);
            proxy_data->stats->ack_received++;
            pthread_mutex_unlock(&proxy_data->stats->stats_mutex);
        }

        forward_ack(proxy_data, buffer, numBytes);
    }
}

// Function to determine whether to drop or delay data/ack based on given chances
static void drop_or_delay(ProxyData *proxy_data) {
    srand(time(NULL));

    proxy_data->drop_data_decision = (rand() % 100 < proxy_data->drop_data_chance * 100);
    proxy_data->drop_ack_decision = (rand() % 100 < proxy_data->drop_ack_chance * 100);
    proxy_data->delay_data_decision = (rand() % 100 < proxy_data->delay_data_chance * 100);
    proxy_data->delay_ack_decision = (rand() % 100 < proxy_data->delay_ack_chance * 100);

    if (proxy_data->drop_data_decision) {
        pthread_mutex_lock(&proxy_data->stats->stats_mutex);
        proxy_data->stats->data_dropped++;
        pthread_mutex_unlock(&proxy_data->stats->stats_mutex);
    }
    if (proxy_data->delay_data_decision) {
        pthread_mutex_lock(&proxy_data->stats->stats_mutex);
        proxy_data->stats->data_delayed++;
        pthread_mutex_unlock(&proxy_data->stats->stats_mutex);
    }
    if (proxy_data->drop_ack_decision) {
        pthread_mutex_lock(&proxy_data->stats->stats_mutex);
        proxy_data->stats->ack_dropped++;
        pthread_mutex_unlock(&proxy_data->stats->stats_mutex);
    }
    if (proxy_data->delay_ack_decision) {
        pthread_mutex_lock(&proxy_data->stats->stats_mutex);
        proxy_data->stats->ack_delayed++;
        pthread_mutex_unlock(&proxy_data->stats->stats_mutex);
    }
}

// Forward data to the receiver
static void forward_data(ProxyData *proxy_data, const char *data, size_t data_len) {
    struct sockaddr_storage receiver_addr;
    convert_address(proxy_data->fsm->receiver_ip, &receiver_addr);

    if (receiver_addr.ss_family == AF_INET) {
        ((struct sockaddr_in *)&receiver_addr)->sin_port = htons(proxy_data->fsm->receiver_port);
    } else if (receiver_addr.ss_family == AF_INET6) {
        ((struct sockaddr_in6 *)&receiver_addr)->sin6_port = htons(proxy_data->fsm->receiver_port);
    }

    ssize_t sent_bytes = sendto(proxy_data->receiver_sockfd, data, data_len, 0, (struct sockaddr *)&receiver_addr, sizeof(receiver_addr));
    if (sent_bytes < 0) {
        perror("Failed to send data");
        fprintf(stderr, "Error sending data: %s\n", strerror(errno));
        receive_data(proxy_data);
    } else {
        pthread_mutex_lock(&proxy_data->stats->stats_mutex);
        proxy_data->stats->data_sent++;
        pthread_mutex_unlock(&proxy_data->stats->stats_mutex);
    }
}

static void forward_ack(ProxyData *proxy_data, const char *ack_data, size_t ack_data_len) {
    ssize_t sent_bytes = sendto(proxy_data->writer_sockfd, ack_data, ack_data_len, 0, (struct sockaddr *)&(proxy_data->writer_addr), sizeof(proxy_data->writer_addr));
    if (sent_bytes < 0) {
        perror("Failed to send acknowledgement");
        fprintf(stderr, "Error sending acknowledgement: %s\n", strerror(errno));
        receive_ack(proxy_data);
    } else {
        pthread_mutex_lock(&proxy_data->stats->stats_mutex);
        proxy_data->stats->ack_sent++;
        pthread_mutex_unlock(&proxy_data->stats->stats_mutex);
    }
}

// Function to store data statistics in a file
static void store_statistics(const char *filename, Statistics *stats) {
  FILE *file = fopen(filename, "w");
  if (file == NULL) {
    perror("Failed to open statistics file");
    return;
  }
  
    fprintf(file, "Data Sent: %lu\n", stats->data_sent);
    fprintf(file, "Data Received: %lu\n", stats->data_received);
    fprintf(file, "Data Dropped: %lu\n", stats->data_dropped);
    fprintf(file, "Data Delayed: %lu\n", stats->data_delayed);
    fprintf(file, "Ack Sent: %lu\n", stats->ack_sent);
    fprintf(file, "Ack Received: %lu\n", stats->ack_received);
    fprintf(file, "Ack Dropped: %lu\n", stats->ack_dropped);
    fprintf(file, "Ack Delayed: %lu\n", stats->ack_delayed);

  fclose(file);
}

static void send_statistics(ProxyData *proxy_data) {
    int gui_sockfd;
    struct sockaddr_storage gui_addr;

    // Convert GUI address
    convert_address(GUI_IP, &gui_addr);

    // Determine socket type (IPv4 or IPv6) and set port
    if (gui_addr.ss_family == AF_INET) {
        ((struct sockaddr_in*)&gui_addr)->sin_port = htons(GUI_PORT);
    } else if (gui_addr.ss_family == AF_INET6) {
        ((struct sockaddr_in6*)&gui_addr)->sin6_port = htons(GUI_PORT);
    } else {
        fprintf(stderr, "Unsupported address family\n");
        exit(EXIT_FAILURE);
    }

    // Create TCP socket
    gui_sockfd = socket(gui_addr.ss_family, SOCK_STREAM, 0);
    if (gui_sockfd < 0) {
        perror("Failed to create socket");
        exit(EXIT_FAILURE);
    }

    // Connect the socket to the GUI server
    if (connect(gui_sockfd, (struct sockaddr *)&gui_addr,
                gui_addr.ss_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6)) < 0) {
        perror("Connect failed");
        close(gui_sockfd);
        exit(EXIT_FAILURE);
    }

    // Convert statistics to a string format
    char stats_buffer[1024];
    snprintf(stats_buffer, sizeof(stats_buffer), "%lu,%lu", proxy_data->stats->packets_sent, proxy_data->stats->packets_received);

    // Send the statistics
    if (send(gui_sockfd, stats_buffer, strlen(stats_buffer), 0) < 0) {
        perror("Failed to send statistics");
        close(gui_sockfd);
        exit(EXIT_FAILURE);
    }

    // Close the socket
    close(gui_sockfd);
}


// Function to perform cleanup, particularly closing sockets
static void cleanup(ProxyData *proxy_data) {
    if (proxy_data->writer_sockfd != -1) {
        socket_close(proxy_data->writer_sockfd);
        proxy_data->writer_sockfd = -1;
    }
    if (proxy_data->receiver_sockfd != -1) {
        socket_close(proxy_data->receiver_sockfd);
        proxy_data->receiver_sockfd = -1;
    }
}

// Function to close a socket
static void socket_close(int sockfd) {
  if(close(sockfd) == -1)
  {
    perror("Error closing socket");
    exit(EXIT_FAILURE);
  }
}
