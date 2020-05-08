#ifndef CLIENT_H
#define CLIENT_H

int open_connection(void);
void * send_message(void * args);
void send_cmd(struct qnode ** send_queue);
void * msg_handler(void * args);
void * recv_answer(void * args);

#endif
