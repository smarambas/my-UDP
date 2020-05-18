#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/select.h>

#include "myUDP.h"
#include "common.h"
#include "server.h"

/* Global Variables */
struct timespec T  = {1, 0};                                        //timeout {seconds, nanoseconds}

unsigned long myseq;                                                //sequence number
unsigned long expected_seq = 0;                                     //next expected sequence number
int connsd, fd, closed = 0, opened = 0, first_open = 1;  
struct sockaddr_in cliaddr;                                         //client address
struct qnode * rec_queue = NULL;                                    //receiving queue
struct qnode * send_base = NULL;                                    //pointer to the first element of the sending window
int acked[N] = {0};                                                 //array to report to sending threads that an ack is received
int snd_indexes[N] = {0};                                           //array of indexes for the sending threads
int rcv_indexes[N] = {0};                                           //array of indexes for the receiving threads
char new_file[BUFF_SIZE] = {0};                                     //requested file name
pthread_mutex_t index_mutex = PTHREAD_MUTEX_INITIALIZER;            //to sync the index choice between threads
pthread_mutex_t rec_index_mutex = PTHREAD_MUTEX_INITIALIZER;        //to sync the index choice between handling threads
pthread_mutex_t mutexes[N];                                         //to sync the sending threads and the receiving thread
pthread_mutex_t acked_mutexes[N];                                   //to make the access to acked[i] more robust
pthread_mutex_t rec_mutex = PTHREAD_MUTEX_INITIALIZER;              //to sync the accesses to rec_queue
pthread_mutex_t snd_mutex = PTHREAD_MUTEX_INITIALIZER;              //to sync the accesses to snd_queue
pthread_mutex_t exp_mutex = PTHREAD_MUTEX_INITIALIZER;              //to sync the updates to expected_seq
pthread_mutex_t close_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t wr_mutex = PTHREAD_MUTEX_INITIALIZER;               //to sync writes on the file
pthread_mutex_t timer_mutex = PTHREAD_MUTEX_INITIALIZER;            //to sync updates to the timeout timer
pthread_cond_t index_cond = PTHREAD_COND_INITIALIZER;               //to signal the release of an index
pthread_cond_t rec_cond = PTHREAD_COND_INITIALIZER;                 //to signal the receiving of messages
pthread_cond_t exp_cond = PTHREAD_COND_INITIALIZER;                 //to signal updates to expected_seq
pthread_cond_t sb_cond = PTHREAD_COND_INITIALIZER;                  //to signal updates to send_base
pthread_cond_t snd_cond = PTHREAD_COND_INITIALIZER;                 //to signal to the sending thread that there is something to send
pthread_cond_t ack_cond[N];                                         //to signal to thread i the arriving of an ack
pthread_t * s_tid;                                                  //array of sending threads


