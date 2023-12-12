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
#include <net/if.h> 
#include <signal.h> 

#define UNKNOWN_OPTION_MESSAGE_LEN 24 
#define BASE_TEN 10 
#define BUFFER_SIZE 1024 

static volatile sig_atomic_t shutdown_flag = 0; 

typedef enum {
    INITIALIZE, 
    PARSE_ARUGMENTS,
    HANDLE_ARGUMENTS,
    CONVERT_ADDRESS,
    SOCKET_CREATE,
    SOCKET_BIND,
    WAITING_FOR_DATA, 
    FORWARDING_DATA,
    WAITING_FOR_ACK, 
    FORWARDING_ACK,
    HANDLE_ERROR,
    TERMINATE 
} ProxyState; 

typedef struct {
    bool error_flag;
    ssize_t numBytes; 
    time_t start_time; 
    pthread_t stats_thread;
    ProxyState currentState; 
    char *receiver_ip, *proxy_ip, *gui_ip, **argv; 
    in_port_t proxy_port, receiver_port, gui_port; 
    socklen_t proxy_addr_len, receiver_addr_len, gui_addr_len;
    pthread_mutex_t stats_mutex, error_flag_mutex;  
    struct sockaddr_storage proxy_addr, receiver_addr, gui_addr; 
    char data_buffer[BUFFER_SIZE], ack_buffer[BUFFER_SIZE], gui_buffer[BUFFER_SIZE]; 
    float drop_data_chance, drop_ack_chance, delay_data_chance, delay_ack_chance;
    int argc, sockfd, writer_sockfd, receiver_sockfd, gui_sockfd, data_send, data_dropped, data_delayed, data_received; 
    int ack_send, ack_dropped, ack_delayed, ack_received, current_packet_id; 
} FSM;

static void signal_handler(int signum);
static int setup_signal_handler(); 
ProxyState transition(FSM *fsm, struct pollfd fds[]); 
static bool is_valid_ip(const char *ip);
static bool is_numeric(const char *str);
static bool validate_percentage(const char *str);
static bool parse_arguments(FSM *fsm);
static bool handle_arguments(FSM *fsm);
in_port_t parse_in_port_t(const char *str);
static bool convert_address(FSM *fsm, struct sockaddr_storage *addr_storage, const char *ip, in_port_t port, socklen_t *addr_len);
static int socket_create(const char *ip, bool use_tcp); 
static int socket_bind(int sockfd, const char *ip, in_port_t port);
static void polling_loop(FSM *fsm, struct pollfd fds[], pthread_t stats_thread);
static bool receive_data(FSM *fsm); 
static bool drop_or_delay_data(FSM *fsm); 
static bool forward_data(FSM *fsm); 
static bool receive_ack(FSM *fsm); 
static bool drop_or_delay_ack(FSM *fsm);
static bool forward_ack(FSM *fsm);
const char* format_elapsed_time(double elapsed_seconds);
static void* write_statistics_periodically(void *arg);
static bool store_statistics(const char *filename, FSM *fsm); 
static void cleanup(FSM *fsm); 
static int socket_close(int sockfd); 
static int connect_to_gui(FSM *fsm);
static bool send_statistics_to_gui(FSM *fsm, const char *stats_message);

int main(int argc, char *argv[]) {
    FSM fsmInstance;
    memset(&fsmInstance, 0, sizeof(FSM));
    srand(time(NULL));
    struct pollfd fds[2];
    fsmInstance.argc = argc;
    fsmInstance.argv = argv;
    fsmInstance.start_time = time(NULL);
    fsmInstance.currentState = INITIALIZE;
    pthread_mutex_init(&fsmInstance.stats_mutex, NULL);
    fsmInstance.proxy_addr_len = sizeof(fsmInstance.proxy_addr);
    fsmInstance.receiver_addr_len = sizeof(fsmInstance.receiver_addr);

    setup_signal_handler();
    if (pthread_create(&fsmInstance.stats_thread, NULL, write_statistics_periodically, (void *)&fsmInstance) != 0) {
        perror("Failed to create the statistics thread");
        return HANDLE_ERROR;
    }

    pthread_mutex_init(&fsmInstance.error_flag_mutex, NULL);
    fsmInstance.error_flag = false;

    while (fsmInstance.currentState != TERMINATE) {
        fsmInstance.currentState = transition(&fsmInstance, NULL);
        if (fsmInstance.currentState == WAITING_FOR_DATA) {
            polling_loop(&fsmInstance, fds, fsmInstance.stats_thread);
        }
    }

    if (fsmInstance.currentState == TERMINATE) {
        printf("Exiting program...\n");
    }

    store_statistics("Proxy Statistics.csv", &fsmInstance);

    cleanup(&fsmInstance);
    pthread_cancel(fsmInstance.stats_thread);
    pthread_join(fsmInstance.stats_thread, NULL);

    return EXIT_SUCCESS;

}

