#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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
#include <dirent.h>

#include "myUDP.h"
#include "common.h"
#include "client.h"

/* Global Variables */
struct timespec T  = {1, 0};                                    //timeout {seconds, nanoseconds}
struct timespec start_test;
struct timespec end_test;

unsigned long myseq;                                            //sequence number
unsigned long expected_seq = 0;                                 //next expected sequence number
int sockfd, fd, first_open = 1, closed = 0, opened = 0;
unsigned long bsize = BUFF_SIZE;                                //buffer size
struct sockaddr_in servaddr;                                    //server's address
struct qnode * rec_queue = NULL;                                //receiving queue
struct qnode * send_base = NULL;                                //pointer to the first element of the sending window
char * global_buffer;                                           //buffer used by the handler threads
char new_file[BUFF_SIZE] = {0};                                 //requested file name
int acked[N] = {0};                                             //array to report to sending threads that an ack is received
int snd_indexes[N] = {0};                                       //array of indexes for the sending threads
int rcv_indexes[N] = {0};                                       //array of indexes for the receiving threads
pthread_mutex_t index_mutex = PTHREAD_MUTEX_INITIALIZER;        //to sync the index and message choice between threads
pthread_mutex_t rec_index_mutex = PTHREAD_MUTEX_INITIALIZER;    //to sync the index choice between handling threads
pthread_mutex_t mutexes[N];                                     //to sync the sending threads and the receiving thread
pthread_mutex_t acked_mutexes[N];                               //to make the access to acked[i] more robust
pthread_mutex_t rec_mutex = PTHREAD_MUTEX_INITIALIZER;          //to sync the access to the rec_queue
pthread_mutex_t snd_mutex = PTHREAD_MUTEX_INITIALIZER;          //to sync the accesses to snd_queue
pthread_mutex_t wr_mutex = PTHREAD_MUTEX_INITIALIZER;           //to sync writes on the file
pthread_mutex_t close_mutex = PTHREAD_MUTEX_INITIALIZER;    
pthread_mutex_t open_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t exp_mutex = PTHREAD_MUTEX_INITIALIZER;          //to sync the updates to expected_seq
pthread_mutex_t timer_mutex = PTHREAD_MUTEX_INITIALIZER;        //to sync updates to the timeout timer
pthread_cond_t index_cond = PTHREAD_COND_INITIALIZER;           //to signal the release of an index
pthread_cond_t close_cond = PTHREAD_COND_INITIALIZER;           //to signal the closing of the connection
pthread_cond_t open_cond = PTHREAD_COND_INITIALIZER;            //to signal the opening of the connection
pthread_cond_t rec_cond = PTHREAD_COND_INITIALIZER;             //to signal the receiving of messages
pthread_cond_t sb_cond = PTHREAD_COND_INITIALIZER;              //to signal updates to send_base
pthread_cond_t exp_cond = PTHREAD_COND_INITIALIZER;             //to signal updates to expected_seq
pthread_cond_t snd_cond = PTHREAD_COND_INITIALIZER;             //to signal to the sending thread that there is something to send
pthread_cond_t ack_cond[N];                                     //to signal to thread i the arriving of an ack


void open_connection(struct qnode ** send_queue)
{
    /*
     * Send a SYN message to the server and wait for the handshake 
     */

    int check = 0;
    struct msg m;
    struct timespec time_to_wait = {60, 0};
    struct timespec now;
          
    printf("Sending SYN message...\n");

    reset_msg(&m);
    m.seq = myseq;
    m.startfile = 1;
    m.syn = 1;
    
    check = pthread_mutex_lock(&snd_mutex);
    if(check != 0) {
        perror("pthread_mutex_lock");
        exit(EXIT_FAILURE);
    }
    
    insert_sorted(send_queue, &servaddr, &m, -1);

    check = pthread_mutex_unlock(&snd_mutex);
    if(check != 0) {
        perror("pthread_mutex_unlock");
        exit(EXIT_FAILURE);
    }
    
    pthread_cond_broadcast(&snd_cond);
    
    check = pthread_mutex_lock(&open_mutex);    
    if(check != 0) {
        perror("pthread_mutex_lock");
        exit(EXIT_FAILURE);
    }
    
    while(!opened) {    //wait for the SYN-ACK for 60 seconds
        clock_gettime(CLOCK_REALTIME, &now);
        time_to_wait = timespec_add(now, time_to_wait);
        
        check = pthread_cond_timedwait(&open_cond, &open_mutex, &time_to_wait);
        if(check != 0 && check != ETIMEDOUT) {
            perror("pthread_cond_timedwait");
            exit(EXIT_FAILURE);
        }
        else if(check == ETIMEDOUT) {
            printf("\nThe server is unreachable right now, try again later.\n\n");
            exit(EXIT_FAILURE);
        }
    }
    
    printf("Connection established with server.\n");
#ifdef verbose            
    printf("myseq = %lu\n\n", myseq);
#endif       
    
    check = pthread_mutex_unlock(&open_mutex);   
    if(check != 0) {
        perror("pthread_mutex_unlock");
        exit(EXIT_FAILURE);
    }
    
    return;
}

