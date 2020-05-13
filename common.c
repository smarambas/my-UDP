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
#ifdef debug
        printf("Sending ack for message #%u with seq #%u\n", m.ack_num, m.seq);
#endif    
    }
    else {
#ifdef debug 
        printf("Ack for message #%u with seq #%u lost\n", m.ack_num, m.seq);
#endif        
    }
    return;
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

double rand_value(void)
{
    return (double) rand() / RAND_MAX;
}

void sigint_handler(int dummy)
{
    printf("\nShutting down...\n");
    exit(EXIT_SUCCESS);
}