static void signal_handler(int signum) {
    if (signum == SIGINT) {
        shutdown_flag = 1;
    }
}

static int setup_signal_handler() {
    if (signal(SIGINT, signal_handler) == SIG_ERR) {
        perror("signal");
        return -1; 
    }
    return 0; 
}

ProxyState transition(FSM *fsm, struct pollfd fds[]) {
    switch(fsm->currentState) {
        case INITIALIZE:
            printf("Initializing...\n");
            if (setup_signal_handler() != 0) {
                printf("Failed to set up signal handler.\n");
                return HANDLE_ERROR;
            }
            printf("Initialized.\n");
            return PARSE_ARUGMENTS;

        case PARSE_ARUGMENTS:
            printf("Parsing arugments...\n");
            if (!parse_arguments(fsm)) {
                printf("Failed to parse arugments.\n");
                return HANDLE_ERROR;
            }
            printf("Arguments parsed.\n");
            return HANDLE_ARGUMENTS;

        case HANDLE_ARGUMENTS:
            printf("Validating arugments...\n");
            if (!handle_arguments(fsm)) {
                printf("Arugments are invalid.\n");
                return HANDLE_ERROR;
            }
            printf("Arguments validated.\n");
            return CONVERT_ADDRESS;

        case CONVERT_ADDRESS:
            printf("Converting addresses...\n");
            if (!convert_address(fsm, &fsm->receiver_addr, fsm->receiver_ip, fsm->receiver_port, &fsm->receiver_addr_len) ||
                !convert_address(fsm, &fsm->gui_addr, fsm->gui_ip, fsm->gui_port, &fsm->gui_addr_len)) {
                printf("Failed to convert address.\n");
                return HANDLE_ERROR;
            }
            printf("Addresses converted.\n");
            /*
            printf("Receiver IP: %s\n", fsm->receiver_ip);
            printf("Receiver Port: %hu\n", fsm->receiver_port);
            printf("Proxy IP: %s\n", fsm->proxy_ip);
            printf("Proxy Port: %hu\n", fsm->proxy_port);
            printf("GUI IP address: %s\n", fsm->gui_ip);
            printf("GUI port: %hu\n", fsm->gui_port);
            printf("Drop Data Chance: %.2f%%\n", fsm->drop_data_chance);
            printf("Drop Ack Chance: %.2f%%\n", fsm->drop_ack_chance);
            printf("Delay Data Chance: %.2f%%\n", fsm->delay_data_chance);
            printf("Delay Ack Chance: %.2f%%\n\n", fsm->delay_ack_chance);
            */
            return SOCKET_CREATE;

        case SOCKET_CREATE:
            printf("Creating Sockets...\n");
            fsm->writer_sockfd = socket_create(fsm->proxy_ip, false);
            fsm->receiver_sockfd = socket_create(fsm->receiver_ip, false);
            fsm->gui_sockfd = socket_create(fsm->gui_ip, true); // true for TCP
            if (fsm->gui_sockfd == -1) {
                printf("Failed to create or connect GUI socket.\n");
                return HANDLE_ERROR;
            } else if (connect_to_gui(fsm) == -1) {
                printf("Failed to connect GUI socket.\n");
                return HANDLE_ERROR;
            }
            printf("Sockets created.\n");
            return SOCKET_BIND;

        case SOCKET_BIND:
            printf("Binding sockets...\n");
            if (socket_bind(fsm->writer_sockfd, fsm->proxy_ip, fsm->proxy_port) == - 1) {
                printf("Failed to bind socket.\n");
                return HANDLE_ERROR;
            }
            printf("Sockets binded.\n\n");
            return WAITING_FOR_DATA;

        case FORWARDING_DATA:
            printf("Forwarding data to receiver...\n");
            if (!forward_data(fsm)) {
                printf("Failed to forward data.\n\n");
                return WAITING_FOR_DATA;
            } 
            return WAITING_FOR_ACK;
            
        case FORWARDING_ACK:
            printf("Forwarding ACK to writer...\n");
            if (!forward_ack(fsm)) {
                printf("Failed to forward ack.\n");
                return WAITING_FOR_DATA;
            }
            return WAITING_FOR_DATA;

        case HANDLE_ERROR:
            if (!parse_arguments(fsm) || !handle_arguments(fsm) || !convert_address(fsm, &fsm->receiver_addr, fsm->receiver_ip, fsm->receiver_port, &fsm->receiver_addr_len) ||
                !convert_address(fsm, &fsm->gui_addr, fsm->gui_ip, fsm->gui_port, &fsm->gui_addr_len)|| setup_signal_handler() == -1) {
                printf("Critical Error - Recovery not possible.\n");
                return TERMINATE;
            }

            if (fsm->writer_sockfd == -1 || fsm->receiver_sockfd == -1 || fsm->gui_sockfd == -1) {
                fprintf(stderr, "Attempting to recreate sockets...\n");
                fsm->writer_sockfd = socket_create(fsm->proxy_ip, false);
                fsm->receiver_sockfd = socket_create(fsm->receiver_ip, false);
                fsm->gui_sockfd = socket_create(fsm->gui_ip, true);
                if (fsm->writer_sockfd == -1 || fsm->receiver_sockfd == -1 || fsm->gui_sockfd == -1) {
                    fprintf(stderr, "Recovery failed.\n");
                    return TERMINATE;
                } else {
                    return SOCKET_BIND;
                }
            }

            if (socket_bind(fsm->writer_sockfd, fsm->proxy_ip, fsm->proxy_port) == -1) {
                fprintf(stderr, "Attempting to rebind writer socket...\n");
                if (socket_bind(fsm->writer_sockfd, fsm->proxy_ip, fsm->proxy_port) == -1) {
                    fprintf(stderr, "Recovery failed.\n");
                    return TERMINATE;
                } else {
                    return WAITING_FOR_DATA;
                }
            }

        default:
            return fsm->currentState;
    }
    return fsm->currentState;
}