int accept_connection(int listensd, struct sockaddr_in * cliaddr, unsigned long * cliseq)
{
    /*
     * Wait for a SYN message and create a new socket for the connection
     */

    int sd, check = 0;
    struct msg m;
    socklen_t addlen = sizeof(*cliaddr);
    fd_set rset;
    
    FD_ZERO(&rset);

    while(check == 0) {
        FD_SET(listensd, &rset);
        
        if(select(listensd+1, &rset, NULL, NULL, NULL) < 0) {
            perror("select");
            exit(EXIT_FAILURE);
        }
        
        if((recvfrom(listensd, (void*) &m, sizeof(struct msg), 0, (struct sockaddr*) cliaddr, &addlen)) < 0) {
            perror("recvfrom");
            exit(EXIT_FAILURE);
        }
        
        if(m.syn == 1 && m.seq > expected_seq) {
            check = 1;
        }
    }

    printf("\nReceived SYN message from %s on port %u\n", inet_ntoa((*cliaddr).sin_addr), ntohs((*cliaddr).sin_port));
    *cliseq = m.seq;
    expected_seq = m.seq + 1;
    printf("Spawning a connection socket...\n");

    if((sd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
        perror("Error in socket");
        exit(-1);
    }

    return sd;
}

void complete_handshake(unsigned long * cliseq, struct qnode ** send_queue)
{
    /*
     * Send a SYN-ACK to the client and wait for an answer to establish a connection
     */

    int check;
    struct msg m;
        
    reset_msg(&m);
    m.seq = myseq;
    m.startfile = 1;
    m.syn = 1;
    m.ack = 1;
    m.ack_num = *cliseq;
    
    check = pthread_mutex_lock(&snd_mutex);
    if(check != 0) {
        perror("pthread_mutex_lock");
        exit(EXIT_FAILURE);
    }
    
    insert_sorted(send_queue, &cliaddr, &m, -1);

    check = pthread_mutex_unlock(&snd_mutex);
    if(check != 0) {
        perror("pthread_mutex_unlock");
        exit(EXIT_FAILURE);
    }    
    
    pthread_cond_broadcast(&snd_cond);
    
    return;
}

void * send_message(void * args)
{
    /*
     * Try to send a message and handle the ritrasmissions until an ack arrives
     * It can use both a fixed value timer or an adaptive timer
     */
    
    int i = -1, j, check = 0;
    struct qnode ** snd_queue = (struct qnode **) args;
    struct qnode * node = NULL;
    struct msg m;
    socklen_t addlen = sizeof(cliaddr);
    struct timespec timeout;
    struct timespec time_to_wait;
    long double temp;
    
#ifdef adaptive    
    /* 
     * variables needed for the adaptive timer
     */
    int rx = 0;
    struct timespec sampleRTT = {0, 0};
    long double estimatedRTT = 0.0;
    long double devRTT = 0.0;
    struct timespec start_t = {0, 0};
    struct timespec end_t = {0, 0};
#endif
    
    srand(time(NULL));
    
    check = pthread_mutex_lock(&index_mutex);
    if(check != 0) {
        perror("pthread_mutex_lock");
        exit(EXIT_FAILURE);
    }

    //select the index
    for(j = 0; j < N; j++) {
        if(snd_indexes[j] == 0) {
            i = j;  //thread index
            snd_indexes[j] = 1;
            break;
        }
    }
    
#ifdef verbose
    printf("I'm thread %lu with index %d\n", pthread_self(), i);
#endif
    
    check = pthread_mutex_unlock(&index_mutex);
    if(check != 0) {
        perror("pthread_mutex_unlock");
        exit(EXIT_FAILURE);
    }
    
    timeout = T;
    
    while(!closed) {
        check = pthread_mutex_lock(&snd_mutex);
        if(check != 0) {
            perror("pthread_mutex_lock");
            exit(EXIT_FAILURE);
        }
        
        while(*snd_queue == NULL) {
            check = pthread_cond_wait(&snd_cond, &snd_mutex);
            if(check != 0) {
                perror("pthread_cond_wait");
                exit(EXIT_FAILURE);
            }
        }

        node = search_node_to_serve(snd_queue, i);  //search the snd_queue for a node that has not yet been sent
        
        check = pthread_mutex_unlock(&snd_mutex);
        if(check != 0) {
            perror("pthread_mutex_unlock");
            exit(EXIT_FAILURE);
        }

        if(node != NULL) {
#ifdef verbose
            printf("Thread %lu sending msg #%lu\n", pthread_self(), node->m->seq);
#endif
            if(node->m->startfile == 1) {
                send_base = node;
            }
            
//             check = pthread_mutex_lock(&timer_mutex);
//             if(check != 0) {
//                 perror("pthread_mutex_lock");
//                 exit(EXIT_FAILURE);
//             }
//             
//             timeout = T;
//             
//             check = pthread_mutex_unlock(&timer_mutex);
//             if(check != 0) {
//                 perror("pthread_mutex_unlock");
//                 exit(EXIT_FAILURE);
//             }
        
            reset_msg(&m);
            memcpy(&m, node->m, sizeof(struct msg));    //extract the message from the node
        
            check = pthread_mutex_lock(&acked_mutexes[i]);
            if(check != 0) {
                perror("pthread_mutex_lock");
                exit(EXIT_FAILURE);
            }
            
            acked[i] = 0;   //added for robustness
            
            check = pthread_mutex_unlock(&acked_mutexes[i]);
            if(check != 0) {
                perror("pthread_mutex_unlock");
                exit(EXIT_FAILURE);
            }
        
            if(rand_value() > P) {  //if the random value is greater than the probability to lose the message, then it is sent to destination
                check = sendto(connsd, (void *) &m, sizeof(struct msg), 0, (struct sockaddr *) &cliaddr, addlen);
                if(check < 0) {
                    perror("sendto");
                    exit(EXIT_FAILURE);
                }       
#ifdef adaptive 
//                 check = pthread_mutex_lock(&snd_mutex);
//                 if(check != 0) {
//                     perror("pthread_mutex_lock");
//                     exit(EXIT_FAILURE);
//                 }
//                 
//                 if(node->m->seq == send_base->m->seq) { 
                clock_gettime(CLOCK_THREAD_CPUTIME_ID, &start_t);   //measure the time between a succesful send and the arrive of an ack
//                 }
//                 
//                 check = pthread_mutex_unlock(&snd_mutex);
//                 if(check != 0) {
//                     perror("pthread_mutex_unlock");
//                     exit(EXIT_FAILURE);
//                 }
#endif            
#ifdef verbose
                printf("Message sent to server with seq #%lu (tx)\n", m.seq);
#endif
            }
            else {
#ifdef verbose 
                printf("Message #%lu lost\n", m.seq);
#endif            
            }
        
            check = pthread_mutex_lock(&mutexes[i]);
            if(check != 0) {
                perror("pthread_mutex_lock");
                exit(EXIT_FAILURE);
            }
        
            while(!acked[i]) {  //until the message is not acked, continue to retransmit it
                clock_gettime(CLOCK_REALTIME, &time_to_wait);
                time_to_wait = timespec_add(time_to_wait, timeout);
                
                check = pthread_cond_timedwait(&ack_cond[i], &mutexes[i], &time_to_wait);
                if(check != 0) {
                    if(check == ETIMEDOUT) {    //if the timer expires, then we must try to send again the message
#ifdef adaptive                
                        rx = 1;
#endif                     
                        if(rand_value() > P) {
                            check = sendto(connsd, (void *) &m, sizeof(struct msg), 0, (struct sockaddr *) &cliaddr, addlen);
                            if(check < 0) {
                                perror("sendto");
                                exit(EXIT_FAILURE);
                            }                 
#ifdef verbose
                            printf("Message sent to server with seq #%lu (rx)\n", m.seq);
#endif
                        }
                        else {
#ifdef verbose 
                            printf("Message #%lu lost\n", m.seq);
#endif            
                        }
                    
                        //if the timer expires, it doubles for a maximum of MAX_TIMEOUT_INTERVAL seconds
                        timeout = timespec_add(timeout, timeout);   
                        temp = timespec_to_double(timeout);
                        if(temp > MAX_TIMEOUT_INTERVAL) {
                            temp = MAX_TIMEOUT_INTERVAL;
                        }
                        timeout = timespec_from_double(temp);               
                    }
                    else {
                        perror("pthread_cond_timedwait");
                        exit(EXIT_FAILURE);
                    }
                }
            }
            
            check = pthread_mutex_lock(&acked_mutexes[i]);
            if(check != 0) {
                perror("pthread_mutex_lock");
                exit(EXIT_FAILURE);
            }
            
            acked[i] = 0;
            
            check = pthread_mutex_unlock(&acked_mutexes[i]);
            if(check != 0) {
                perror("pthread_mutex_unlock");
                exit(EXIT_FAILURE);
            }
        
#ifdef adaptive 
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &end_t); //after the arrive of the ack, we can stop the measurement
            if(!rx) {   //if there wasn't a retrasmission, calculate the new RTT values
                sampleRTT = timespec_sub(end_t, start_t);
                temp = timespec_to_double(sampleRTT);
                estimatedRTT = (1 - ALFA) * estimatedRTT + ALFA * temp;
                devRTT = (1 - BETA) * devRTT + BETA * fabsl(temp - estimatedRTT);
                
                //printf("sampleRTT = %Lf\nestimatedRTT = %Lf\ndevRTT = %Lf\n", temp, estimatedRTT, devRTT);
                
    //             if(node->m->seq == send_base->m->seq) { //update the timer value only if the node is the send_base
//                 check = pthread_mutex_lock(&timer_mutex);
//                 if(check != 0) {
//                     perror("pthread_mutex_lock");
//                     exit(EXIT_FAILURE);
//                 }
            
                temp = estimatedRTT + 4 * devRTT;
                if(temp > MAX_TIMEOUT_INTERVAL) {
                    temp = MAX_TIMEOUT_INTERVAL;
                }
                
                printf("Timeout = %Lf\n", temp);
                
//                 T = timespec_from_double(temp);            
                timeout = timespec_from_double(temp);
                            
//                 check = pthread_mutex_unlock(&timer_mutex);
//                 if(check != 0) {
//                     perror("pthread_mutex_unlock");
//                     exit(EXIT_FAILURE);
//                 }
    //             }
            }
            
            rx = 0;
#endif
            check = pthread_mutex_lock(&snd_mutex);
            if(check != 0) {
                perror("pthread_mutex_lock");
                exit(EXIT_FAILURE);
            }
            
            while(send_base->m->seq != node->m->seq) {  //if the node is not the send base, it must wait to become it
                check = pthread_cond_wait(&sb_cond, &snd_mutex);
                if(check != 0) {
                    perror("pthread_cond_wait");
                    exit(EXIT_FAILURE);
                }
            }
            
            //select the new send base, delete the node from the sending queue and search for a new node, if exists
            send_base = send_base->next;
            
            if(node->m->fin == 1) {
                closed = 1;
            }
            
            delete_node(snd_queue, &m);
        
            check = pthread_mutex_unlock(&snd_mutex);
            if(check != 0) {
                perror("pthread_mutex_unlock");
                exit(EXIT_FAILURE);
            }
            
            pthread_cond_broadcast(&sb_cond);   //signal that a new send base has been chosen
            
            check = pthread_mutex_unlock(&mutexes[i]);
            if(check != 0) {
                perror("pthread_mutex_unlock");
                exit(EXIT_FAILURE);
            }
        }

    }

    pthread_exit(NULL);
}