void * send_message(void * args)
{
    /*
     * Try to send a message and handle the ritrasmissions until an ack arrives
     * It can use both a fixed value timer or an adaptive timer
     */
    
    int i = 0, j, check = 0;
    struct qnode ** snd_queue = (struct qnode **) args;
    struct qnode * node = NULL;
    struct msg m;
    socklen_t addlen = sizeof(servaddr);
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
    
    //select the index that will last for the whole execution
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
    
    while(!closed) {    //while the connection is open
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
                check = sendto(sockfd, (void *) &m, sizeof(struct msg), 0, (struct sockaddr *) &servaddr, addlen);
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
                
//                 if(node->m->seq == send_base->m->seq) { 
                clock_gettime(CLOCK_THREAD_CPUTIME_ID, &start_t);   //measure the time between a succesful send and the arrive of an ack
//                 }
                
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
                            check = sendto(sockfd, (void *) &m, sizeof(struct msg), 0, (struct sockaddr *) &servaddr, addlen);
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
                
//                 if(node->m->seq == send_base->m->seq) { //update the timer value only if the node is the snd_base
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
//                 }
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
            
            //select the new send base, delete the node from the sending queue
            send_base = send_base->next;        
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

void * msg_handler(void * args) 
{
    /*
     * Manage the packets in the correct order
     */

    int i = 0, check = 0, blen, residual = 0, rec_base = 0;
    struct qnode ** snd_queue = (struct qnode **) args;
    struct qnode * node = NULL;
    struct qnode * msg_node = NULL;
    
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

    while(!closed) {    //while the connection is open
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
            }
            else {
                rec_base = 0;
            }
            
            if(msg_node->m->ack == 1) { //if the message is an ack 
                node = search_node_by_seq((*snd_queue), msg_node->m->ack_num);   //search in the sending queue to find a message that is waiting for the ack
                if(node != NULL) {  //if there is a node in the sending queue with the correct sequence
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
                    printf("Ack received for message #%lu\n", msg_node->m->ack_num);
#endif
                    if(msg_node->m->syn == 1) { //if the message is a syn-ack, open the connection
                        opened = 1;
                        pthread_cond_signal(&open_cond);
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
                                                
                if(msg_node->m->cmd_t == 1) {  //if the message is an answer to the list command
                    if(msg_node->m->data_size != msg_node->m->file_size) {  //if the data of the message is only a part of the file, save it in a buffer
                        blen = strlen(global_buffer);
                        if(msg_node->m->data_size + blen >= BUFF_SIZE) {
                            bsize += BUFF_SIZE;
                            global_buffer = realloc(global_buffer, bsize);
                            if(!global_buffer) {
                                perror("realloc");
                                exit(EXIT_FAILURE);
                            }
                        }
                    
                        strcat(global_buffer, msg_node->m->data);
                        
                        if(msg_node->m->endfile == 1) { //if the message is the last in the ordered sequence, then print the list
                            printf("\nLIST:\n%s\n", global_buffer);
                            memset(global_buffer, 0, bsize+1);                            
                            clock_gettime(CLOCK_REALTIME, &end_test);
                            printf("Total time elapsed: %.3Lf s\n\n", timespec_to_double(timespec_sub(end_test, start_test)));                            
                        }
                    }
                    else {
                        printf("\nLIST:\n%s\n", msg_node->m->data);                        
                        clock_gettime(CLOCK_REALTIME, &end_test);
                        printf("Total time elapsed: %.3Lf s\n\n", timespec_to_double(timespec_sub(end_test, start_test)));                        
                    }
                }
                else if(msg_node->m->cmd_t == 2) {  //if the message is an answer to the get command 
                    if(msg_node->m->ecode == success) { //if the request was succesful
                        if(first_open  && rec_base) {        
                            fd = open(new_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                            if(fd == -1) {
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
                                                
                        check = write(fd, msg_node->m->data, msg_node->m->data_size);   //write the data on the file
                        if(check == -1) {
                            perror("write");
                            exit(EXIT_FAILURE);
                        }
                        else if(check < (long int) msg_node->m->data_size) {
                            residual = msg_node->m->data_size - check;
                            check = 0;
                            while(check < residual) {
                                check = write(fd, msg_node->m->data, residual);
                                if(check == -1) {
                                    perror("write");
                                    exit(EXIT_FAILURE);
                                }
                                residual -= check;
                            }
                        }
                        
                        if(msg_node->m->endfile == 1) { //if the message is the last in the ordered sequence, close the file
                            close(fd);
                            printf("\nDownload completed!\n\n");                            
                            clock_gettime(CLOCK_REALTIME, &end_test);
                            printf("Total time elapsed: %.3Lf s\n\n", timespec_to_double(timespec_sub(end_test, start_test)));                            
                        }
                        
                        check = pthread_mutex_unlock(&wr_mutex);
                        if(check != 0) {
                            perror("pthread_mutex_unlock");
                            exit(EXIT_FAILURE);
                        }
                    }
                    else if(msg_node->m->ecode == clierror) {   //if the request was not succesful because of an errore client side
                        printf("\nError: the file requested doesn't exist. Please, try again.\n\n");
                    }
                    else if(msg_node->m->ecode == serverror) {  //if the request was not succesful because of an errore server side
                        printf("\nError: the server couldn't send the requested file because a critical error occurred.\n\n");
                        exit(EXIT_FAILURE);
                    }
                }
                else if(msg_node->m->cmd_t == 3) { //if the message is an answer to the post command 
                    if(msg_node->m->ecode == success) {
                        printf("\nFile uploaded correctly!\n\n");                        
                        clock_gettime(CLOCK_REALTIME, &end_test);
                        printf("Total time elapsed: %.3Lf s\n\n", timespec_to_double(timespec_sub(end_test, start_test)));                       
                    }
                    else {
                        printf("\nError: the upload failed.\n\n");  //if the upload fails, we close the connection because almost surely the server crashed
                        closed = 1;
                        pthread_cond_signal(&close_cond);
                    }
                }
                else if(msg_node->m->fin == 1) {    //if it's a fin message, signal the closing of the connection
                    closed = 1;
                    pthread_cond_signal(&close_cond);
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

void * recv_msg(void * args)
{
    /*
     * Receive messages from the server and put them in the receiving queue if they are not retrasmissions
     */

    int i, t, check = 0;
    struct qnode ** snd_queue = (struct qnode **) args; 
    struct msg m;
    socklen_t addlen = sizeof(servaddr);
    fd_set rset;
    
    pthread_t * s_tid = (pthread_t *) malloc(N * sizeof(pthread_t));    //sending threads
    if(!s_tid) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    
    pthread_t * h_tid = (pthread_t *) malloc(N * sizeof(pthread_t));    //msg handler threads
    if(!h_tid) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    
    for(i = 0; i < N; i++) {
        check = pthread_mutex_init(&mutexes[i], NULL);
        if(check != 0) {
            perror("pthread_mutex_init");
            exit(EXIT_FAILURE);
        }
        
        check = pthread_cond_init(&ack_cond[i], NULL);
        if(check != 0) {
            perror("pthread_mutex_init");
            exit(EXIT_FAILURE);
        }
        
        t = pthread_create(&s_tid[i], NULL, send_message, (void *) snd_queue);
        if(t != 0) {
            perror("pthread_create");
            exit(EXIT_FAILURE);
        }
        
        t = pthread_create(&h_tid[i], NULL, msg_handler, (void *) snd_queue);
        if(t != 0) {
            perror("pthread_create");
            exit(EXIT_FAILURE);
        }
    }

    FD_ZERO(&rset);
    
    while(1) {
        FD_SET(sockfd, &rset);
        
        if(select(sockfd+1, &rset, NULL, NULL, NULL) < 0) {
            perror("select");
            exit(EXIT_FAILURE);
        }

        if(FD_ISSET(sockfd, &rset)) {
            reset_msg(&m);
            check = recvfrom(sockfd, (void *) &m, sizeof(struct msg), 0, (struct sockaddr *) &servaddr, &addlen);
            if(check < 0) {
                perror("recvfrom");
                exit(EXIT_FAILURE);
            }
            
            check = pthread_mutex_lock(&rec_mutex);
            if(check != 0) {
                perror("pthread_mutex_lock");
                exit(EXIT_FAILURE);
            }

            if(m.seq >= expected_seq) {  //if the message is not old
                check = insert_sorted(&rec_queue, NULL, &m, -1);    
                if((check == 1 && m.ack != 1) || (check == 1 && m.ack == 1 && m.syn == 1)) {
                    send_ack(sockfd, &servaddr, ++myseq, m.seq);
                }
                else if(check != 1 && m.ack != 1){
                    send_ack(sockfd, &servaddr, myseq, m.seq);
                }
            }
            else {
                if(m.ack != 1) {
                    send_ack(sockfd, &servaddr, myseq, m.seq);
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

    pthread_exit(NULL);
}

void print_mylist(void) 
{
    /*
     * Print the list of the client's files
     */    
    
    int i, check;
    char * buff;
    size_t blen = 0;
    struct dirent ** filelist;
    
    buff = malloc(BUFF_SIZE * sizeof(char));
    if(!buff) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    
    memset(buff, 0, BUFF_SIZE);
    
    check = scandir("./files_client/", &filelist, 0, versionsort);  //sort the list alphabetically
    if(check < 0) {
        perror("scandir");
        exit(EXIT_FAILURE);
    }
    else {
        blen = strlen(buff);
        for(i = 0; i < check; i++) {
            if(filelist[i]->d_name[0] != '.') {
                if((blen + strlen(filelist[i]->d_name)) >= bsize) {
                    bsize += BUFF_SIZE; 
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
    
    printf("\nMY LIST:\n"
           "%s\n", buff);
    
    free(buff);
    
    return;
}

void send_file(struct qnode ** send_queue, char * filename)
{
    /*
     * Open the file, divide it in chunks, insert them in the sending queue and send them
     */
    
    int fd, check, count = 0;
    unsigned long filesize, dim = 0, buff_size = PAYLOAD_SIZE;    
    struct msg m;
    char file[BUFF_SIZE] = "files_client/";
    
    strcat(file, filename);

    fd = open(file, O_RDONLY);
    if(fd == -1) {
        if(errno == ENOENT) {
            printf("\nError: the file to send doesn't exist.\n\n");
            return;
        }
        else {
            perror("open");
            exit(EXIT_FAILURE);
        }
    }

    filesize = (unsigned long) lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    
    //send also the name of the file as first message
    reset_msg(&m);
    m.startfile = 1;
    myseq += strlen(filename);
    m.seq = myseq;
    m.file_size = filesize + strlen(filename);
    m.cmd_t = 3;
    memcpy(m.data, filename, strlen(filename));
    
    check = pthread_mutex_lock(&snd_mutex);
    if(check != 0) {
        perror("pthread_mutex_lock");
        exit(EXIT_FAILURE);
    }
            
    clock_gettime(CLOCK_REALTIME, &start_test);
    insert_sorted(send_queue, &servaddr, &m, -1);
    
    check = pthread_mutex_unlock(&snd_mutex);
    if(check != 0) {
        perror("pthread_mutex_unlock");
        exit(EXIT_FAILURE);
    }
    
    pthread_cond_broadcast(&snd_cond);
    
#ifdef verbose
    printf("Inserted message with seq #%lu in the queue\n", m.seq);
#endif    
    
    do {
        reset_msg(&m);
        
        if((filesize - dim) <= buff_size) { //if the chunk is less than the PAYLOAD_SIZE, then it's the last message of the ordered sequence
            buff_size = filesize - dim;
            m.endfile = 1;
        }
        else {
            m.endfile = 0;
        }
        
        count = read(fd, m.data, buff_size);
        while(count != (long int) buff_size) {
            printf("Error: read less bytes\n");
            if(count < 0) {
                perror("read");
                exit(EXIT_FAILURE);
            }
            lseek(fd, -count, SEEK_CUR);
            memset(m.data, 0, PAYLOAD_SIZE);
            count = read(fd, m.data, buff_size);
        }
                
        myseq += count;
        m.seq = myseq;
        m.startfile = 0;
        m.data_size = (unsigned long) count;
        m.file_size = filesize + strlen(filename);
        m.cmd_t = 3;
        
        check = pthread_mutex_lock(&snd_mutex);
        if(check != 0) {
            perror("pthread_mutex_lock");
            exit(EXIT_FAILURE);
        }
                
        insert_sorted(send_queue, &servaddr, &m, -1);
        
        check = pthread_mutex_unlock(&snd_mutex);
        if(check != 0) {
            perror("pthread_mutex_unlock");
            exit(EXIT_FAILURE);
        }
        
        pthread_cond_broadcast(&snd_cond);
        
        dim += count;
        
#ifdef verbose
        printf("Inserted message with seq #%lu in the sending queue\n", m.seq);
#endif
    }
    while(dim < filesize);
    
    return;
}

void send_cmd(struct qnode ** send_queue)
{
    /*
     * Send commands to the server
     */

    int end = 0, check;
    char * cmd;
    char ** tokens;
    struct msg m;
    struct timespec time_to_wait;
    struct sigaction act;
    sigset_t set;
    
    sigfillset(&set);
    act.sa_handler = sigint_handler;
    act.sa_mask = set;
    act.sa_flags = 0;
    check = sigaction(SIGINT, &act, NULL);
    if(check == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
    
    printf("\nInsert one of the following requests to the server:\n"
                "- list\n"
                "- get <filename>\n"
                "- put <filename>\n"
                "- mylist\n"
                "- help\n"
                "- quit\n\n");
        
    while(!end) {
        cmd = read_line();

        tokens = split_line(cmd);   //split the commands to parse them

        if(strcmp(tokens[0], "list") == 0) {
            reset_msg(&m);
            myseq += 1;
            m.seq = myseq;
            m.data_size = 1;
            m.startfile = 1;
            m.endfile = 1;
            m.cmd_t = 1;
            
            check = pthread_mutex_lock(&snd_mutex);
            if(check != 0) {
                perror("pthread_mutex_lock");
                exit(EXIT_FAILURE);
            }
                    
            clock_gettime(CLOCK_REALTIME, &start_test);
            insert_sorted(send_queue, &servaddr, &m, -1);
            
            check = pthread_mutex_unlock(&snd_mutex);
            if(check != 0) {
                perror("pthread_mutex_unlock");
                exit(EXIT_FAILURE);
            }
            
            pthread_cond_broadcast(&snd_cond);
        }
        else if(strcmp(tokens[0], "get") == 0 && tokens[1] != NULL) {
            reset_msg(&m);
            myseq += strlen(tokens[1]);
            m.seq = myseq;
            m.startfile = 1;
            m.endfile = 1;
            m.cmd_t = 2;
            m.data_size = strlen(tokens[1]);
            m.file_size = m.data_size;
            memcpy(m.data, tokens[1], strlen(tokens[1]));
            
            first_open = 1;
            memset(new_file, 0, BUFF_SIZE);
            strcat(new_file, "files_client/");
            strcat(new_file, tokens[1]);
            
            check = pthread_mutex_lock(&snd_mutex);
            if(check != 0) {
                perror("pthread_mutex_lock");
                exit(EXIT_FAILURE);
            }
                    
            clock_gettime(CLOCK_REALTIME, &start_test);
            insert_sorted(send_queue, &servaddr, &m, -1);
            
            check = pthread_mutex_unlock(&snd_mutex);
            if(check != 0) {
                perror("pthread_mutex_unlock");
                exit(EXIT_FAILURE);
            }
            
            pthread_cond_broadcast(&snd_cond);
        }
        else if(strcmp(tokens[0], "put") == 0 && tokens[1] != NULL) {
            send_file(send_queue, tokens[1]);
        }
        else if(strcmp(tokens[0], "mylist") == 0) {
            print_mylist();
        }
        else if(strcmp(tokens[0], "help") == 0) {
            printf("\nInsert one of the following requests to the server:\n"
                "- list\n"
                "- get <filename>\n"
                "- put <filename>\n"
                "- mylist"
                "- help\n"
                "- quit\n\n");
        }
        else if(strcmp(tokens[0], "quit") == 0) {   //close connection
            reset_msg(&m);
            myseq += 1;
            m.seq = myseq;
            m.startfile = 1;
            m.endfile = 1;
            m.data_size = 1;
            m.fin = 1;
            
            check = pthread_mutex_lock(&snd_mutex);
            if(check != 0) {
                perror("pthread_mutex_lock");
                exit(EXIT_FAILURE);
            }
                    
            insert_sorted(send_queue, &servaddr, &m, -1);
            
            check = pthread_mutex_unlock(&snd_mutex);
            if(check != 0) {
                perror("pthread_mutex_unlock");
                exit(EXIT_FAILURE);
            }
            
            pthread_cond_broadcast(&snd_cond);
            
            check = pthread_mutex_lock(&close_mutex);
            if(check != 0) {
                perror("pthread_mutex_lock");
                exit(EXIT_FAILURE);
            }
            
            while(!closed) {    //wait for the fin message
                check = pthread_cond_wait(&close_cond, &close_mutex);
                if(check != 0) {
                    perror("pthread_cond_wait");
                    exit(EXIT_FAILURE);
                }
            }
            
            check = pthread_mutex_unlock(&close_mutex);
            if(check != 0) {
                perror("pthread_mutex_unlock");
                exit(EXIT_FAILURE);
            }
            
            printf("\nClosing connection...\n");
            time_to_wait.tv_sec = 30.0;
            time_to_wait.tv_nsec = 0;
            nanosleep(&time_to_wait, NULL); //wait 30 seconds before closing the connection
            
            end = 1;
        }
        else {
            printf("\nBad command, please try again.\n\n");
        }
    }

    return;
}

int main(int argc, char** argv) 
{
    int t, servport, check = 0;
    struct qnode * s_head = NULL;   //send queue head
    pthread_t r_tid;                //receiving thread
    
    if(argc != 3) {
        fprintf(stderr, "Usage: client <server IP address> <server port>\n");
        exit(EXIT_FAILURE);
    }

    if((sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    memset((void*) &servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servport = atoi(argv[2]);
    if(servport > 0) {
        servaddr.sin_port = htons(servport);    
    }
    else {
        perror("atoi");
        exit(EXIT_FAILURE);
    }
    
    if(inet_pton(AF_INET, argv[1], &servaddr.sin_addr) <= 0) {
        perror("inet_pton");
        exit(EXIT_FAILURE);
    }

    srand(pthread_self());
    myseq = 1 + rand();   //choose a random sequence number
    
    for(int i = 0; i < N; i++) {
        check = pthread_mutex_init(&acked_mutexes[i], NULL);
        if(check != 0) {
            perror("pthread_mutex_init");
            exit(EXIT_FAILURE);
        }
    }
    
    t = pthread_create(&r_tid, NULL, recv_msg, (void *) &s_head);  
    if(t != 0) {
        perror("pthread_create\n");
        exit(EXIT_FAILURE);
    } 
    
    printf("\nN = %d\nT = %.2Lf s\nP = %.f%%\n\n", N, timespec_to_double(T), P * 100);

    open_connection(&s_head);
    
    global_buffer = malloc(bsize * sizeof(char));
    if(!global_buffer) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    
    global_buffer[0] = '\0';
    
    send_cmd(&s_head);

    return 0;
}