static bool is_valid_ip(const char *ip) {
    struct sockaddr_in sa;
    struct sockaddr_in6 sa6;

    if (strchr(ip, ':') == NULL) { 
        return inet_pton(AF_INET, ip, &(sa.sin_addr)) == 1;
    } else { 
        return inet_pton(AF_INET6, ip, &(sa6.sin6_addr)) == 1;
    }
}

static bool is_numeric(const char *str) {
    if (str == NULL || *str == '\0') {
        return false;
    }
    char *endptr;
    strtod(str, &endptr);  
    return *endptr == '\0';
}

static bool validate_percentage(const char *str) {
    if (!is_numeric(str)) {
        return false;
    }
    float chance = atof(str);
    return chance >= 0.0f && chance <= 100.0f;
}

static bool parse_arguments(FSM *fsm) {
    if (fsm->argc != 11) {
        fprintf(stderr, "Incorrect number of arguments. Expected 11 but got %d.\n", fsm->argc);
        return false;
    } 

    fsm->receiver_ip = fsm->argv[1];
    fsm->receiver_port = parse_in_port_t(fsm->argv[2]);
    fsm->proxy_ip = fsm->argv[3];
    fsm->proxy_port = parse_in_port_t(fsm->argv[4]);
    fsm->gui_ip = fsm->argv[5];
    fsm->gui_port = parse_in_port_t(fsm->argv[6]);
    fsm->drop_data_chance = atof(fsm->argv[7]);
    fsm->drop_ack_chance = atof(fsm->argv[8]);
    fsm->delay_data_chance = atof(fsm->argv[9]);
    fsm->delay_ack_chance = atof(fsm->argv[10]);

    return true;
}

