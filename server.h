#ifndef SERVER_H
#define SERVER_H

int accept_connection(int listensd, struct sockaddr_in * cliaddr, unsigned long * cliseq);
void complete_handshake(unsigned long * cliseq, struct qnode ** send_queue);
void * send_message(void * args);
void send_list(struct qnode ** send_queue);
void send_file(struct qnode ** send_queue, char * filename);
void * msg_handler(void * args);
void recv_msg(struct qnode ** send_queue);

#endif
