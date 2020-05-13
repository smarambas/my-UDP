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

#ifdef adaptive

#define alfa    0.125
#define beta    0.25

#endif

struct timespec T  = {1, 0};    //timeout

unsigned long myseq;
unsigned long expected_seq = 0;   //next expected sequence number
int sockfd, bsize = BUFF_SIZE, fd, first_open = 1, closed = 0, opened = 0;
struct sockaddr_in servaddr;
struct qnode * rec_queue = NULL;    //receiving queue
struct qnode * send_base = NULL;    //pointer to the first element of the sending window
char * global_buffer;    //buffer used by the handler threads
char new_file[BUFF_SIZE] = {0};    //requested file name
int acked[N] = {0}; //array to report to sending threads that an ack is received
int snd_indexes[N] = {0};   //array of indexes for the sending threads
int rcv_indexes[N] = {0};   //array of indexes for the receiving threads
pthread_mutex_t index_mutex = PTHREAD_MUTEX_INITIALIZER;    //to sync the index and message choice between threads
pthread_mutex_t rec_index_mutex = PTHREAD_MUTEX_INITIALIZER;    //to sync the index choice between handling threads
pthread_mutex_t mutexes[N];     //to sync the sending threads and the receiving thread
pthread_mutex_t rec_mutex = PTHREAD_MUTEX_INITIALIZER;   //to sync the access to the rec_queue
pthread_mutex_t snd_mutex = PTHREAD_MUTEX_INITIALIZER;   //to sync the accesses to snd_queue
pthread_mutex_t wr_mutex = PTHREAD_MUTEX_INITIALIZER;   //to sync writes on the file
pthread_mutex_t close_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t open_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t exp_mutex = PTHREAD_MUTEX_INITIALIZER;  //to sync the updates to expected_seq
pthread_mutex_t first_open_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t order_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t index_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t close_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t open_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t rec_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t sb_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t exp_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t ack_cond[N];