static bool handle_arguments(FSM *fsm) {
    if (!is_valid_ip(fsm->receiver_ip)) {
        fprintf(stderr, "Invalid receiver IP address.\n");
        return false;
    }
    if (fsm->receiver_port == 0 || fsm->receiver_port > UINT16_MAX) {
        return false;
    }
    if (!is_valid_ip(fsm->proxy_ip)) {
        fprintf(stderr, "Invalid proxy IP address.\n");
        return false;
    }
    if (fsm->proxy_port == 0 || fsm->proxy_port > UINT16_MAX) {
        return false;
    }
    if (!is_valid_ip(fsm->gui_ip)) {
        fprintf(stderr, "Invalid GUI IP address.\n");
        return false;
    }
    if (fsm->gui_port == 0 || fsm->gui_port > UINT16_MAX) {
        return false;
    }

    if (!validate_percentage(fsm->argv[7])) {
        fprintf(stderr, "Invalid drop data chance provided: %s. Must be a number between 0 and 100.\n", fsm->argv[7]);
        return false;
    }

    if (!validate_percentage(fsm->argv[8])) {
        fprintf(stderr, "Invalid drop ack chance provided: %s. Must be between 0 and 100.\n", fsm->argv[8]);
        return false;
    }
    if (!validate_percentage(fsm->argv[9])) {
        fprintf(stderr, "Invalid delay data chance provided: %s. Must be between 0 and 100.\n", fsm->argv[9]);
        return false;
    }
    if (!validate_percentage(fsm->argv[10])) {
        fprintf(stderr, "Invalid delay ack chance provided: %s. Must be between 0 and 100.\n", fsm->argv[10]);
        return false;
    }

    return true;
}

in_port_t parse_in_port_t(const char *str) {
    char *endptr;
    unsigned long port = strtoul(str, &endptr, BASE_TEN);

    if (endptr == str || *endptr != '\0' || errno == ERANGE || port == 0 || port > UINT16_MAX) {
        fprintf(stderr, "Invalid port number: '%s'\n", str);
        return 0; 
    }

    return (in_port_t)port;
}

static bool convert_address(FSM *fsm, struct sockaddr_storage *addr_storage, const char *ip, in_port_t port, socklen_t *addr_len) {
    memset(addr_storage, 0, sizeof(struct sockaddr_storage));
    char tmp_address[INET6_ADDRSTRLEN];
    strncpy(tmp_address, ip, sizeof(tmp_address) - 1);
    tmp_address[sizeof(tmp_address) - 1] = '\0';

    struct sockaddr_in6 *addr_v6 = (struct sockaddr_in6 *)addr_storage;
    struct sockaddr_in *addr_v4 = (struct sockaddr_in *)addr_storage;
    char *percent_sign = strchr(tmp_address, '%');

    if (percent_sign != NULL) {
        *percent_sign = '\0';
        char *interface_name = percent_sign + 1;
        addr_v6->sin6_scope_id = if_nametoindex(interface_name);
        if (addr_v6->sin6_scope_id == 0) {
            perror("Invalid interface for IPv6 scope ID");
            return false;
        }
    }

    if (inet_pton(AF_INET, tmp_address, &(addr_v4->sin_addr)) == 1) {
        addr_storage->ss_family = AF_INET;
        addr_v4->sin_port = htons(port);
        *addr_len = sizeof(struct sockaddr_in); // Set the address length for IPv4
    } else if (inet_pton(AF_INET6, tmp_address, &(addr_v6->sin6_addr)) == 1) {
        addr_storage->ss_family = AF_INET6;
        addr_v6->sin6_port = htons(port);
        *addr_len = sizeof(struct sockaddr_in6); // Set the address length for IPv6
    } else {
        fprintf(stderr, "%s is not a valid IPv4 or IPv6 address\n", ip);
        return false;
    }

    return true;
}

