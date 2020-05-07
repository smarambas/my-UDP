#ifndef SERVER_H
#define SERVER_H

int accept_connection(int listensd, struct sockaddr_in * cliaddr, unsigned int * cliseq);
int complete_handshake(unsigned int * cliseq);
void * send_message(void * args);
int send_list(struct qnode ** send_queue);
//int recv_acks(struct qnode ** send_queue);
//int recv_cmd(struct qnode ** send_queue);
void * msg_handler(void * args);
void recv_msgs(struct qnode ** send_queue);

#endif
