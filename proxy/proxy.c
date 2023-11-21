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

// Define constants 
#define UNKNOWN_OPTION_MESSAGE_LEN 24
#define BASE_TEN 10
#define BUFFER_SIZE 1024

// Enum for FSM states
typedef enum {
  INITIALIZE,
  SOCKET_CREATE,
  BINDING,
  THREAD_CREATE,
  THREAD_JOIN,
  IDLE,
  HANDLE_ACK,
  HANDLE_data,
  CLEAN_UP,
  HANDLE_ERROR
} ProxyState;

// FSM structure to hold state and essential data
typedef struct {
  ProxyState currentState; 
  int argc, writer_sockfd, receiver_sockfd;        
  char **argv, *receiver_ip, *receiver_port, *writer_port, *port_str; 
  in_port_t port;                    
  struct sockaddr_storage addr;       
  pthread_t t_idle, t_data, t_ack; 
  ProxyData proxy_data;           
} FSM;

// Structure to hold proxy-related data
typedef struct {
  int sockfd;                           
  struct sockaddr_storage receiver_addr, writer_addr; 
  float drop_data_chance;                    
  float delay_data_chance;
  float drop_ack_chance;
  float delay_ack_chance;    
  int writer_sockfd;
  int receiver_sockfd; 
  char *receiver_ip, *receiver_port, *writer_port;        
  Statistics *stats;              
} ProxyData;

// Structure for keeping track of statistics
typedef struct {
  unsigned long data_sent;        
  unsigned long data_received;       
  pthread_mutex_t stats_mutex;   
} Statistics;

// Define the Header structure
typedef struct {
    uint32_t seqNum;  
    uint32_t ackNum; 
    uint8_t flags;   
} Header;

// Define the CustomPacket structure
typedef struct {
    Header header;                 
    char data[BUFFER_SIZE];     
} Packet;

static void parse_arguments(int argc, char *argv[], char **receiver_ip, in_port_t *receiver_port, in_port_t *writer_port, ProxyData *proxy_data);
static void handle_arguments(const char *binary_name, const char *receiver_ip, in_port_t receiver_port,  in_port_t writer_port, ProxyData *proxy_data);
in_port_t parse_in_port_t(const char *binary_name, const char *str);
_Noreturn static void usage(const char *program_name, int exit_code, const char *message);
static void convert_address(const char *address, struct sockaddr_storage *addr);
static int socket_create(int domain, int type, int protocol);
static void socket_bind(int sockfd, struct sockaddr_storage *addr, in_port_t port);
static void cleanup(ProxyData *proxy_data);
static void socket_close(int sockfd);
static void transition(FSM *fsm);
static void store_statistics(const char *filename, Statistics *stats);
static void receive_data(int sockfd, ProxyData *proxy_data);
static void receive_ack(int sockfd, ProxyData *proxy_data);
static void forward_data(ProxyData *proxy_data, const char *data, size_t data_len);
static void forward_ack(ProxyData *proxy_data, const char *data, size_t data_len);
static void drop_or_delay(float drop_data_chance, float drop_ack_chance, float delay_data_chance, float delay_ack_chance, int *drop_data, int *drop_ack, int *delay_data, int *delay_ack);

int main(int argc, char *argv[]) {
  FSM fsm; // Instantiate FSM structure
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

  cleanup(&proxy_data); // Perform cleanup

  return fsm.currentState == CLEAN_UP ? EXIT_SUCCESS : EXIT_FAILURE;
}