static int socket_create(const char *ip, bool use_tcp) {
    int sockfd;
    int yes = 1;

    int family = (strchr(ip, ':') != NULL) ? AF_INET6 : AF_INET;
    printf("Creating %s socket for IP: %s\n", use_tcp ? "TCP" : "UDP", ip);
    sockfd = socket(family, use_tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (sockfd == -1) {
        perror("socket creation failed");
        return -1;
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
        perror("setsockopt SO_REUSEADDR failed");
        close(sockfd);
        return -1;
    }

    if (family == AF_INET6) {
        if (setsockopt(sockfd, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes)) == -1) {
            perror("setsockopt IPV6_V6ONLY failed");
            close(sockfd);
            return -1;
        }
    }

    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        close(sockfd);
        return -1;
    }
    flags |= O_NONBLOCK;
    if (fcntl(sockfd, F_SETFL, flags) == -1) {
        perror("fcntl F_SETFL O_NONBLOCK");
        close(sockfd);
        return -1;
    }

    return sockfd;
}


static int socket_bind(int sockfd, const char *ip, in_port_t port) {
    struct sockaddr_in addr4;
    struct sockaddr_in6 addr6;
    void *addr;
    int addr_len;

    if (strchr(ip, ':') != NULL) {  
        memset(&addr6, 0, sizeof(addr6));
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = htons(port);
        if (inet_pton(AF_INET6, ip, &addr6.sin6_addr) != 1) {
            perror("inet_pton failed for IPv6");
            return -1;
        }
        addr = &addr6;
        addr_len = sizeof(addr6);
    } else {
        memset(&addr4, 0, sizeof(addr4));
        addr4.sin_family = AF_INET;
        addr4.sin_port = htons(port);
        if (inet_pton(AF_INET, ip, &addr4.sin_addr) != 1) {
            perror("inet_pton failed for IPv4");
            return -1;
        }
        addr = &addr4;
        addr_len = sizeof(addr4);
    }

    if (bind(sockfd, (struct sockaddr *)addr, addr_len) == -1) {
        perror("bind failed");
        return -1;
    }

    return 0; 
}

static void polling_loop(FSM *fsm, struct pollfd fds[], pthread_t stats_thread) {
    int poll_result; 
    int timeout = 3000; 
    fds[0].fd = fsm->writer_sockfd; 
    fds[0].events = POLLIN;

    fds[1].fd = fsm->receiver_sockfd;
    fds[1].events = POLLIN;
    
    while (fsm->currentState != TERMINATE && !shutdown_flag) {
        poll_result = poll(fds, 2, timeout); 

        pthread_mutex_lock(&fsm->error_flag_mutex);
        if (fsm->error_flag) {
            fsm->currentState = HANDLE_ERROR;
        }
        pthread_mutex_unlock(&fsm->error_flag_mutex);

        if (poll_result == -1) {
            if (errno == EINTR) {
                if (shutdown_flag) {
                    printf("Shutdown signal received...\n");
                    fsm->currentState = TERMINATE;
                }
                continue;
            } else {
                perror("poll error");
            }
            break;
        }

        if (poll_result == 0) {
            printf("Waiting to receive data...\n");
        } else {
            if (fds[0].revents & POLLIN) {
                if (!receive_data(fsm)) {
                    fsm->currentState = WAITING_FOR_DATA;
                } else {
                    fsm->currentState = FORWARDING_DATA;
                }
            }
            if (fds[1].revents & POLLIN) {
                if (!receive_ack(fsm)) {
                    fsm->currentState = WAITING_FOR_DATA;
                } else {
                    fsm->currentState = FORWARDING_ACK;
                }
            }
        }

        fsm->currentState = transition(fsm, fds); 
    }

    if (shutdown_flag) { 
        fsm->currentState = TERMINATE;
    }
}

static bool receive_data(FSM *fsm) {
    int received_packet_id;
    fsm->numBytes = recvfrom(fsm->writer_sockfd, fsm->data_buffer, BUFFER_SIZE - 1, 0, (struct sockaddr *)&fsm->proxy_addr, &fsm->proxy_addr_len);
    if (fsm->numBytes < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            return false;
        }
        perror("recvfrom failed");
        return false;
    }

    fsm->data_buffer[fsm->numBytes] = '\0';
    printf("Received %zd bytes: %s\n", fsm->numBytes, fsm->data_buffer);
    fsm->data_received++;

    if (!drop_or_delay_data(fsm)) {
        return true; 
    }
    return false;
}


