#ifndef CLIENT_H
#define CLIENT_H

void open_connection(struct qnode ** send_queue);
void * send_message(void * args);
void send_cmd(struct qnode ** send_queue);
void send_file(struct qnode ** send_queue, char * filename);
void * msg_handler(void * args);
void * recv_msg(void * args);
void print_mylist(void);

#endif
