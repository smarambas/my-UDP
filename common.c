#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <time.h>
#include <math.h>

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
    char ** tokens = malloc(bsize * sizeof(char*));
    char * tok;

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
    m->startfile = 0;
    m->endfile = 0;
    m->ecode = 0;
    m->ack_num = 0;
    m->data_size = 0;
    m->file_size = 0;
    memset(m->data, 0, PAYLOAD_SIZE);
}

void send_ack(int sockfd, struct sockaddr_in * addr, unsigned long my_seq, unsigned long seq_to_ack)
{
    int check;
    struct msg m;
    socklen_t addlen = sizeof(*addr);

    reset_msg(&m);
    m.ack = 1;
    m.ack_num = seq_to_ack;
    m.seq = my_seq;
    
    if(rand_value() > P) {
        check = sendto(sockfd, (void*) &m, sizeof(struct msg), 0, (struct sockaddr*) addr, addlen);
        if(check < 0) {
            fprintf(stderr, "Error in sendto\n");
            exit(EXIT_FAILURE);
        }
#ifdef verbose
        printf("Sending ack for message #%lu with seq #%lu\n", m.ack_num, m.seq);
#endif    
    }
    else {
#ifdef verbose 
        //printf("Ack for message #%lu with seq #%lu lost\n", m.ack_num, m.seq);
#endif        
    }
    return;
}

void print_queue(struct qnode * head)
{
    struct qnode * curr = head;
    
    printf("\n");
    while(curr != NULL) {
        printf("#%lu-> ", curr->m->seq);
        curr = curr->next;
    }
    
    printf("null\n\n");
    return;
}

void insert_sorted(struct qnode ** headp, struct sockaddr_in * addr, struct msg * m, int index)
{
    /*
     * Insert a new node in the queue, sorted by the sequence number of the message, ignoring the duplicates
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

    if(prev == NULL && curr == NULL) {  //empty list
        new->next = *headp;
        *headp = new;
    }
    else if(prev == NULL && curr != NULL && curr->m->seq != m->seq) {   //head of the list
        new->next = *headp;
        *headp = new;
    }
    else if(prev != NULL && curr == NULL) { //end of the list
        prev->next = new;
        new->next = curr;
    }
    else if(prev != NULL && curr != NULL && prev->m->seq != m->seq && curr->m->seq != m->seq) {
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

struct qnode * search_node_byseq(struct qnode * head, unsigned long seq)
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

struct qnode * search_node_to_serve(struct qnode ** headp, int i)
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

    if(head == NULL) {
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

struct timespec timespec_normalise(struct timespec ts)
{
    while(ts.tv_nsec >= BILLION) {
        ++(ts.tv_sec);
        ts.tv_nsec -= BILLION;
    }

    while(ts.tv_nsec <= -BILLION) {
        --(ts.tv_sec);
        ts.tv_nsec += BILLION;
    }
        
    if(ts.tv_nsec < 0 && ts.tv_sec > 0) {
        /* 
        * Negative nanoseconds while seconds is positive.
        * Decrement tv_sec and roll tv_nsec over.
        */
    
        --(ts.tv_sec);
        ts.tv_nsec = BILLION - (-1 * ts.tv_nsec);
    }
    else if(ts.tv_nsec > 0 && ts.tv_sec < 0) {
        /* 
        * Positive nanoseconds while seconds is negative.
        * Increment tv_sec and roll tv_nsec over.
        */
    
        ++(ts.tv_sec);
        ts.tv_nsec = -BILLION - (-1 * ts.tv_nsec);
    }

    return ts;
}

struct timespec timespec_from_double(long double s)
{
    struct timespec ts = {
        .tv_sec  = s,
        .tv_nsec = (s - (long)(s)) * BILLION,
    };

    return timespec_normalise(ts);
}

long double timespec_to_double(struct timespec ts)
{
    return ((long double)(ts.tv_sec) + ((long double)(ts.tv_nsec) / BILLION));
}

struct timespec timespec_add(struct timespec ts1, struct timespec ts2)
{
    /* 
    * Normalise inputs to prevent tv_nsec rollover if whole-second values are packed in it
    */
    
    ts1 = timespec_normalise(ts1);
    ts2 = timespec_normalise(ts2);
    
    ts1.tv_sec  += ts2.tv_sec;
    ts1.tv_nsec += ts2.tv_nsec;
    
    return timespec_normalise(ts1);
}

struct timespec timespec_sub(struct timespec ts1, struct timespec ts2)
{
    /* 
    * Normalise inputs to prevent tv_nsec rollover if whole-second values are packed in it
    */
    
    ts1 = timespec_normalise(ts1);
    ts2 = timespec_normalise(ts2);

    ts1.tv_sec  -= ts2.tv_sec;
    ts1.tv_nsec -= ts2.tv_nsec;

    return timespec_normalise(ts1);
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

double rand_value(void)
{
    return (double) rand() / RAND_MAX;
}

void sigint_handler(int dummy)
{
    printf("\nShutting down...\n");
    exit(EXIT_SUCCESS);
}