static bool drop_or_delay_data(FSM *fsm) {
    float rand_percent = ((float)rand() / RAND_MAX) * 100;
    if (rand_percent < fsm->drop_data_chance) {
        printf("Data dropped.\n\n");
        fsm->data_dropped++;

        return true;
    } else if (rand_percent < fsm->drop_data_chance + fsm->delay_data_chance) {
        int delay_time_ms = (int)((rand_percent - fsm->drop_data_chance) / fsm->delay_data_chance * 1000);
        printf("Delaying data for %d ms.\n\n", delay_time_ms);
        fsm->data_delayed++;
        fflush(stdout);
        usleep(delay_time_ms * 1000); 
        return false;
    }
    return false;
}

static bool forward_data(FSM *fsm) {
    ssize_t numBytes = sendto(fsm->receiver_sockfd, fsm->data_buffer, fsm->numBytes, 0, (struct sockaddr *)&fsm->receiver_addr, fsm->receiver_addr_len);

    if (numBytes < 0) {
        return false;
    }

    fsm->data_send++;
    printf("%zd bytes data sent to receiver.\n\n", numBytes);
    return true;
}

static bool receive_ack(FSM *fsm) {
    int acked_packet_id;
    printf("Waiting to receive ACK...\n");
    fsm->numBytes = recvfrom(fsm->receiver_sockfd, fsm->ack_buffer, BUFFER_SIZE - 1, 0, (struct sockaddr *)&fsm->receiver_addr, &fsm->receiver_addr_len);
    if (fsm->numBytes == -1) {
        perror("recvfrom failed for ACK");
        return false;
    }

    fsm->ack_buffer[fsm->numBytes] = '\0';
    printf("Received ACK: %s\n\n", fsm->ack_buffer);
    fsm->ack_received++;

    if (!drop_or_delay_ack(fsm)) {
        return true;
    } else {
        fsm->currentState = WAITING_FOR_DATA;
        return false;
    }
}

static bool drop_or_delay_ack(FSM *fsm) {
    float rand_percent = ((float)rand() / RAND_MAX) * 100;
    if (rand_percent < fsm->drop_ack_chance) {
        printf("ACK dropped.\n\n");
        fsm->ack_dropped++;
        return true;
    } else if (rand_percent < fsm->drop_ack_chance + fsm->delay_ack_chance) {
        int delay_time_ms = (int)((rand_percent - fsm->drop_ack_chance) / fsm->delay_ack_chance * 1000);
        printf("Delaying ACK for %d ms.\n\n", delay_time_ms);
        fsm->ack_delayed++;
        usleep(delay_time_ms * 1000); 
    }
    return false;
}

static bool forward_ack(FSM *fsm) {
    ssize_t numBytes = sendto(fsm->writer_sockfd, fsm->ack_buffer, fsm->numBytes, 0, (struct sockaddr *)&fsm->proxy_addr, fsm->proxy_addr_len);
    if (numBytes < 0) {
        perror("sendto failed for ACK");
        printf("Error code: %d\n", errno);
        return false;
    }
    fsm->ack_send++;
    printf("ACK Send to writer.\n\n");
    return true;
}

static int connect_to_gui(FSM *fsm) {
    printf("Attempting to connect to GUI...\n");
    //printf("GUI IP: %s\n", fsm->gui_ip);
    //printf("GUI Port: %hu\n", fsm->gui_port);
    
    // Attempt to connect to the GUI
    if (connect(fsm->gui_sockfd, (struct sockaddr *)&fsm->gui_addr, fsm->gui_addr_len) < 0) {
        if (errno == EINPROGRESS) {
            //printf("Connection in progress... \n");
            return 0; 
        } else {
            perror("connect to GUI failed");
            printf("Error code: %d\n", errno);
            return -1; 
        }
    }

    printf("Connected to GUI successfully.\n");
    return 0; 
}

static bool send_statistics_to_gui(FSM *fsm, const char *stats_message) {
    
    ssize_t numBytes = send(fsm->gui_sockfd, stats_message, strlen(stats_message), 0);
    
    if (numBytes < 0) {
        perror("Failed to send statistics to GUI");
        printf("Error sending to GUI, errno: %d\n", errno);
        return false;
    }
    
    //printf("Sent %zd bytes to GUI. Message: %s\n", numBytes, stats_message);
    return true;
}