void open_connection(struct qnode ** send_queue)
{
    /*
     * Send a SYN message to the server and wait for the handshake 
    */

    int t, check = 0;
    struct msg m;
    //socklen_t addlen = sizeof(servaddr);
    pthread_t s_tid;
    struct timespec time_to_wait;
    struct timeval now;
          
    printf("Sending SYN message...\n");

    //send SYN message
    reset_msg(&m);
    m.seq = myseq;    //sequence number
    m.syn = 1;
    
    insert_sorted(send_queue, &servaddr, &m, -1);
    send_base = *send_queue;
    
    t = pthread_create(&s_tid, NULL, send_message, (void *) send_queue); //sending thread
    if(t != 0) {
        fprintf(stderr, "Error in pthread_create\n");
        exit(EXIT_FAILURE);
    }
    
    check = pthread_mutex_lock(&open_mutex);    
    if(check != 0) {
        perror("pthread_mutex_lock");
        exit(EXIT_FAILURE);
    }
    
    //wait for the SYN-ACK for 60 seconds
    while(!opened) {
        gettimeofday(&now, NULL);
        time_to_wait.tv_sec = now.tv_sec + 60.0;        //wait 60 seconds
        time_to_wait.tv_nsec = now.tv_usec * 1000UL;
        
        check = pthread_cond_timedwait(&open_cond, &open_mutex, &time_to_wait);
        if(check != 0 && check != ETIMEDOUT) {
            perror("pthread_cond_timedwait");
            exit(EXIT_FAILURE);
        }
        else if(check == ETIMEDOUT) {
            printf("The server is unreachable right now, try again later.\n\n");
            exit(EXIT_FAILURE);
        }
    }
    
    printf("Connection established with server.\n");
#ifdef debug            
            printf("myseq = %u\n\n", myseq);
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
    int i = -1, j, check = 0;
    struct qnode ** snd_queue = (struct qnode **) args;
    struct qnode * node = NULL;
    struct msg m;
    socklen_t addlen;
    struct timespec timeout;
    struct timespec time_to_wait;
    struct timeval now;

    check = pthread_mutex_lock(&index_mutex);
    if(check != 0) {
        fprintf(stderr, "Error in pthread_mutex_lock\n");
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
    
    while(i < 0) {
        check = pthread_cond_wait(&index_cond, &index_mutex);
        if(check != 0) {
            perror("pthread_cond_wait");
            exit(EXIT_FAILURE);
        }
        else {
            for(j = 0; j < N; j++) {
                if(snd_indexes[j] == 0) {
                    i = j;  //thread index
                    snd_indexes[j] = 1;
                    break;
                }
            }
        }
    }
    
#ifdef debug
    printf("I'm thread %u with index %d\n", pthread_self(), i);
#endif
    
    check = pthread_mutex_unlock(&index_mutex);
    if(check != 0) {
        fprintf(stderr, "Error in pthread_mutex_unlock\n");
        exit(EXIT_FAILURE);
    }

    check = pthread_mutex_lock(&snd_mutex);
    if(check != 0) {
        fprintf(stderr, "Error in pthread_mutex_lock\n");
        exit(EXIT_FAILURE);
    }

    node = search_node_to_serve(snd_queue, i);
    
    check = pthread_mutex_unlock(&snd_mutex);
    if(check != 0) {
        fprintf(stderr, "Error in pthread_mutex_unlock\n");
        exit(EXIT_FAILURE);
    }

    addlen = sizeof(servaddr);
    
    srand(time(NULL));
    
    while(node != NULL) { 
#ifdef debug
        printf("Thread %u sending msg #%u\n", (unsigned long) pthread_self(), node->m->seq);
#endif
        timeout = T;
        
        reset_msg(&m);
        memcpy(&m, node->m, sizeof(struct msg));
        
        if(rand_value() > P) {
            check = sendto(sockfd, (void *) &m, sizeof(struct msg), 0, (struct sockaddr *) &servaddr, addlen);
            if(check < 0) {
                fprintf(stderr, "Error in sendto\n");
                perror("sendto");
                exit(EXIT_FAILURE);
            }        
#ifdef debug
            printf("Message sent to server with seq #%u (tx)\n", m.seq);
#endif
        }
        else {
#ifdef debug 
            printf("Message #%u lost\n", m.seq);
#endif            
        }
        
        check = pthread_mutex_lock(&mutexes[i]);
        if(check != 0) {
            fprintf(stderr, "Error in pthread_mutex_lock\n");
            exit(EXIT_FAILURE);
        }
        
        while(!acked[i]) {
            gettimeofday(&now, NULL);
            time_to_wait.tv_sec = now.tv_sec + timeout.tv_sec;
            time_to_wait.tv_nsec = now.tv_usec * 1000UL + timeout.tv_nsec;
            
            check = pthread_cond_timedwait(&ack_cond[i], &mutexes[i], &time_to_wait);
            if(check != 0) {
                if(check == ETIMEDOUT) {
                    if(rand_value() > P) {
                        check = sendto(sockfd, (void *) &m, sizeof(struct msg), 0, (struct sockaddr *) &servaddr, addlen);
                        if(check < 0) {
                            fprintf(stderr, "Error in sendto\n");
                            perror("sendto");
                            exit(EXIT_FAILURE);
                        }
                        
#ifdef debug
                        printf("Message sent to server with seq #%u (rx)\n", m.seq);
#endif
                    }
                    else {
#ifdef debug 
                        printf("Message #%u lost\n", m.seq);
#endif            
                    }
                }   
                else {
                    fprintf(stderr, "Error in pthread_cond_timedwait\n");
                    perror("pthread_cond_timedwait");
                    exit(EXIT_FAILURE);
                }
            }
        }
        
        acked[i] = 0;
        
        check = pthread_mutex_lock(&snd_mutex);
        if(check != 0) {
            fprintf(stderr, "Error in pthread_mutex_lock\n");
            exit(EXIT_FAILURE);
        }
        
        while(send_base->m->seq != node->m->seq) {
            check = pthread_cond_wait(&sb_cond, &snd_mutex);
            if(check != 0) {
                perror("pthread_cond_wait");
                exit(EXIT_FAILURE);
            }
        }
        
        send_base = send_base->next;        
        delete_node(snd_queue, &m);
        node = search_node_to_serve(snd_queue, i);
        
        check = pthread_mutex_unlock(&snd_mutex);
        if(check != 0) {
            fprintf(stderr, "Error in pthread_mutex_unlock\n");
            exit(EXIT_FAILURE);
        }
        
        pthread_cond_broadcast(&sb_cond);
        
        check = pthread_mutex_unlock(&mutexes[i]);
        if(check != 0) {
            fprintf(stderr, "Error in pthread_mutex_unlock\n");
            exit(EXIT_FAILURE);
        }
    }

    check = pthread_mutex_lock(&index_mutex);
    if(check != 0) {
        fprintf(stderr, "Error in pthread_mutex_lock\n");
        exit(EXIT_FAILURE);
    }

    snd_indexes[i] = 0; //release the index

    check = pthread_mutex_unlock(&index_mutex);
    if(check != 0) {
        fprintf(stderr, "Error in pthread_mutex_unlock\n");
        exit(EXIT_FAILURE);
    }
    
    pthread_cond_broadcast(&index_cond);

    pthread_exit(NULL);
}

void * msg_handler(void * args) 
{
    /*
     * Manage the packets in the correct order
     */

    int i, check = 0, blen, residual = 0, rec_base = 0;
    struct qnode ** snd_queue = (struct qnode **) args;
    struct qnode * node = NULL;
    struct qnode * msg_node = NULL;
    
    check = pthread_mutex_lock(&rec_index_mutex);
    if(check != 0) {
        perror("pthread_mutex_lock");
        exit(EXIT_FAILURE);
    }
    
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

    while(!closed) {
        check = pthread_mutex_lock(&rec_mutex);
        if(check != 0) {
            perror("pthread_mutex_lock");
            exit(EXIT_FAILURE);
        }
        
        while(rec_queue == NULL) {
            check = pthread_cond_wait(&rec_cond, &rec_mutex);
            if(check != 0) {
                perror("pthread_cond_wait");
                exit(EXIT_FAILURE);
            }
        }
        
        msg_node = search_node_to_serve(&rec_queue, i);
        
        check = pthread_mutex_unlock(&rec_mutex);
        if(check != 0) {
            perror("pthread_mutex_unlock");
            exit(EXIT_FAILURE);
        }
        
        if(msg_node != NULL) {
#ifdef debug            
            printf("Handler %d got msg #%d\n", i, msg_node->m->seq);
#endif            

            if(msg_node->m->startfile == 1) {   
                rec_base = 1;                
            }
            else {
                rec_base = 0;
            }
            
            if(msg_node->m->ack == 1) {    //ack message
                node = search_node_byseq((*snd_queue), msg_node->m->ack_num);   //search in the sending queue if there is a message with that sequence
                if(node != NULL) {              //if there is a node in the queue with the correct sequence
                    acked[node->index] = 1;     //report to the sending thread that the message is acked
                    
                    pthread_cond_signal(&ack_cond[node->index]);                
#ifdef debug
                    printf("Ack received for message #%u\n", msg_node->m->ack_num);
#endif
                    if(msg_node->m->syn == 1) {      
                        opened = 1;
                        pthread_cond_signal(&open_cond);
                    }
                }
            }
            else {
                if(!rec_base) {
#ifdef debug            
                    printf("Handler %d is not rec_base\n", i);
#endif                    
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
                
#ifdef debug                
                printf("Expected seq = %u\n"
                       "Myseq = %u\n", expected_seq, msg_node->m->seq);
#endif
                                
                if(msg_node->m->cmd_t == 1) {  //LIST
                    if(msg_node->m->data_size != msg_node->m->file_size) {    //if the data of the message is only a part of the file
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
                        
                        if(msg_node->m->endfile == 1) {
                            printf("\nLIST:\n%s\n", global_buffer);
                            memset(global_buffer, 0, bsize+1);
                        }
                    }
                    else {
                        printf("\nLIST:\n%s\n", msg_node->m->data);   //print the list
                    }
                }
                else if(msg_node->m->cmd_t == 2) { //GET
                    if(msg_node->m->ecode == success) {
                        check = pthread_mutex_lock(&first_open_mutex);
                        if(check != 0) {
                            perror("pthread_mutex_lock");
                            exit(EXIT_FAILURE);
                        }
                        
                        if(first_open) {
#ifdef debug
                            printf("new_file = %s\n", new_file);
#endif                            
                            fd = open(new_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                            if(fd == -1) {
                                fprintf(stderr, "Error in open\n");
                                exit(EXIT_FAILURE);
                            }
                            
                            first_open = 0;
                        }
                        
                        check = pthread_mutex_unlock(&first_open_mutex);
                        if(check != 0) {
                            perror("pthread_mutex_unlock");
                            exit(EXIT_FAILURE);
                        }
                        
                        check = pthread_mutex_lock(&wr_mutex);
                        if(check != 0) {
                            perror("pthread_mutex_lock");
                            exit(EXIT_FAILURE);
                        }
                                                
                        check = write(fd, msg_node->m->data, msg_node->m->data_size);
                        if(check == -1) {
                            perror("write");
                            exit(EXIT_FAILURE);
                        }
                        else if(check < msg_node->m->data_size) {
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
                        
                        if(msg_node->m->endfile == 1) {
                            close(fd);
                            printf("\nDownload completed!\n\n");
                        }
                        
                        check = pthread_mutex_unlock(&wr_mutex);
                        if(check != 0) {
                            perror("pthread_mutex_unlock");
                            exit(EXIT_FAILURE);
                        }
                    }
                    else if(msg_node->m->ecode == clierror) {
                        printf("\nError: the file requested doesn't exist. Please, try again.\n\n");
                    }
                    else if(msg_node->m->ecode == serverror) {
                        printf("\nError: the server couldn't send the requested file because a critical error occurred.\n\n");
                        exit(EXIT_FAILURE);
                    }
                }
                else if(msg_node->m->cmd_t == 3) { //answer to PUT
                    if(msg_node->m->ecode == success) {
                        printf("File uploaded correctly!\n\n");
                    }
                    else {
                        printf("Error: the upload failed.\n\n");
                        closed = 1;
                        pthread_cond_signal(&close_cond);
                    }
                }
                else if(msg_node->m->fin == 1) {    //close connection
                    closed = 1;
                    pthread_cond_signal(&close_cond);
                }
                
                check = pthread_mutex_lock(&exp_mutex);
                if(check != 0) {
                    perror("pthread_mutex_lock");
                    exit(EXIT_FAILURE);
                }

                expected_seq = msg_node->m->seq + 1;
                
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
            
#ifdef debug 
            printf("Before delete:");
            print_queue(rec_queue);
#endif            
            
            delete_node(&rec_queue, msg_node->m);
            
#ifdef debug 
            printf("After delete:");
            print_queue(rec_queue);
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
     * Receive messages from the server
     */

    int i, t, check = 0;
    struct qnode ** snd_queue = (struct qnode **) args; 
    struct msg m;
    socklen_t addlen = sizeof(servaddr);
    fd_set rset;
    
    pthread_t * h_tid = (pthread_t *) malloc(N * sizeof(pthread_t));    //msg handler threads
    if(!h_tid) {
        fprintf(stderr, "Error in malloc\n");
        exit(EXIT_FAILURE);
    }

    for(i = 0; i < N; i++) {
        check = pthread_mutex_init(&mutexes[i], NULL);
        if(check != 0) {
            fprintf(stderr, "Error in pthread_mutex_init\n");
            exit(EXIT_FAILURE);
        }
        
        check = pthread_cond_init(&ack_cond[i], NULL);
        if(check != 0) {
            fprintf(stderr, "Error in pthread_mutex_init\n");
            exit(EXIT_FAILURE);
        }
        
        t = pthread_create(&h_tid[i], NULL, msg_handler, (void *) snd_queue);
        if(t != 0) {
            fprintf(stderr, "Error in pthread_create\n");
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
                fprintf(stderr, "Error in recvfrom\n");
                exit(EXIT_FAILURE);
            }

#ifdef debug
            printf("Message received from the server with seq #%u\n", m.seq);
#endif
            check = pthread_mutex_lock(&rec_mutex);
            if(check != 0) {
                fprintf(stderr, "Error in pthread_mutex_lock\n");
                exit(EXIT_FAILURE);
            }
            
            if(m.ack != 1) {
                send_ack(sockfd, &servaddr, ++myseq, m.seq);    //send an ack for the message
            } 
            
            if(m.seq >= expected_seq) {  //the message is not old
                insert_sorted(&rec_queue, NULL, &m, -1);
            }
            
            if(m.ack == 1 && m.syn == 1) {
                send_ack(sockfd, &servaddr, ++myseq, m.seq);    //send an ack for the message
                expected_seq = m.seq;    //the next message should be an ack for a cmd
            }
            
            check = pthread_mutex_unlock(&rec_mutex);
            if(check != 0) {
                fprintf(stderr, "Error in pthread_mutex_unlock\n");
                exit(EXIT_FAILURE);
            }
            
            pthread_cond_broadcast(&rec_cond);
        }
    }

    pthread_exit(NULL);
}

void print_mylist(void) 
{
    int i, check;
    char * buff;
    size_t blen = 0;
    struct dirent ** filelist;
    
    buff = malloc(BUFF_SIZE * sizeof(char));
    if(!buff) {
        //fprintf(stderr, "Error in malloc\n");
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    
    memset(buff, 0, BUFF_SIZE);
    
    check = scandir("./files_client/", &filelist, 0, versionsort);
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
                        fprintf(stderr, "Error in realloc\n");
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
    int fd, i, t, check;
    unsigned long filesize, dim = 0, bsize = PAYLOAD_SIZE;    
    struct msg m;
    char file[BUFF_SIZE] = "files_client/";
    pthread_t * s_tid = (pthread_t *) malloc(N * sizeof(pthread_t));
    
    strcat(file, filename);

    fd = open(file, O_RDONLY);
    if(fd == -1) {
        if(errno == ENOENT) {
            printf("The file to send doesn't exist.\n");
            return;
        }
        else {
            perror("open");
            exit(EXIT_FAILURE);
        }
    }

    filesize = (unsigned long) lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    
    //send also the name of the file
    reset_msg(&m);
    m.startfile = 1;
    myseq += strlen(filename);
    m.seq = myseq;
    m.file_size = filesize + strlen(filename);
    m.cmd_t = 3;
    memcpy(m.data, filename, strlen(filename));
    
    insert_sorted(send_queue, &servaddr, &m, -1);
    
#ifdef debug
        printf("Inserted message with seq #%d in the queue\n", m.seq);
        print_queue(*send_queue);
#endif    
    
    do {
        reset_msg(&m);
        
        if((filesize - dim) <= bsize) {
            bsize = filesize - dim;
            m.endfile = 1;
        }
        else {
            m.endfile = 0;
        }
        
        check = read(fd, m.data, bsize);
        while(check != bsize) {
            printf("Error: read less bytes\n");
            if(check < 0) {
                fprintf(stderr, "Error in read\n");
                exit(EXIT_FAILURE);
            }
            lseek(fd, -check, SEEK_CUR);
            memset(m.data, 0, PAYLOAD_SIZE);
            check = read(fd, m.data, bsize);
        }
                
        myseq += check;
        m.seq = myseq;
        m.startfile = 0;
        m.data_size = (unsigned long) check;
        m.file_size = filesize + strlen(filename);
        m.cmd_t = 3;
        //m.ecode = success;        
                
        insert_sorted(send_queue, &servaddr, &m, -1);
#ifdef debug
        printf("Inserted message with seq #%d in the queue\n", m.seq);
        print_queue(*send_queue);
#endif
        dim += check;
    }
    while(dim < filesize);
    
    send_base = *send_queue;

    for(i = 0; i < N; i++) {
        if(i < queue_size(*send_queue)) {
            t = pthread_create(&s_tid[i], NULL, send_message, (void *) send_queue);
            if(t != 0) {
                fprintf(stderr, "Error in pthread_create\n");
                exit(EXIT_FAILURE);
            }    
        }
    }
    
    return;
}

void send_cmd(struct qnode ** send_queue)
{
    /*
     * Routine used by the client to send commands to the server
     */

    int t, end = 0, check;
    char * cmd;
    int cmd_len;
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

    pthread_t * s_tid = (pthread_t *) malloc(N * sizeof(pthread_t));    //sendig threads
    if(!s_tid) {
        fprintf(stderr, "Error in malloc\n");
        exit(EXIT_FAILURE);
    }
    
    printf("\nInsert one of the following requests to the server:\n"
                "- list\n"
                "- get <filename>\n"
                "- post <filename>\n"
                "- mylist\n"
                "- help\n"
                "- quit\n\n");
    
    while(!end) {
        cmd = read_line();
        cmd_len = sizeof(cmd);

        tokens = split_line(cmd);

        if(strcmp(tokens[0], "list") == 0) {
            reset_msg(&m);
            myseq += 1;
            m.seq = myseq;
            m.data_size = 1;
            m.startfile = 1;
            m.endfile = 1;
            m.cmd_t = 1;

            insert_sorted(send_queue, &servaddr, &m, -1);
            send_base = *send_queue;
            
            t = pthread_create(&s_tid[0], NULL, send_message, (void *) send_queue); //sending thread
            if(t != 0) {
                fprintf(stderr, "Error in pthread_create\n");
                exit(EXIT_FAILURE);
            }
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

            insert_sorted(send_queue, &servaddr, &m, -1);
            send_base = *send_queue;

            t = pthread_create(&s_tid[0], NULL, send_message, (void *) send_queue); //sending thread
            if(t != 0) {
                fprintf(stderr, "Error in pthread_create\n");
                exit(EXIT_FAILURE);
            }
        }
        else if(strcmp(tokens[0], "post") == 0 && tokens[1] != NULL) {
            send_file(send_queue, tokens[1]);
        }
        else if(strcmp(tokens[0], "mylist") == 0) {
            print_mylist();
        }
        else if(strcmp(tokens[0], "help") == 0) {
            printf("\nInsert one of the following requests to the server:\n"
                "- list\n"
                "- get <filename>\n"
                "- post <filename>\n"
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
            
            insert_sorted(send_queue, &servaddr, &m, -1);
            send_base = *send_queue;
            
            t = pthread_create(&s_tid[0], NULL, send_message, (void *) send_queue); //sending thread
            if(t != 0) {
                fprintf(stderr, "Error in pthread_create\n");
                exit(EXIT_FAILURE);
            }
            
            check = pthread_mutex_lock(&close_mutex);
            if(check != 0) {
                fprintf(stderr, "Error in pthread_mutex_lock\n");
                exit(EXIT_FAILURE);
            }
            
            while(!closed) {
                check = pthread_cond_wait(&close_cond, &close_mutex);
                if(check != 0) {
                    fprintf(stderr, "Error in pthread_cond_wait\n");
                    exit(EXIT_FAILURE);
                }
            }
            
            check = pthread_mutex_unlock(&close_mutex);
            if(check != 0) {
                fprintf(stderr, "Error in pthread_mutex_unlock\n");
                exit(EXIT_FAILURE);
            }
            
            //wait 30 seconds before closing the connection
            printf("\nClosing connection...\n");
            time_to_wait.tv_sec = 30.0;
            time_to_wait.tv_nsec = 0;
            nanosleep(&time_to_wait, NULL);
            
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
    int t, servport;
    struct qnode * s_head = NULL;   //send queue
    pthread_t r_tid;  //receiving thread
    
    if(argc != 3) {
        fprintf(stderr, "Usage: client <server IP address> <server port>\n");
        exit(-1);
    }

    if((sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
        fprintf(stderr, "Error in socket\n");
        exit(-1);
    }

    memset((void*) &servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servport = atoi(argv[2]);
    if(servport > 0) {
        servaddr.sin_port = htons(servport);    
    }
    else {
        fprintf(stderr, "Error in atoi\n");
        exit(-1);
    }
    
    if(inet_pton(AF_INET, argv[1], &servaddr.sin_addr) <= 0) {
        fprintf(stderr, "Error in inet_pton for %s\n", argv[1]);
        exit(-1);
    }

    srand(pthread_self());
    myseq = 1 + rand();   //choose a random sequence number
    
    t = pthread_create(&r_tid, NULL, recv_msg, (void *) &s_head);  //receiving thread
    if(t != 0) {
        fprintf(stderr, "Error in pthread_create\n");
        exit(EXIT_FAILURE);
    } 

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