void send_list(struct qnode ** send_queue)
{
    /*
     * Send a list of the server's file
     */
    
    char * buff;    
    unsigned long bsize = PAYLOAD_SIZE; 
    size_t blen = 0;
    struct msg m;
    int dim, i, sizetocpy, check, first = 1;
    struct dirent ** filelist;

    buff = malloc(bsize * sizeof(char));
    if(!buff) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    
    memset(buff, 0, bsize);
    
    check = scandir("./files_server/", &filelist, 0, versionsort);  //sort the list alphabetically
    if(check < 0) {
        perror("scandir");
        exit(EXIT_FAILURE);
    }
    else {
        blen = strlen(buff);
        for(i = 0; i < check; i++) {
            if(filelist[i]->d_name[0] != '.') {
                if((blen + strlen(filelist[i]->d_name)) >= bsize) {
                    bsize += PAYLOAD_SIZE; 
                    buff = realloc(buff, bsize);
                    if(!buff) {
                        perror("realloc");
                        exit(EXIT_FAILURE);
                    }
                }
                strcat(buff, filelist[i]->d_name);
                strcat(buff, "\n");
            }
        }
        free(filelist);
    }

    blen = strlen(buff);
    sizetocpy = (int) blen;

    while(sizetocpy > 0) {
        reset_msg(&m);

        if(sizetocpy > PAYLOAD_SIZE) {  //if the chunk is greater than the PAYLOAD_SIZE, then it's not the last message of the ordered sequence
            dim = PAYLOAD_SIZE; 
            m.endfile = 0;
        }
        else if(sizetocpy == PAYLOAD_SIZE) {    
            dim = PAYLOAD_SIZE; 
            m.endfile = 1;
        }
        else {
            dim = sizetocpy;
            m.endfile = 1;
        }
        
        if(first) {
            m.startfile = 1;
            first = 0;
        }
        else {
            m.startfile = 0;
        }

        myseq += dim;
        m.seq = myseq;
        m.data_size = dim;
        m.file_size = blen;
        m.cmd_t = 1;
        memcpy(m.data, buff, dim);
        str_cut(buff, 0, dim);
        
        check = pthread_mutex_lock(&snd_mutex);
        if(check != 0) {
            perror("pthread_mutex_lock");
            exit(EXIT_FAILURE);
        }
        
        insert_sorted(send_queue, &cliaddr, &m, -1);

        check = pthread_mutex_unlock(&snd_mutex);
        if(check != 0) {
            perror("pthread_mutex_unlock");
            exit(EXIT_FAILURE);
        }    
        
        pthread_cond_broadcast(&snd_cond);
        
#ifdef verbose
            printf("Inserted message with seq #%lu in the queue\n", m.seq);
#endif
        
        sizetocpy = (int) sizetocpy - dim; 
    }
    
    free(buff);
    
    return;
}

