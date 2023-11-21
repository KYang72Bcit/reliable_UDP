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
  PROCESS_PACKET_OR_ACK,
  FORWARD_PACKET_OR_ACK,
  CLEAN_UP,
  HANDLE_ERROR
} ProxyState;

// FSM structure to hold state and essential data
typedef struct {
  ProxyState currentState; 
  int argc, sockfd;        
  char **argv, *receiver_ip, *port_str; 
  in_port_t port;                    
  struct sockaddr_storage addr;       
  pthread_t t_incoming, t_resend, t_ack; 
  ProxyData proxy_data;           
} FSM;

// Structure to hold proxy-related data
typedef struct {
  int sockfd;                           
  struct sockaddr_storage receiver_addr; 
  float drop_packet_chance;                    
  float delay_packet_chance;
  float drop_ack_chance;
  float delay_ack_chance;                  
  Statistics *stats;              
} ProxyData;

// Structure for keeping track of statistics
typedef struct {
  unsigned long packets_sent;           // Counter for sent packets
  unsigned long packets_received;       // Counter for received packets
  pthread_mutex_t stats_mutex;          // Mutex for synchronizing access to statistics
} Statistics;

// Function prototypes
static void parse_arguments(int argc, char *argv[], char **receiver_ip, char **port, ProxyData *proxy_data);
static void handle_arguments(const char *binary_name, const char *receiver_ip, const char *port_str, in_port_t *port);
in_port_t parse_in_port_t(const char *binary_name, const char *str);
_Noreturn static void usage(const char *program_name, int exit_code, const char *message);
static void convert_address(const char *receiver_address, struct sockaddr_storage *addr);
static int socket_create(int domain, int type, int protocol);
static void socket_bind(int sockfd, struct sockaddr_storage *addr, in_port_t port);
static void cleanup(ProxyData *proxy_data);
static void socket_close(int sockfd);
static void transition(FSM *fsm);
static void *incoming_handler(void *arg);
static void *resend_handler(void *arg);
static void *ack_handler(void *arg);
static void store_statistics(const char *filename, Statistics *stats);
static int wait_packets_and_acks(ProxyData *proxy_data);

// Main function
int main(int argc, char *argv[]) {
  FSM fsm; // Instantiate FSM structure
  fsm.currentState = INITIALIZE; 
  fsm.argc = argc;              
  fsm.argv = argv;              
  Statistics stats = {0, 0, PTHREAD_MUTEX_INITIALIZER}; // Initialize statistics structure
  ProxyData proxy_data = {0};    // Initialize proxy data structure
  proxy_data.stats = &stats;     // Link proxy data to statistics

  // Check for minimum required arguments
  if (argc < 8) {
    usage(argv[0], EXIT_FAILURE, "Usage: <Receiver IP> <Receiver to send to> <Writer Port> <Drop Data %> <Drop Ack %> <Delay Data %> <Delay Ack %>");
    exit(EXIT_FAILURE);
  }

  cleanup(&proxy_data); // Perform cleanup

  // Check FSM state for successful cleanup
  return fsm.currentState == CLEAN_UP ? EXIT_SUCCESS : EXIT_FAILURE;
}

