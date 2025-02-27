/*
# Copyright 2025 University of Kentucky
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0
*/

/*
Please specify the group members here

# Student #1: Charles Cochran
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>

#define MAX_EVENTS 64
#define MESSAGE_SIZE 16
#define DEFAULT_CLIENT_THREADS 4

char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 1000000;

/*
 * This structure is used to store per-thread data in the client
 */
typedef struct {
    int id; // thread id
    int epoll_fd;        /* File descriptor for the epoll instance, used for monitoring events on the socket. */
    int socket_fd;       /* File descriptor for the client socket connected to the server. */
    long long total_rtt; /* Accumulated Round-Trip Time (RTT) for all messages sent and received (in microseconds). */
    long total_messages; /* Total number of messages sent and received. */
    float request_rate;  /* Computed request rate (requests per second) based on RTT and total messages. */
    struct sockaddr_in server_addr;
    long tx_cnt;  // # messages transmitted
    long rx_cnt;  // # messages received
} client_thread_data_t;

typedef struct {
    int thread_id;
    int seq;
    char send_buf[MESSAGE_SIZE - 2*sizeof(int)];
} frame;

/*
 * This function runs in a separate client thread to handle communication with the server
 */
void *client_thread_func(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event event, events[MAX_EVENTS];
    struct timeval start1, start2, end1, end2;

    // send messages
    event.events = EPOLLIN;
    event.data.fd = data->socket_fd;
    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event)) {
	perror("epoll_ctl");
	exit(EXIT_FAILURE);
    }

    data->tx_cnt = 0;
    data->rx_cnt = 0;
    socklen_t addr_len = sizeof(data->server_addr);
    gettimeofday(&start1, NULL);
    frame f;
    f.thread_id = data->id;
    sprintf(f.send_buf, "ABCDEFG");
    for (int seq = 0; seq < num_requests; seq++) {
	// send a message to the server, wait for the response, and calculate
	// RTT statistics
        gettimeofday(&start2, NULL);
	data->tx_cnt++;
        while (1) {  // loop until successful transmission
            f.seq = seq;  // send the current seq number
            sendto(data->socket_fd, &f, MESSAGE_SIZE, 0,
                   (struct sockaddr *)&data->server_addr, addr_len);
            f.seq = -1; // reset f.seq so we can check ack
            // see if packet received after .1 sec
            if (epoll_wait(data->epoll_fd, events, MAX_EVENTS, 100) < 1) {
                continue;
            }
            int bytes = recvfrom(data->socket_fd, &f, MESSAGE_SIZE, 0,
                                 (struct sockaddr *)&data->server_addr, &addr_len);
            // if correct ack received, move on
            if (bytes > 0 && f.seq == seq) {
                break;
            }
            // otherwise, re-transmit
        }
        data->rx_cnt++;
        gettimeofday(&end2, NULL);
        long long rtt = (end2.tv_sec - start2.tv_sec) * 1000000LL + (end2.tv_usec - start2.tv_usec);
        data->total_rtt += rtt;
        data->total_messages++;
    }
    // Calculate overall request rate of all messages sent
    gettimeofday(&end1, NULL);
    float elapsed = (end1.tv_sec - start1.tv_sec) + (end1.tv_usec - start1.tv_usec) / 1000000.0;
    data->request_rate = data->total_messages / elapsed;

    // close socket and epoll
    if (close(data->socket_fd) || close(data->epoll_fd)) {
	perror("close");
        exit(EXIT_FAILURE);
    };

    return NULL;
}

/*
 * This function orchestrates multiple client threads to send requests to a server,
 * collect performance data of each threads, and compute aggregated metrics of all threads.
 */