void send_file(struct qnode ** send_queue, char * filename)
{
    /*
     * Open the file, divide it in chunks, insert them in the sending queue and send them
     */
    
    int fd, check, first = 1, bsize = PAYLOAD_SIZE, count = 0;
    long filesize, dim = 0;    
    struct msg m;
    char file[BUFF_SIZE] = "files_server/";
    
    strcat(file, filename);

    fd = open(file, O_RDONLY);
    if(fd == -1) {
        if(errno == ENOENT) {   //if the file doesn't exist, send an error code
            reset_msg(&m);
            
            myseq += 1;
            m.seq = myseq;
            m.cmd_t = 2;
            m.data_size = 1;
            m.startfile = 1;
            m.ecode = clierror;
            
            check = pthread_mutex_lock(&snd_mutex);
            if(check != 0) {
                perror("pthread_mutex_lock");
                exit(EXIT_FAILURE);
            }
            
            insert_sorted(send_queue, &cliaddr, &m, -1);

            check = pthread_mutex_unlock(&snd_mutex);
            if(check != 0) {
                perror("pthread_mutex_unlock");
                exit(EXIT_FAILURE);
            }    
            
            pthread_cond_broadcast(&snd_cond);
            
            return;
        }
        else {  //if the open fails, send an error code and exit
            reset_msg(&m);
            
            myseq += 1;
            m.seq = myseq;
            m.cmd_t = 2;
            m.data_size = 1;
            m.startfile = 1;
            m.ecode = serverror;
            
            check = pthread_mutex_lock(&snd_mutex);
            if(check != 0) {
                perror("pthread_mutex_lock");
                exit(EXIT_FAILURE);
            }
            
            insert_sorted(send_queue, &cliaddr, &m, -1);

            check = pthread_mutex_unlock(&snd_mutex);
            if(check != 0) {
                perror("pthread_mutex_unlock");
                exit(EXIT_FAILURE);
            }    
            
            pthread_cond_broadcast(&snd_cond);
            
            perror("open");
            exit(EXIT_FAILURE);
        }
    }

    filesize = (long) lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    
    do {
        reset_msg(&m);
                
        if((filesize - dim) <= bsize) { //if the chunk is less than the PAYLOAD_SIZE, then it's the last message of the ordered sequence
            bsize = filesize - dim;
            m.endfile = 1;
        }
        else {
            m.endfile = 0;
        }
        
        count = read(fd, m.data, bsize);
        while(count != bsize) {
            printf("Error: read less bytes\n");
            if(count < 0) {
                perror("read");
                exit(EXIT_FAILURE);
            }
            lseek(fd, -count, SEEK_CUR);
            memset(m.data, 0, PAYLOAD_SIZE);
            count = read(fd, m.data, bsize);
        }
                
        myseq += count;
        m.seq = myseq;
        m.data_size = (unsigned long) count;
        m.file_size = filesize;
        m.cmd_t = 2;
        m.ecode = success;        
        
        if(first == 1) {
            m.startfile = 1;
            first = 0;
        }
        else {
            m.startfile = 0;
        }
        
        check = pthread_mutex_lock(&snd_mutex);
        if(check != 0) {
            perror("pthread_mutex_lock");
            exit(EXIT_FAILURE);
        }
        
        insert_sorted(send_queue, &cliaddr, &m, -1);

        check = pthread_mutex_unlock(&snd_mutex);
        if(check != 0) {
            perror("pthread_mutex_unlock");
            exit(EXIT_FAILURE);
        }    
        
        pthread_cond_broadcast(&snd_cond);
        
        dim += count;        
                
#ifdef verbose
            printf("Inserted message with seq #%lu in the queue\n", m.seq);
#endif
    }
    while(dim < filesize);
    
    return;
}

