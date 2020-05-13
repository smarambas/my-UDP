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
void reset_msg(struct msg * m);
void send_ack(int sockfd, struct sockaddr_in * addr, unsigned long my_seq, unsigned long seq_to_ack);
void print_queue(struct qnode * head);
void insert_sorted(struct qnode ** headp, struct sockaddr_in * addr, struct msg * m, int index);
int delete_node(struct qnode ** headp, struct msg * m);
struct qnode * search_node_byseq(struct qnode * head, unsigned long seq);
struct qnode * search_node_to_serve(struct qnode ** head, int i);
struct qnode * pop_first(struct qnode ** headp);
int queue_size(struct qnode * head);
double get_elapsed_time(struct timespec * start, struct timespec * end);
void str_cut(char * str, int begin, int len);
double rand_value(void);
void sigint_handler(int dummy);

#endif