void run_client() {
    pthread_t threads[num_client_threads];
    client_thread_data_t thread_data[num_client_threads];
    struct sockaddr_in server_addr;

    // create client threads
    for (int i = 0; i < num_client_threads; i++) {
	thread_data[i].id = i;
	thread_data[i].total_messages = 0;
	thread_data[i].total_rtt = 0;
	thread_data[i].request_rate = 0;

        // Lower buffer size to ensure packet loss
        int buf_size = 2048;
        setsockopt(thread_data[i].socket_fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
        setsockopt(thread_data[i].socket_fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));

	// initialize and connect to socket
	thread_data[i].socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (thread_data[i].socket_fd == -1) {
	    perror("socket");
	    exit(EXIT_FAILURE);
	}
	struct sockaddr_in server_addr;
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(server_port);
	if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) != 1) {
	    perror("inet_pton");
	    exit(EXIT_FAILURE);
	}
	thread_data[i].server_addr = server_addr;

	// initialize epoll
	thread_data[i].epoll_fd = epoll_create1(0);
	if (thread_data[i].epoll_fd == -1) {
	    perror("epoll_create1");
	    exit(EXIT_FAILURE);
	}

	// actually start the thread
        pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]);
    }

    long long total_rtt = 0;
    long total_messages = 0;
    float total_request_rate = 0.0;
    long total_tx_cnt = 0;
    long total_rx_cnt = 0;
    // wait for client threads to finish and calculate overall
    // RTT/request-rate statistics
    for (int i = 0; i < num_client_threads; i++) {
        pthread_join(threads[i], NULL);
        total_rtt += thread_data[i].total_rtt;
        total_messages += thread_data[i].total_messages;
        total_request_rate += thread_data[i].request_rate;
        total_tx_cnt += thread_data[i].tx_cnt;
        total_rx_cnt += thread_data[i].rx_cnt;
    }
    long lost_pkt_cnt = total_tx_cnt - total_rx_cnt;
    float lost_pkt_rate = 100.0 * (float)lost_pkt_cnt / total_tx_cnt;

    printf("Average RTT: %lld us\n", total_rtt / total_messages);
    printf("Total Request Rate: %f messages/s\n", total_request_rate);
    printf("Lost packet cnt: %ld (%.2f%%)\n", lost_pkt_cnt, lost_pkt_rate);
}

void run_server() {
    // create server socket
    int server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server_fd == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    int buf_size = 2048;
    setsockopt(server_fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
    setsockopt(server_fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));

    // set-up server to allow to listen for connections
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(server_port);

    // bind socket
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    // create instance of epoll
    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }

    // register server_fd to epoll to listen for connections
    struct epoll_event event, events[MAX_EVENTS];
    event.data.fd = server_fd;
    event.events = EPOLLIN;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event);

    // create seq ack stuff
    int *seq_numbers = calloc(num_client_threads, sizeof(int));

    /* Server's run-to-completion event loop */
    while (1) {
	// wait until at least one fd has an event
	int num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        for (int i = 0; i < num_events; i++) {
            if (events[i].data.fd == server_fd) {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
                frame f;
		int bytes_read = recvfrom(server_fd, &f, MESSAGE_SIZE, 0,
					  (struct sockaddr*)&client_addr, &client_len);
		if (bytes_read > 0) {
                    int seq_expected = seq_numbers[f.thread_id];
                    // if expected seq number received, expect next seq number
                    if (seq_expected == f.seq) {
                        seq_numbers[f.thread_id]++;
                    }
                    // either way, send ack
		    sendto(server_fd, &f, bytes_read, 0,
			   (struct sockaddr*)&client_addr, client_len);
		}
            }
        }
    }

    free(seq_numbers);
    // close socket and epoll
    if (close(server_fd) || close(epoll_fd)) {
	perror("close");
        exit(EXIT_FAILURE);
    };
}

int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "server") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);

        run_server();
    } else if (argc > 1 && strcmp(argv[1], "client") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);
        if (argc > 4) num_client_threads = atoi(argv[4]);
        if (argc > 5) num_requests = atoi(argv[5]);

        run_client();
    } else {
        printf("Usage: %s <server|client> [server_ip server_port num_client_threads num_requests]\n", argv[0]);
    }

    return 0;
}