void * msg_handler(void * args) 
{
    /*
     * Manage the packets in the correct order
     */
    
    int i = 0, check, rec_base = 0, residual = 0, count = 0;
    struct qnode ** snd_queue = (struct qnode **) args;
    struct qnode * node = NULL;
    struct qnode * msg_node = NULL;
    struct msg m;
    
    check = pthread_mutex_lock(&rec_index_mutex);
    if(check != 0) {
        perror("pthread_mutex_lock");
        exit(EXIT_FAILURE);
    }
    
    //select an index that will last for the whole execution
    for(int j = 0; j < N; j++) {
        if(rcv_indexes[j] == 0) {
            i = j;  //thread index
            rcv_indexes[j] = 1;
            break;
        }
    }
    
    check = pthread_mutex_unlock(&rec_index_mutex);
    if(check != 0) {
        perror("pthread_mutex_unlock");
        exit(EXIT_FAILURE);
    }
    
    while(1) {
        check = pthread_mutex_lock(&rec_mutex);
        if(check != 0) {
            perror("pthread_mutex_lock");
            exit(EXIT_FAILURE);
        }
        
        while(rec_queue == NULL) {  //if there are no messages in the receiving queue, wait
            check = pthread_cond_wait(&rec_cond, &rec_mutex);
            if(check != 0) {
                perror("pthread_cond_wait");
                exit(EXIT_FAILURE);
            }
        }
        
        msg_node = search_node_to_serve(&rec_queue, i); //select a new node that has not yet been sent
        
        check = pthread_mutex_unlock(&rec_mutex);
        if(check != 0) {
            perror("pthread_mutex_unlock");
            exit(EXIT_FAILURE);
        }
        
        if(msg_node != NULL) {
#ifdef verbose            
            printf("Handler %d got msg #%lu\n", i, msg_node->m->seq);
#endif             
            if(msg_node->m->startfile == 1) {   //if the message is the first of the ordered sequence, then it's the receive base
                rec_base = 1;               
                first_open = 1;
            }
            else {
                rec_base = 0;
            }
            
            if(msg_node->m->ack == 1) {    //if the message is an ack 
                node = search_node_by_seq((*snd_queue), msg_node->m->ack_num);   //search in the sending queue to find a message that is waiting for the ack
                if(node != NULL) {  //if there is a node in the queue with the correct sequence
                    check = pthread_mutex_lock(&acked_mutexes[node->index]);
                    if(check != 0) {
                        perror("pthread_mutex_lock");
                        exit(EXIT_FAILURE);
                    }
                    
                    acked[node->index] = 1; //report to the sending thread that the message is acked
                    
                    check = pthread_mutex_unlock(&acked_mutexes[node->index]);
                    if(check != 0) {
                        perror("pthread_mutex_unlock");
                        exit(EXIT_FAILURE);
                    }
                    
                    pthread_cond_signal(&ack_cond[node->index]);    
#ifdef verbose
                    printf("Ack received for message #%lu (%d)\n", msg_node->m->ack_num, node->index);
#endif
                    if(!opened) {   //if the message is a syn, open the connection
                        opened = 1;
                        printf("\nConnection established with:\n"
                               "Client address\t%s\n"
                               "Client port\t%u\n\n", inet_ntoa((cliaddr).sin_addr), ntohs((cliaddr).sin_port));
#ifdef verbose 
                        printf("myseq = %lu\n\n", myseq);
#endif        
                    }
                }
            }
            else {  //if the message is not an ack
                if(!rec_base) { //if the node is not the receiving base, wait to become it
                    check = pthread_mutex_lock(&exp_mutex);
                    if(check != 0) {
                        perror("pthread_mutex_lock");
                        exit(EXIT_FAILURE);
                    }
                    
                    while((expected_seq - 1 + msg_node->m->data_size) != msg_node->m->seq) {
                        check = pthread_cond_wait(&exp_cond, &exp_mutex);
                        if(check != 0) {
                            perror("pthread_cond_wait");
                            exit(EXIT_FAILURE);
                        }
                    }
                    
                    check = pthread_mutex_unlock(&exp_mutex);
                    if(check != 0) {
                        perror("pthread_mutex_unlock");
                        exit(EXIT_FAILURE);
                    }
                }
                
                if(msg_node->m->cmd_t == 1) {  //if the message is a list command
                    send_list(snd_queue);
                }
                else if(msg_node->m->cmd_t == 2) {  //if the message is a get command
                    send_file(snd_queue, msg_node->m->data);
                }
                else if(msg_node->m->cmd_t == 3) {  //if the message is a post command
                    if(first_open && rec_base) {
                        memset(new_file, 0, BUFF_SIZE);
                        strcat(new_file, "files_server/");
                        strcat(new_file, msg_node->m->data);
                        
                        fd = open(new_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                        if(fd == -1) {  //if an error occurred creating the file, send an error message and exit
                            reset_msg(&m);
                            myseq += 1;
                            m.seq = myseq;
                            m.startfile = 1;
                            m.endfile = 1;
                            m.data_size = 1;
                            m.cmd_t = 3;
                            m.ecode = serverror;
                            
                            check = pthread_mutex_lock(&snd_mutex);
                            if(check != 0) {
                                perror("pthread_mutex_lock");
                                exit(EXIT_FAILURE);
                            }
                            
                            insert_sorted(snd_queue, &cliaddr, &m, -1);

                            check = pthread_mutex_unlock(&snd_mutex);
                            if(check != 0) {
                                perror("pthread_mutex_unlock");
                                exit(EXIT_FAILURE);
                            }    
                            
                            pthread_cond_broadcast(&snd_cond);
                            
                            sleep(10);
                            
                            perror("open");
                            exit(EXIT_FAILURE);
                        }
                        
                        first_open = 0;
                    }
                    
                    check = pthread_mutex_lock(&wr_mutex);
                    if(check != 0) {
                        perror("pthread_mutex_lock");
                        exit(EXIT_FAILURE);
                    }
                                        
                    count = write(fd, msg_node->m->data, msg_node->m->data_size);   //write the data on the file
                    if(count == -1) {
                        reset_msg(&m);
                        myseq += 1;
                        m.seq = myseq;
                        m.startfile = 1;
                        m.endfile = 1;
                        m.data_size = 1;
                        m.cmd_t = 3;
                        m.ecode = serverror;
                        
                        check = pthread_mutex_lock(&snd_mutex);
                        if(check != 0) {
                            perror("pthread_mutex_lock");
                            exit(EXIT_FAILURE);
                        }
                        
                        insert_sorted(snd_queue, &cliaddr, &m, -1);

                        check = pthread_mutex_unlock(&snd_mutex);
                        if(check != 0) {
                            perror("pthread_mutex_unlock");
                            exit(EXIT_FAILURE);
                        }    
                        
                        pthread_cond_broadcast(&snd_cond);
                        
                        sleep(10);
                        
                        perror("write");
                        exit(EXIT_FAILURE);
                    }
                    else if(count < (long int) msg_node->m->data_size) {
                        residual = msg_node->m->data_size - count;
                        count = 0;
                        while(count < residual) {
                            count = write(fd, msg_node->m->data, residual);
                            if(count == -1) {
                                myseq += 1;
                                m.seq = myseq;
                                m.startfile = 1;
                                m.endfile = 1;
                                m.data_size = 1;
                                m.cmd_t = 3;
                                m.ecode = serverror;
                                
                                check = pthread_mutex_lock(&snd_mutex);
                                if(check != 0) {
                                    perror("pthread_mutex_lock");
                                    exit(EXIT_FAILURE);
                                }
                                
                                insert_sorted(snd_queue, &cliaddr, &m, -1);

                                check = pthread_mutex_unlock(&snd_mutex);
                                if(check != 0) {
                                    perror("pthread_mutex_unlock");
                                    exit(EXIT_FAILURE);
                                }    
                                
                                pthread_cond_broadcast(&snd_cond);
                                
                                sleep(10);
                                
                                perror("write");
                                exit(EXIT_FAILURE);
                            }
                            residual -= count;
                        }
                    }
                    
                    if(msg_node->m->endfile == 1) { //if the message is the last in the ordered sequence, close the file, send a message of success
                        close(fd);
                        reset_msg(&m);
                        myseq += 1;
                        m.seq = myseq;
                        m.startfile = 1;
                        m.endfile = 1;
                        m.data_size = 1;
                        m.cmd_t = 3;
                        m.ecode = success;
                        
                        check = pthread_mutex_lock(&snd_mutex);
                        if(check != 0) {
                            perror("pthread_mutex_lock");
                            exit(EXIT_FAILURE);
                        }
                        
                        insert_sorted(snd_queue, &cliaddr, &m, -1);

                        check = pthread_mutex_unlock(&snd_mutex);
                        if(check != 0) {
                            perror("pthread_mutex_unlock");
                            exit(EXIT_FAILURE);
                        }    
                        
                        pthread_cond_broadcast(&snd_cond);
                    }
                    
                    check = pthread_mutex_unlock(&wr_mutex);
                    if(check != 0) {
                        perror("pthread_mutex_unlock");
                        exit(EXIT_FAILURE);
                    }
                }
                else if(msg_node->m->fin == 1) {    //if it's a fin message, send a fin and signal the closing of the connection
                    reset_msg(&m);
                    myseq += 1;
                    m.seq = myseq;
                    m.data_size = 1;
                    m.startfile = 1;
                    m.endfile = 1;
                    m.fin = 1;
                    
                    check = pthread_mutex_lock(&snd_mutex);
                    if(check != 0) {
                        perror("pthread_mutex_lock");
                        exit(EXIT_FAILURE);
                    }
                    
                    insert_sorted(snd_queue, &cliaddr, &m, -1);

                    check = pthread_mutex_unlock(&snd_mutex);
                    if(check != 0) {
                        perror("pthread_mutex_unlock");
                        exit(EXIT_FAILURE);
                    }    
                    
                    pthread_cond_broadcast(&snd_cond);
                }
                
                check = pthread_mutex_lock(&exp_mutex);
                if(check != 0) {
                    perror("pthread_mutex_lock");
                    exit(EXIT_FAILURE);
                }
               
                expected_seq = msg_node->m->seq + 1;    //update the expected sequence number
                
                check = pthread_mutex_unlock(&exp_mutex);
                if(check != 0) {
                    perror("pthread_mutex_unlock");
                    exit(EXIT_FAILURE);
                }
                
                pthread_cond_broadcast(&exp_cond);
            }
            
            check = pthread_mutex_lock(&rec_mutex);
            if(check != 0) {
                perror("pthread_mutex_lock");
                exit(EXIT_FAILURE);
            }
            
#ifdef verbose 
            //printf("Before delete:");
            //print_queue(rec_queue);
#endif             
            
            delete_node(&rec_queue, msg_node->m);
            
#ifdef verbose 
            //printf("After delete:");
            //print_queue(rec_queue);
#endif            
            
            check = pthread_mutex_unlock(&rec_mutex);
            if(check != 0) {
                perror("pthread_mutex_unlock");
                exit(EXIT_FAILURE);
            }
        }
    }
    
    pthread_exit(NULL);
}

void recv_msg(struct qnode ** send_queue)
{
    /*
     * Receive messages from the client and put them in the receiving queue if they are not retrasmissions
     */
    
    int check, t;
    struct msg m;
    socklen_t addlen = sizeof(cliaddr);
    fd_set rset;
    struct timeval tv; // = {15, 0};
    
    pthread_t * h_tid = (pthread_t *) malloc(N * sizeof(pthread_t));    //msg handler threads
    if(!h_tid) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    
    for(int i = 0; i < N; i++) {
        check = pthread_mutex_init(&acked_mutexes[i], NULL);
        if(check != 0) {
            perror("pthread_mutex_init");
            exit(EXIT_FAILURE);
        }
        
        check = pthread_cond_init(&ack_cond[i], NULL);
        if(check != 0) {
            perror("pthread_mutex_init");
            exit(EXIT_FAILURE);
        }
        
        t = pthread_create(&h_tid[i], NULL, msg_handler, (void *) send_queue);
        if(t != 0) {
            perror("pthread_create");
            exit(EXIT_FAILURE);
        }
    }
    
    FD_ZERO(&rset);
    
    while(!closed) {   //until the connection is not closed
        FD_SET(connsd, &rset);
        tv.tv_sec = 15;
        tv.tv_usec = 0;
                
        check = select(connsd+1, &rset, NULL, NULL, &tv);   //wait 15 seconds for messages, then wake up to check if the connection is still open
        if(check < 0) {
            perror("select");
            exit(EXIT_FAILURE);
        }
        else if(check > 0) {
            if(FD_ISSET(connsd, &rset)) {
                reset_msg(&m);
                check = recvfrom(connsd, (void*) &m, sizeof(struct msg), 0, (struct sockaddr*) &cliaddr, &addlen);
                if(check < 0) {
                    perror("recvfrom");
                    exit(EXIT_FAILURE);
                }
                
                check = pthread_mutex_lock(&rec_mutex);
                if(check != 0) {
                    perror("pthread_mutex_lock");
                    exit(EXIT_FAILURE);
                }

                if(m.seq >= expected_seq) {  //the message is not old
                    check = insert_sorted(&rec_queue, NULL, &m, -1);
                    if(check == 1 && m.ack != 1) {
                        send_ack(connsd, &cliaddr, ++myseq, m.seq);
                    }
                    else if(check == 0 && m.ack != 1){
                        send_ack(connsd, &cliaddr, myseq, m.seq);
                    }
                }
                else {
                    if(m.ack != 1) {
                        send_ack(connsd, &cliaddr, myseq, m.seq);
                    }
                    else if(m.ack == 1) {
                        insert_sorted(&rec_queue, NULL, &m, -1);
                    }
                }

                check = pthread_mutex_unlock(&rec_mutex);
                if(check != 0) {
                    perror("pthread_mutex_unlock");
                    exit(EXIT_FAILURE);
                }
                
                pthread_cond_broadcast(&rec_cond);
            }
        }
    }
    
    return;
}

int main(int argc, char** argv)
{
    int t, listensd, port, check = 0;
    unsigned long cliseq;
    struct sockaddr_in servaddr;
    struct qnode * s_head = NULL;   //send queue head
    pid_t pid;
    struct sigaction act;
    sigset_t set;
    
    if(argc != 2) {
        fprintf(stderr, "Usage: ./server <port>\n");
        exit(EXIT_FAILURE);
    }

    if((listensd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) { 
        perror("Error in socket");
        exit(EXIT_FAILURE);
    }

    memset((void*) &servaddr, 0, sizeof(servaddr));
    memset((void*) &cliaddr, 0, sizeof(cliaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    port = atoi(argv[1]);
    if(port > 0) {
        servaddr.sin_port = htons(port);
    }
    else {
        perror("atoi");
        exit(EXIT_FAILURE);
    }

    if(bind(listensd, (struct sockaddr*) &servaddr, sizeof(servaddr)) < 0) {
        perror("Error in bind");
        exit(EXIT_FAILURE);
    }
    
    sigfillset(&set);
    act.sa_handler = sigint_handler;
    act.sa_mask = set;
    act.sa_flags = 0;
    check = sigaction(SIGINT, &act, NULL);
    if(check == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
    
    while(1) {  
        connsd = accept_connection(listensd, &cliaddr, &cliseq);    //the parent process continues to listen for new connections
        pid = fork();
        if(pid == -1) {
            perror("fork");
            exit(EXIT_FAILURE);
        }
        else if(pid == 0) {         //child process
            close(listensd);        //close the listening socket  
            srand(pthread_self());
            myseq = 1 + rand();     //select a random sequence number
            
            s_tid = (pthread_t *) malloc(N * sizeof(pthread_t));    //sending threads
            if(!s_tid) {
                perror("malloc");
                exit(EXIT_FAILURE);
            }
            
            for(int i = 0; i < N; i++) {
                check = pthread_mutex_init(&mutexes[i], NULL);
                if(check != 0) {
                    perror("pthread_mutex_init");
                    exit(EXIT_FAILURE);
                }
                
                t = pthread_create(&s_tid[i], NULL, send_message, (void *) &s_head);
                if(t != 0) {
                    perror("pthread_create");
                    exit(EXIT_FAILURE);
                }
            }

            complete_handshake(&cliseq, &s_head);
            
            recv_msg(&s_head);

            close(connsd);
            printf("Connection closed with client on port %u\n\n", ntohs((cliaddr).sin_port));
            exit(EXIT_SUCCESS);
        }
        
        close(connsd);
    }

    return 0;
}