const char* format_elapsed_time(double elapsed_seconds) {
    static char buffer[32];
    int minutes = ((int)elapsed_seconds % 3600) / 60;
    int seconds = (int)elapsed_seconds % 60;
    snprintf(buffer, 32, "%02d:%02d", minutes, seconds);
    return buffer;
}

static void* write_statistics_periodically(void *arg) {
    FSM *fsm = (FSM *)arg;
    while (!shutdown_flag) {
        sleep(5);

        pthread_mutex_lock(&fsm->stats_mutex);
        if (shutdown_flag) {
            pthread_mutex_unlock(&fsm->stats_mutex);
            break;
        }
        bool success = store_statistics("Proxy Statistics.csv", fsm);
        pthread_mutex_unlock(&fsm->stats_mutex);

        if (!success) {
            pthread_mutex_lock(&fsm->error_flag_mutex);
            fsm->error_flag = true;
            pthread_mutex_unlock(&fsm->error_flag_mutex);
            break; 
        }
    }
    return NULL; 
}

static bool store_statistics(const char *filename, FSM *fsm) {
    FILE *file = fopen(filename, "a");
    if (!file) {
        perror("Failed to open statistics file");
        return false;
    }

    fseek(file, 0, SEEK_END);
    size_t size = ftell(file);
    if (size == 0) {
        fprintf(file, "Time Elapsed,Data Send, Data Received,ACK Send,ACK Received, Data Delayed, Data Dropped, ACK Delayed, ACK Dropped\n");
    }
    
    time_t now = time(NULL);
    double elapsed = difftime(now, fsm->start_time);
    const char* elapsed_time_str = format_elapsed_time(elapsed);
    
    fprintf(file, "%s,%d,%d,%d,%d,%d,%d,%d,%d\n",
        elapsed_time_str,
        fsm->data_send,
        fsm->data_received,
        fsm->ack_send,
        fsm->ack_received,
        fsm->data_delayed,
        fsm->data_dropped,
        fsm->ack_delayed,
        fsm->ack_dropped
    );
    
    if (fclose(file) == EOF) {
        perror("fclose failed");
        return false; 
    }

    char stats_message[256];
    snprintf(stats_message, sizeof(stats_message),
        "Time Elapsed: %s, Data Delayed: %d, Data Dropped: %d, ACK Delayed: %d, ACK Dropped: %d\n",
        elapsed_time_str,
        fsm->data_delayed,
        fsm->data_dropped,
        fsm->ack_delayed,
        fsm->ack_dropped);

    return send_statistics_to_gui(fsm, stats_message);
}

static void cleanup(FSM *fsm) {
    printf("Cleaning up and updating statistics...\n");

    const char* terminate_message = "TERMINATE";
    ssize_t send_result = send(fsm->gui_sockfd, terminate_message, strlen(terminate_message), 0);
    if (send_result < 0) {
        perror("Failed to send TERMINATE message to GUI");
    } else {
        //printf("Sent TERMINATE message to GUI\n");
    }

    if (fsm->writer_sockfd != -1 && fcntl(fsm->writer_sockfd, F_GETFD) != -1) {
        printf("Closing writer socket...\n");
        socket_close(fsm->writer_sockfd);
    }
    if (fsm->receiver_sockfd != -1 && fcntl(fsm->receiver_sockfd, F_GETFD) != -1) {
        printf("Closing receiver socket...\n");
        socket_close(fsm->receiver_sockfd);
    }
    if (fsm->gui_sockfd != -1 && fcntl(fsm->gui_sockfd, F_GETFD) != -1) {
        printf("Closing GUI socket...\n");
        socket_close(fsm->gui_sockfd);
    }
    if (&fsm->stats_mutex) {
        pthread_mutex_destroy(&fsm->stats_mutex);
    }
    if (&fsm->error_flag_mutex) {
        pthread_mutex_destroy(&fsm->error_flag_mutex);
    }
    printf("Cleaned up.\n");
}


static int socket_close(int sockfd) {
    if (sockfd != -1) {
        if (close(sockfd) == -1) {
            perror("Error closing socket");
            return -1;  
        }
    }
    return 0;  
} 