// State transition function for FSM
static void transition(FSM *fsm) {
  switch(fsm->currentState) {
  case INITIALIZE:
    // Parse and handle arguments to set receiver IP and ports
    parse_arguments(fsm->argc, fsm->argv, &(fsm->receiver_ip), &(fsm->receiver_port), &(fsm->writer_port), &(fsm->proxy_data));
    handle_arguments(fsm->argv[0], fsm->receiver_ip, fsm->receiver_port, fsm->writer_port, &(fsm->proxy_data));
    fsm->currentState = SOCKET_CREATE;
    break;
    case SOCKET_CREATE:
      // Create sockets for writer and receiver
      fsm->writer_sockfd = socket_create(AF_INET, SOCK_DGRAM, 0);
      fsm->receiver_sockfd = socket_create(AF_INET, SOCK_DGRAM, 0);
      fsm->currentState = BINDING;
      break;
    case BINDING:
      // Convert receiver address and bind receiver socket
      convert_address(fsm->receiver_ip, &(fsm->addr));
      socket_bind(fsm->receiver_sockfd, &(fsm->addr), fsm->receiver_port);
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
  case HANDLE_ACK:
    ack_handler(&(fsm->proxy_data));
    break;
  case HANDLE_data:
    data_handler(&(fsm->proxy_data));
    break;
  case CLEAN_UP:
    cleanup(fsm->writer_sockfd);
    cleanup(fsm->receiver_sockfd);
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

// Function to store data statistics in a file
static void store_statistics(const char *filename, Statistics *stats) {
  FILE *file = fopen(filename, "w");
  if (file == NULL) {
    perror("Failed to open statistics file");
    return;
  }
  
  fprintf(file, "data Sent: %lu\n", stats->data_sent);
  fprintf(file, "data Received: %lu\n", stats->data_received);

  fclose(file);
}

// Idle handler function using non-blocking I/O
static void *idle_handler(void *arg) {
    ProxyData *proxy_data = (ProxyData *)arg;

    int writer_sockfd = proxy_data->writer_sockfd;
    int receiver_sockfd = proxy_data->receiver_sockfd;

    fcntl(writer_sockfd, F_SETFL, O_NONBLOCK);
    fcntl(receiver_sockfd, F_SETFL, O_NONBLOCK);

    fd_set readfds;
    struct timeval timeout;
    int max_sd;

    while (1) {
        FD_ZERO(&readfds);

        FD_SET(writer_sockfd, &readfds);
        FD_SET(receiver_sockfd, &readfds);
        max_sd = (writer_sockfd > receiver_sockfd) ? writer_sockfd : receiver_sockfd;

        timeout.tv_sec = 1; 
        timeout.tv_usec = 0;

        int activity = select(max_sd + 1, &readfds, NULL, NULL, &timeout);

        if ((activity < 0) && (errno != EINTR)) {
            perror("select error");
            break;
        }

        if (FD_ISSET(writer_sockfd, &readfds)) {
            receive_data(writer_sockfd, proxy_data);
        }

        if (FD_ISSET(receiver_sockfd, &readfds)) {
            receive_ack(receiver_sockfd, &(proxy_data->receiver_addr));
        }
    }

    close(writer_sockfd);
    close(receiver_sockfd);

    return NULL;
}


// Thread function for handling acknowledgment packets
static void *ack_handler(void *arg) {
    ProxyData *proxy_data = (ProxyData *)arg;
    int sockfd = proxy_data->receiver_sockfd; 

    while (1) {
        int drop_ack_decision, delay_ack_decision;
        
        drop_or_delay(proxy_data->drop_data_chance, proxy_data->drop_ack_chance,
                      proxy_data->delay_data_chance, proxy_data->delay_ack_chance,
                      NULL, &drop_ack_decision, NULL, &delay_ack_decision);

        if (drop_ack_decision) {
            continue;
        }

        if (delay_ack_decision) {
            sleep(5);
        }

        receive_ack(sockfd, proxy_data);
    }

    return NULL;
}


// Thread function for handling data
static void *data_handler(void *arg) {
    ProxyData *proxy_data = (ProxyData *)arg;
    int sockfd = proxy_data->writer_sockfd; 

    while (1) {
        int drop_data_decision, delay_data_decision;
        
        drop_or_delay(proxy_data->drop_data_chance, proxy_data->drop_ack_chance,
                      proxy_data->delay_data_chance, proxy_data->delay_ack_chance,
                      &drop_data_decision, NULL, &delay_data_decision, NULL);

        if (drop_data_decision) {
            continue;
        }

        if (delay_data_decision) {
            sleep(5);
        }

        receive_data(sockfd, proxy_data);
    }

    return NULL;
}


// Function to indefinitely listen for data sent from the writer
static void receive_data(int sockfd, ProxyData *proxy_data) {
    char buffer[BUFFER_SIZE];
    struct sockaddr_in writer_addr;
    socklen_t addr_len = sizeof(writer_addr);

    int drop_data_decision, delay_data_decision;

    while (1) {
        ssize_t numBytes = recvfrom(sockfd, buffer, BUFFER_SIZE, 0, (struct sockaddr *)&writer_addr, &addr_len);
        if (numBytes < 0) {
            perror("recvfrom failed");
            continue;
        }

        memcpy(&proxy_data->writer_addr, &writer_addr, sizeof(writer_addr));

        forward_data(proxy_data, buffer, numBytes);
    }
}


// Function to indefinitely listen for ack sent from the receiver
static void receive_ack(int sockfd, ProxyData *proxy_data) {
    char buffer[BUFFER_SIZE];
    struct sockaddr_storage writer_addr;
    socklen_t addr_len = sizeof(writer_addr);

    int drop_ack_decision, delay_ack_decision;

    while (1) {
        ssize_t numBytes = recvfrom(sockfd, buffer, BUFFER_SIZE, 0, (struct sockaddr *)&writer_addr, &addr_len);
        if (numBytes < 0) {
            perror("recvfrom failed");
            continue;
        }

        forward_ack(proxy_data, buffer, numBytes);
    }
}


// Function to determine whether to drop or delay data/ack based on given chances
static void drop_or_delay(float drop_data_chance, float drop_ack_chance, float delay_data_chance, float delay_ack_chance, int *drop_data, int *drop_ack, int *delay_data, int *delay_ack) {
    srand(time(NULL));

    *drop_data = (rand() % 100 < drop_data_chance * 100);
    *drop_ack = (rand() % 100 < drop_ack_chance * 100);
    *delay_data = (rand() % 100 < delay_data_chance * 100);
    *delay_ack = (rand() % 100 < delay_ack_chance * 100);
}

// Forward data to the receiver
static void forward_data(ProxyData *proxy_data, const char *data, size_t data_len) {
    struct sockaddr_storage receiver_addr;
    convert_address(proxy_data->receiver_ip, &receiver_addr);

    if (receiver_addr.ss_family == AF_INET) {
        ((struct sockaddr_in *)&receiver_addr)->sin_port = htons(proxy_data->receiver_port);
    } else if (receiver_addr.ss_family == AF_INET6) {
        ((struct sockaddr_in6 *)&receiver_addr)->sin6_port = htons(proxy_data->receiver_port);
    }

    ssize_t sent_bytes = sendto(proxy_data->sockfd, data, data_len, 0, 
                                (struct sockaddr *)&receiver_addr, 
                                sizeof(receiver_addr));
    if (sent_bytes < 0) {
        perror("Failed to send data");
        // Handle the error appropriately
    }
}

static void forward_ack(ProxyData *proxy_data, const char *ack_data, size_t ack_data_len) {
    ssize_t sent_bytes = sendto(proxy_data->sockfd, ack_data, ack_data_len, 0, 
                                (struct sockaddr *)&(proxy_data->writer_addr), 
                                sizeof(proxy_data->writer_addr));
    if (sent_bytes < 0) {
        perror("Failed to send acknowledgment");
        // Handle the error appropriately
    }
}


// Function to parse and validate command-line arguments
static void parse_arguments(int argc, char *argv[], char **receiver_ip, in_port_t *receiver_port, in_port_t *writer_port, ProxyData *proxy_data) {
    int opt;
    opterr = 0;

    while((opt = getopt(argc, argv, "h")) != -1) {
        switch(opt) {
        case 'h':
            usage(argv[0], EXIT_SUCCESS, NULL);
            break;
        case '?':
            char message[UNKNOWN_OPTION_MESSAGE_LEN];
            snprintf(message, sizeof(message), "Unknown option '-%c'.", optopt);
            usage(argv[0], EXIT_FAILURE, message);
            break;
        default:
            usage(argv[0], EXIT_FAILURE, NULL);
            break;
        }
    }

    // Check for correct number of arguments
    if (argc < 8) {
        usage(argv[0], EXIT_FAILURE, "Incorrect number of arguments.");
    }

    *receiver_ip = argv[optind];
    *receiver_port = parse_in_port_t(argv[0], argv[optind + 1]);
    *writer_port = parse_in_port_t(argv[0], argv[optind + 2]);
    proxy_data->drop_data_chance = atof(argv[optind + 3]);
    proxy_data->drop_ack_chance = atof(argv[optind + 4]);
    proxy_data->delay_data_chance = atof(argv[optind + 5]);
    proxy_data->delay_ack_chance = atof(argv[optind + 6]);
}

// Function to process and validate the command-line arguments
static void handle_arguments(const char *binary_name, const char *receiver_ip, in_port_t receiver_port,  in_port_t writer_port, ProxyData *proxy_data) {
    // Check if receiver IP address is provided
    if (receiver_ip == NULL) {
        usage(binary_name, EXIT_FAILURE, "The receiver IP address is required.");
    }

    // Validate receiver port
    if (receiver_port > UINT16_MAX || receiver_port == 0) {
        usage(binary_name, EXIT_FAILURE, "Invalid receiver port.");
    }
    proxy_data->receiver_port = receiver_port;

    // Validate writer port
    if (writer_port > UINT16_MAX || writer_port == 0) {
        usage(binary_name, EXIT_FAILURE, "Invalid writer port.");
    }
    proxy_data->writer_port = writer_port;
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

// Function to perform cleanup, particularly closing sockets
static void cleanup(ProxyData *proxy_data) {
  socket_close(proxy_data);
}

// Function to close a socket
static void socket_close(int sockfd) {
  if(close(sockfd) == -1)
  {
    perror("Error closing socket");
    exit(EXIT_FAILURE);
  }
}