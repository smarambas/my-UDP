#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <time.h>

#include "myUDP.h"
#include "common.h"

#define TOK_BUF     32
#define TOK_DELIM   " \t\r\n\a"

char* read_line(void)
{
    /*
     * Read a string from the command line and allocate a buffer for it
     */

    char* line = NULL;
    size_t bsize = 0;
    if(getline(&line, &bsize, stdin) == -1)
    {
        fprintf(stderr, "ERROR: getline() call failed.\n");
        exit(-1);
    }

    return line;
}

char** split_line(char* line)
{
    /*
     * Tokenize the string to get a command and its arguments
     */

    int bsize = TOK_BUF, position = 0;
    char** tokens = malloc(bsize * sizeof(char*));
    char* tok;

    if(!tokens)
    {
        fprintf(stderr, "ERROR: malloc() call failed.\n");
        exit(-1);
    }

    tok = strtok(line, TOK_DELIM);

    while(tok != NULL)
    {
        tokens[position] = tok;

        position++;

        if(position > bsize)
        {
            //If we exceed the buffer size, we have to reallocate it
            bsize += TOK_BUF;
            tokens = realloc(tokens, bsize * sizeof(char*));
            if(!tokens)
            {
                fprintf(stderr, "ERROR: realloc() call failed.\n");
                exit(EXIT_FAILURE);
            }
        }

        tok = strtok(NULL, TOK_DELIM);  //next argument
    }
    tokens[position] = NULL;
    return tokens;
}

void reset_msg(struct msg * m)
{
    m->ack = 0;
    m->fin = 0;
    m->syn = 0;
    m->seq = 0;
    m->cmd_t = 0;
    m->ecode = 0;
    m->ack_num = 0;
    m->data_size = 0;
    m->file_size = 0;
    memset(m->data, 0, MAXSIZE + 1 - OFFS);
}

void print_msg(struct msg * m)
{
    printf("m.ack = %d\n"
           "m.fin = %d\n"
           "m.syn = %d\n"
           "m.seq = %u\n"
           "m.endfile = %u\n"
           "m.cmd_t = %d\n"
           "m.ecode = %d\n"
           "m.ack_num = %u\n"
           "m.data_size = %d\n"
           "m.file_size = %d\n",
           m->ack, m->fin, m->syn, m->seq, m->endfile, m->cmd_t, m->ecode, m->ack_num, m->data_size, m->file_size);
}

void send_ack(int sockfd, struct sockaddr_in * addr, unsigned int my_seq, unsigned int seq_to_ack)
{
    int check;
    struct msg m;
    socklen_t addlen = sizeof(*addr);

    reset_msg(&m);
    m.ack = 1;
    m.ack_num = seq_to_ack;
    m.seq = my_seq;
    
    check = sendto(sockfd, (void*) &m, MAXSIZE, 0, (struct sockaddr*) addr, addlen);
    if(check < 0) {
        fprintf(stderr, "Error in sendto\n");
        exit(EXIT_FAILURE);
    }
#ifdef debug
    printf("Sending ack for message #%u with seq #%u\n", m.ack_num, m.seq);
#endif    
    return;
}

int is_ack(struct msg * m, unsigned int myseq)
{
    /*
     * Deprecated
     */
    
    if(m->ack && m->ack_num == myseq) {
        return 1;
    }

    return 0;
}

void print_queue(struct qnode * head)
{
    struct qnode * curr = head;
    
    printf("\n");
    while(curr != NULL) {
        printf("#%u-> ", curr->m->seq);
        curr = curr->next;
    }
    
    printf("null\n\n");
    return;
}

int is_empty(struct qnode * head) 
{
    return head == NULL;
}

void insert_sorted(struct qnode ** headp, struct sockaddr_in * addr, struct msg * m, int index)
{
    /*
     * Insert a new node in the queue, sorted by the sequence number of the message
     */
    
    struct qnode * curr = *headp;
    struct qnode * prev = NULL;
    struct qnode * new;

    new = malloc(sizeof(struct qnode));
    if(!new) {
        //fprintf(stderr, "Error in malloc\n");
        perror("malloc"); 
        exit(EXIT_FAILURE);
    }

    new->addr = addr;
    if(m != NULL) {
        new->m = (struct msg *) malloc(sizeof(struct msg));
        if(!(new->m)) {
            fprintf(stderr, "Error in malloc\n");
            perror("malloc");
            exit(EXIT_FAILURE);
        }
        memcpy(new->m, m, sizeof(struct msg));
    }
    new->index = index;
    
    while(curr != NULL && curr->m->seq < m->seq) {
        prev = curr;
        curr = curr->next;
    }

    if(prev == NULL) {
        new->next = *headp;
        *headp = new;
    }
    else {
        prev->next = new;
        new->next = curr;
    }

    return;
}

int delete_node(struct qnode ** headp, struct msg * m)
{
    /*
     * Delete the node with message m, identified by the sequence number
     */
    
    struct qnode * curr = *headp;
    struct qnode * prev = NULL;

    if(curr != NULL && curr->m->seq == m->seq) {
        *headp = curr->next;
        free(curr);
        return 0;
    }
    
    while(curr != NULL && curr->m->seq != m->seq) {
        prev = curr;
        curr = curr->next;
    }
    
    if(curr == NULL) {
        return 1;
    }
    else {
        prev->next = curr->next;
        free(curr);
        return 0;
    }
}

struct qnode * pop_first(struct qnode ** headp) 
{
    struct qnode * first = *headp;
    
    if(first == NULL) {
        return NULL;
    }
    else {
        *headp = first->next;
        first->next = NULL;
        return first;
    }
}

struct qnode * search_node_byseq(struct qnode * head, unsigned int seq)
{
    struct qnode * curr = head;

    while(curr != NULL) {
        if(curr->m->seq == seq) {
            break; 
        }
        else {
            curr = curr->next;
        }
    }

    return curr;
}

struct qnode * search_node_byindex(struct qnode * head, int i)
{
    struct qnode * curr = head;
    
    while(curr != NULL) {
        if(curr->index == i) {
            break;
        }
        else {
            curr = curr->next;
        }
    }

    return curr;
}

struct qnode * search_node_to_send(struct qnode ** headp, int i)
{
    struct qnode * curr = *headp;

    while(curr != NULL) {
        if(curr->index < 0) {
            curr->index = i;
            break;
        }
        else {
            curr = curr->next;
        }
    }

    return curr;
}

int queue_size(struct qnode * head)
{
    int i = 1;
    struct qnode * curr = head;

    if(is_empty(head)) {
        return 0;
    }
    else {
        while(curr->next != NULL) {
            curr = curr->next;
            i++;
        }

        return i;
    }
}

double get_elapsed_time(struct timespec * start, struct timespec * end)
{
    double delta_s = end->tv_sec - start->tv_sec;
    double delta_ns = end->tv_nsec - start->tv_nsec;
    return delta_s + delta_ns * 1e-9;    
}

void str_cut(char * str, int begin, int len)
{
    int slen = strlen(str);

    if(len < 0) {
        len = slen - begin;
    }
    if(begin + len > slen) {
        len = slen - begin;
    }

    memmove(str + begin, str + begin + len, slen - len + 1);
    return;
}