// State transition function for FSM
static void transition(FSM *fsm) {
  switch(fsm->currentState) {
  case INITIALIZE:
    parse_arguments(fsm->argc, fsm->argv, &fsm->receiver_ip, &fsm->port, &fsm->proxy_data);
    handle_arguments(fsm->argv[0], fsm->receiver_ip, fsm->port_str, &fsm->port);
    fsm->currentState = SOCKET_CREATE;
    break;
  case SOCKET_CREATE:
    fsm->sockfd = socket_create(fsm->addr.ss_family, SOCK_DGRAM, 0);
    fsm->currentState = BINDING;
    break;
  case BINDING:
    socket_bind(fsm->sockfd, &fsm->addr, fsm->port);
    fsm->currentState = THREAD_CREATE;
    break;
  case THREAD_CREATE:
    // Thread creation for incoming, resend, and ack handlers
    if (pthread_create(&fsm->t_incoming, NULL, incoming_handler, &fsm->proxy_data) != 0) {
        perror("Failed to create incoming handler thread");
        exit(EXIT_FAILURE);
    }
    if (pthread_create(&fsm->t_resend, NULL, resend_handler, &fsm->proxy_data) != 0) {
        perror("Failed to create resend handler thread");
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
    pthread_join(fsm->t_incoming, NULL);
    pthread_join(fsm->t_resend, NULL);
    pthread_join(fsm->t_ack, NULL);
    fsm->currentState = IDLE;
    break;
  case IDLE:
    if (wait_packets_and_acks(&fsm->proxy_data)) {
        
    }
    break;
  case PROCESS_PACKET_OR_ACK:
    // Placeholder for processing packet
    break;
  case FORWARD_PACKET_OR_ACK:
    // Placeholder for forwarding packet
    break;
  case CLEAN_UP:
    cleanup(fsm->sockfd); // Clean up resources
    break;
  case HANDLE_ERROR:
    fsm->currentState = CLEAN_UP; // Transition to cleanup on error
    break;
  default:
    fprintf(stderr, "Unexpected FSM state\n");
    fsm->currentState = HANDLE_ERROR; // Handle unexpected state
    break;
  }
}

// Thread function for handling incoming packets
static void *incoming_handler (void *arg) {
  // Receive, drop, delay, or forward packets based on conditions
}

// Thread function for handling packet resending
static void *resend_handler (void *arg) {
  // Add packets to resend list, resend them after a delay, and remove them after receiving ACK
}

// Thread function for handling acknowledgments
static void *ack_handler (void *arg) {
  // Receive and send ACKs for packets
}

// Function to store packet statistics in a file
static void store_statistics(const char *filename, Statistics *stats) {
  // Write stats such as packets sent and received to a file
  FILE *file = fopen(filename, "w");
  if (file == NULL) {
    perror("Failed to open statistics file");
    return;
  }
  
  fprintf(file, "Packets Sent: %lu\n", stats->packets_sent);
  fprintf(file, "Packets Received: %lu\n", stats->packets_received);

  fclose(file);
}

static int wait_packets_and_acks(ProxyData *proxy_data) {
    struct pollfd fds[2];
    // Store return value
    int ret;
    // File descriptor for packets
    fds[0].fd = proxy_data->sockfd;
    fds[0].events = POLLIN;
    // Wait for packets and acks
    ret = poll(fds, 1, -1);
    if (ret == - 1) {
        perror("poll");
        exit(EXIT_FAILURE);
    }
    // Check if data is available
    return (fds[0].revents & POLLIN) ? 1 : 0;
}

// Function to parse command-line arguments
static void parse_arguments(int argc, char *argv[], char **receiver_ip, char **port, ProxyData *proxy_data) {
  int opt;
  opterr = 0;  // Suppress getopt's own error messages

  // Loop to process each command-line option
  while((opt = getopt(argc, argv, "h")) != -1) {
    switch(opt) {
    case 'h':  // Help option
      usage(argv[0], EXIT_SUCCESS, NULL);
      break;
    case '?':  // Unknown option
      char message[UNKNOWN_OPTION_MESSAGE_LEN];
      snprintf(message, sizeof(message), "Unknown option '-%c'.", optopt);
      usage(argv[0], EXIT_FAILURE, message);
      break;
    default:   // Unhandled option
      usage(argv[0], EXIT_FAILURE, NULL);
      break;
    }
  }
  // Check if required arguments are provided
  if (optind >= argc) {
    usage(argv[0], EXIT_FAILURE, "The proxy IP address and ports are required.");
  }
  else if (optind + 1 >= argc) {
    usage(argv[0], EXIT_FAILURE, "The port is required.");
  }
  else if (argc <= optind + 3) {
    usage(argv[0], EXIT_FAILURE, "Insufficient arguments: Need drop and delay percentages.");
  }
  proxy_data->drop_packet_chance = atof(argv[optind + 2]);
  proxy_data->drop_ack_chance = atof(argv[optind + 3]);
  proxy_data->delay_packet_chance = atof(argv[optind + 4]);
  proxy_data->delay_ack_chance = atof(argv[optind + 5]);
  *receiver_ip = argv[optind];
  *port = argv[optind + 1];
}

// Function to process and validate command-line arguments
static void handle_arguments(const char *binary_name, const char *receiver_ip, const char *port_str, in_port_t *port) {
  if (receiver_ip == NULL) {
    usage(binary_name, EXIT_FAILURE, "The receiver IP address is required.");
  }
  if (port_str == NULL) {
    usage(binary_name, EXIT_FAILURE, "The port is required.");
  }
  *port = parse_in_port_t(binary_name, port_str);
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
_Noreturn static void usage(const char *program_name, int exit_code, const char *message)
{
  if(message)
  {
    fprintf(stderr, "%s\n", message);
  }
  fprintf(stderr, "Usage: %s [-h] <Receiver IP> <Receiver to send to> <Writer Port> <Drop Data %> <Drop Ack %> <Delay Data %> <Delay Ack %>\n", program_name);
  fputs("Options:\n", stderr);
  fputs("  -h  Display this help message\n", stderr);
  exit(exit_code);
}

// Function to convert a string IP address to a sockaddr structure
static void convert_address(const char *receiver_address, struct sockaddr_storage *addr)
{
  memset(addr, 0, sizeof(*addr)); // Clear the structure
  char tmp_receiver_address[INET6_ADDRSTRLEN];
  strncpy(tmp_receiver_address, receiver_address, sizeof(tmp_receiver_address));
  tmp_receiver_address[sizeof(tmp_receiver_address) - 1] = '\0';  
  struct sockaddr_in6 *addr_v6 = (struct sockaddr_in6 *)addr;
  char *percent_sign = strchr(tmp_receiver_address, '%'); // Find '%' for IPv6 scope ID
  if (percent_sign != NULL) {
    *percent_sign = '\0';
    char *interface_name = percent_sign + 1;
    addr_v6->sin6_scope_id = if_nametoindex(interface_name); 
    if (addr_v6->sin6_scope_id == 0) {
        perror("Invalid interface for IPv6 scope ID");
        exit(EXIT_FAILURE);
    }
  }
  // Convert IP address from string to binary form
  if(inet_pton(AF_INET, tmp_receiver_address, &(((struct sockaddr_in *)addr)->sin_addr)) == 1) {
    addr->ss_family = AF_INET; // IPv4
  } else if(inet_pton(AF_INET6, tmp_receiver_address, &addr_v6->sin6_addr) == 1) {
    addr->ss_family = AF_INET6; // IPv6
  } else {
    fprintf(stderr, "%s is not an IPv4 or an IPv6 address\n", receiver_address);
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
static void socket_bind(int sockfd, struct sockaddr_storage *addr, in_port_t port)
{
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
  socket_close(proxy_data->sockfd); // Close the socket

  // Destroy mutex inside stats
  pthread_mutex_destroy(&proxy_data->stats->stats_mutex);
}

// Function to close a socket
static void socket_close(int sockfd)
{
  if(close(sockfd) == -1)
  {
    perror("Error closing socket");
    exit(EXIT_FAILURE);
  }
}