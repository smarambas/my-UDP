#ifndef COMMON_H
#define COMMON_H

#define BUFF_SIZE    4096

struct qnode {  
    struct sockaddr_in * addr;
    struct msg * m;
    int index;
    struct qnode * next;
};

char* read_line(void);
char** split_line(char* line);
void print_msg(struct msg * m);
void reset_msg(struct msg * m);
void send_ack(int sockfd, struct sockaddr_in * addr, unsigned int my_seq, unsigned int seq_to_ack);
int is_ack(struct msg * m, unsigned int myseq);
void print_queue(struct qnode * head);
int is_empty(struct qnode * head);
void insert_sorted(struct qnode ** headp, struct sockaddr_in * addr, struct msg * m, int index);
int delete_node(struct qnode ** headp, struct msg * m);
struct qnode * search_node_byseq(struct qnode * head, unsigned int seq);
struct qnode * search_node_byindex(struct qnode * head, int i);
struct qnode * search_node_to_send(struct qnode ** head, int i);
struct qnode * pop_first(struct qnode ** headp);
int queue_size(struct qnode * head);
double get_elapsed_time(struct timespec * start, struct timespec * end);
void str_cut(char * str, int begin, int len);

#endif
