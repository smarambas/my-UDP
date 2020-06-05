#ifndef COMMON_H
#define COMMON_H

#define BUFF_SIZE       4096
#define BILLION         1000000000

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
int insert_sorted(struct qnode ** headp, struct sockaddr_in * addr, struct msg * m, int index);
struct qnode * append(struct qnode ** tailp, struct sockaddr_in * addr, struct msg * m, int index);
int delete_node(struct qnode ** headp, struct msg * m);
struct qnode * search_node_by_seq(struct qnode * head, unsigned long seq);
struct qnode * search_node_to_serve(struct qnode ** head, int i);
int queue_size(struct qnode * head);
void str_cut(char * str, int begin, int len);
double rand_value(void);
void sigint_handler(int dummy);
struct timespec timespec_normalise(struct timespec ts);
struct timespec timespec_from_double(long double s);
long double timespec_to_double(struct timespec ts);
struct timespec timespec_sub(struct timespec ts1, struct timespec ts2);
struct timespec timespec_add(struct timespec ts1, struct timespec ts2);

#endif
