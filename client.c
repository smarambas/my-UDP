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

#include "myUDP.h"
#include "common.h"
#include "client.h"

unsigned int myseq;
unsigned int last_in_order;    //last packet in the correct order
int sockfd;
struct sockaddr_in servaddr;
int acked[N] = {0}; //array to report to sending threads that an ack is received
int indexes[N] = {0};   //array of indexes
pthread_mutex_t index_mutex = PTHREAD_MUTEX_INITIALIZER;    //to sync the index and message choice between threads
pthread_mutex_t mutexes[N];     //to sync the sending threads and the receiving thread
pthread_mutex_t rec_mutex = PTHREAD_MUTEX_INITIALIZER;   //to sync the access to the rec_queue
pthread_mutex_t order_mutex = PTHREAD_MUTEX_INITIALIZER; //to sync the ordered handling of the messages
pthread_mutex_t snd_mutex = PTHREAD_MUTEX_INITIALIZER;   //to sync the accesses to snd_queue
pthread_mutex_t last_mutex = PTHREAD_MUTEX_INITIALIZER; //to sync the access to last_in_order
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t index_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t ack_cond[N];
struct qnode * rec_queue = NULL;    //receiving queue
char * global_buffer;    //buffer used by the handler threads
int bsize = BUFF_SIZE;   //current size of the buffer
char req_file[BUFF_SIZE];    //requested file name

int open_connection()
{
    /*
     * Send a SYN message to the server and wait for the handshake 
    */

    int check = 0;
    struct msg m;
    socklen_t addlen = sizeof(servaddr);
        
    printf("Sending SYN message...\n");

    while(check == 0) {
        reset_msg(&m);
        m.seq = myseq;    //sequence number
        m.syn = 1;
#ifdef debug
        printf("Address\t%s\n"
               "Port\t%u\n\n", inet_ntoa(servaddr.sin_addr), ntohs(servaddr.sin_port));
#endif
        //send SYN message
        if((sendto(sockfd, (void*) &m, sizeof(struct msg) /*MAXSIZE*/, 0, (struct sockaddr *) &servaddr, addlen)) < 0) {
            fprintf(stderr, "Error in sendto\n");
            return -1;
        }
        
        reset_msg(&m);
        
        //receive SYN-ACK
        if((recvfrom(sockfd, (void*) &m, sizeof(struct msg) /*MAXSIZE*/, 0, (struct sockaddr *) &servaddr, &addlen)) < 0) {
            fprintf(stderr, "Error in recvfrom\n");
            return -1;
        }
        if(m.syn == 1 && is_ack(&m, myseq)) {
            last_in_order = m.seq;
            reset_msg(&m);
            send_ack(sockfd, &servaddr, ++myseq, last_in_order);      
            printf("Connection established with server.\n");
            printf("myseq = %u\n\n", myseq);
            check = 1;
        } 
    }
    
    return 0;
}

void * send_message(void * args)
{
    int i = -1, j, check = 0;//, acked = 0;
    struct qnode ** snd_queue = (struct qnode **) args;
    struct qnode * node = NULL;
    struct msg m;
    socklen_t addlen;
    //struct timespec start, current, first;
    struct timespec time_to_wait;
    struct timeval now;
    //double duration;

    check = pthread_mutex_lock(&index_mutex);
    if(check != 0) {
        fprintf(stderr, "Error in pthread_mutex_lock\n");
        exit(EXIT_FAILURE);
    }

    for(j = 0; j < N; j++) {
        if(indexes[j] == 0) {
            i = j;  //thread index
            indexes[j] = 1;
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
                if(indexes[j] == 0) {
                    i = j;  //thread index
                    indexes[j] = 1;
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

    node = search_node_to_send(snd_queue, i);
    
    check = pthread_mutex_unlock(&snd_mutex);
    if(check != 0) {
        fprintf(stderr, "Error in pthread_mutex_unlock\n");
        exit(EXIT_FAILURE);
    }

    addlen = sizeof(servaddr);
    
    while(node != NULL) { 
#ifdef debug
        printf("Thread %u sending msg #%u\n", (unsigned int) pthread_self(), node->m->seq);
#endif
        reset_msg(&m);
        memcpy(&m, node->m, sizeof(struct msg));

        check = sendto(sockfd, (void *) &m, sizeof(struct msg), 0, (struct sockaddr *) &servaddr, addlen);
        if(check < 0) {
            fprintf(stderr, "Error in sendto\n");
            perror("sendto");
            exit(EXIT_FAILURE);
        }
        /*
        clock_gettime(CLOCK_MONOTONIC, &start); //the timer start
        memcpy(&first, &start, sizeof(struct timespec));
        */
        
#ifdef debug
        printf("Message sent to server with seq #%u (tx)\n", m.seq);
#endif
        
        check = pthread_mutex_lock(&mutexes[i]);
        if(check != 0) {
            fprintf(stderr, "Error in pthread_mutex_lock\n");
            exit(EXIT_FAILURE);
        }
        
        while(!acked[i]) {
            gettimeofday(&now, NULL);
            time_to_wait.tv_sec = now.tv_sec + T;
            time_to_wait.tv_nsec = now.tv_usec * 1000UL;
            
            check = pthread_cond_timedwait(&ack_cond[i], &mutexes[i], &time_to_wait);
            if(check != 0) {
                if(check == ETIMEDOUT) {
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
        
        delete_node(snd_queue, &m);
#ifdef debug
        printf("Thread %u: Deleting message with seq #%u\n", pthread_self(), m.seq);
        print_queue(*snd_queue);
#endif         
        node = search_node_to_send(snd_queue, i);
        
        check = pthread_mutex_unlock(&snd_mutex);
        if(check != 0) {
            fprintf(stderr, "Error in pthread_mutex_unlock\n");
            exit(EXIT_FAILURE);
        }
        
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

    indexes[i] = 0; //release the index

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

    int check = 0, blen, fd, writing;
    struct qnode ** snd_queue = (struct qnode **) args;
    struct qnode * node = NULL;
    struct qnode * msg_node = NULL;
    char new_file [BUFF_SIZE] = "new_";
    struct timespec time_to_wait;
    struct timeval now;

    check = pthread_mutex_lock(&rec_mutex);
    if(check != 0) {
        perror("pthread_mutex_lock");
        exit(EXIT_FAILURE);
    }

#ifdef debug 
    print_queue(rec_queue);
#endif  
    
    msg_node = pop_first(&rec_queue);
    
#ifdef debug 
    print_queue(rec_queue);
#endif    
    
    check = pthread_mutex_unlock(&rec_mutex);
    if(check != 0) {
        perror("pthread_mutex_unlock");
        exit(EXIT_FAILURE);
    }

    if(msg_node != NULL) {
        if(msg_node->m->ack == 1) {    //ack message
            node = search_node_byseq((*snd_queue), msg_node->m->ack_num);   //search in the sending queue if there is a message with that sequence
            if(node != NULL) {              //if there is a node in the queue with the correct sequence
                check = pthread_mutex_lock(&last_mutex);
                if(check != 0) {
                    perror("pthread_mutex_lock");
                    exit(EXIT_FAILURE);
                }
#ifdef debug                
                printf("last_in_order = %u\nmsg_node->m->seq = %u\n", last_in_order, msg_node->m->seq);
#endif                
                if(last_in_order < msg_node->m->seq) {
                    last_in_order = msg_node->m->seq;
                }
                
                check = pthread_mutex_unlock(&last_mutex);    //release the lock
                if(check != 0) {
                    perror("pthread_mutex_unlock");
                    exit(EXIT_FAILURE);
                }
                
                acked[node->index] = 1;     //report to the sending thread that the message is acked
                
                pthread_cond_signal(&ack_cond[node->index]);
                
#ifdef debug
                printf("Ack received for message #%u\n", msg_node->m->ack_num);
#endif
            } 
        }
        else {
            send_ack(sockfd, &servaddr, ++myseq, msg_node->m->seq);    //send an ack for the message
            
            if(last_in_order < msg_node->m->seq) { //if the message is not a retrasmission
                if(last_in_order + msg_node->m->data_size == msg_node->m->seq) {    //if the message is the one expected
                    if(msg_node->m->cmd_t == 1) {  //answer to LIST
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
                                memset(global_buffer, 0, bsize);
                            }
                        }
                        else {
                            printf("\nLIST:\n%s\n", msg_node->m->data);   //print the list
                        }
                    }
                    else if(msg_node->m->cmd_t == 2) { //answer to GET
                        if(msg_node->m->ecode == success) {
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
                                writing = 1;
                                strcat(new_file, req_file);
                                fd = open(new_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                                if(fd == -1) {
                                    fprintf(stderr, "Error in open\n");
                                    exit(EXIT_FAILURE);
                                }
                                
                                blen = strlen(global_buffer);
                                
                                while(writing) {
                                    check = write(fd, global_buffer, blen);
                                    if(check == blen) {
                                        close(fd);
                                        printf("\nDownload finished.\n\n");
                                        memset(new_file, 0, BUFF_SIZE);
                                        strcat(new_file, "new_");
                                        writing = 0;
                                    }
                                    else if(check != blen && check != -1) {
                                        close(fd);
                                        fd = open(new_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                                        if(fd == -1) {
                                            fprintf(stderr, "Error in open\n");
                                            exit(EXIT_FAILURE);
                                        }
                                    }
                                    else if(check == -1){
                                        perror("write");
                                        exit(EXIT_FAILURE);
                                    }
                                }
                            
                                memset(global_buffer, 0, bsize);
                            }                        
                        }
                        else if(msg_node->m->ecode == clierror) {
                            printf("\nError: the file requested doesn't exist. Please, try again.\n\n");
                        }
                        else if(msg_node->m->ecode == serverror) {
                            printf("\nError: the server couldn't send the requested file because a critical error occurred.\n\n");
                        }
                    }
                    else if(msg_node->m->cmd_t == 3) { //answer to PUT
                        //
                    }
                    
                    check = pthread_mutex_lock(&last_mutex);
                    if(check != 0) {
                        perror("pthread_mutex_lock");
                        exit(EXIT_FAILURE);
                    }
                    
                    last_in_order = msg_node->m->seq;   //update last in order
                    
                    check = pthread_mutex_unlock(&last_mutex);    //release the lock
                    if(check != 0) {
                        perror("pthread_mutex_unlock");
                        exit(EXIT_FAILURE);
                    }

                    //pthread_cond_signal(&cond);
                    pthread_cond_broadcast(&cond);
                }
                else {  //the message is not in the correct order
                    check = pthread_mutex_lock(&order_mutex);    //acquire the lock
                    if(check != 0) {
                        perror("pthread_mutex_lock");
                        exit(EXIT_FAILURE);
                    }
                    
                    while(last_in_order + msg_node->m->data_size != msg_node->m->seq) {
                        gettimeofday(&now, NULL);
                        time_to_wait.tv_sec = now.tv_sec + T;
                        time_to_wait.tv_nsec = now.tv_usec * 1000UL;
                        
                        check = pthread_cond_timedwait(&cond, &order_mutex, &time_to_wait); //wait for the condition to become true
                        if(check != 0 && check != ETIMEDOUT) {
                            perror("pthread_cond_timedwait");
                            exit(EXIT_FAILURE);
                        }
                    }
                    
                    if(msg_node->m->cmd_t == 1) {  //answer to LIST
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
                                memset(global_buffer, 0, bsize);
                            }
                        }
                        else {
                            printf("\nLIST:\n%s\n", msg_node->m->data);   //print the list
                        }
                    }
                    else if(msg_node->m->cmd_t == 2) { //answer to GET
                        if(msg_node->m->ecode == success) {
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
                                writing = 1;
                                strcat(new_file, req_file);
                                fd = open(new_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                                if(fd == -1) {
                                    fprintf(stderr, "Error in open\n");
                                    exit(EXIT_FAILURE);
                                }
                                
                                blen = strlen(global_buffer);
                                
                                while(writing) {
                                    check = write(fd, global_buffer, blen);
                                    if(check == blen) {
                                        close(fd);
                                        printf("\nDownload finished.\n\n");
                                        memset(new_file, 0, BUFF_SIZE);
                                        strcat(new_file, "new_");
                                        writing = 0;
                                    }
                                    else if(check != blen && check != -1) {
                                        close(fd);
                                        fd = open(new_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                                        if(fd == -1) {
                                            fprintf(stderr, "Error in open\n");
                                            exit(EXIT_FAILURE);
                                        }
                                    }
                                    else if(check == -1){
                                        perror("write");
                                        exit(EXIT_FAILURE);
                                    }
                                }
                            
                                memset(global_buffer, 0, bsize);
                            }                        
                        }
                        else if(msg_node->m->ecode == clierror) {
                            printf("\nError: the file requested doesn't exist. Please, try again.\n\n");
                        }
                        else if(msg_node->m->ecode == serverror) {
                            printf("\nError: the server couldn't send the requested file because a critical error occurred.\n\n");
                        }
                    }
                    else if(msg_node->m->cmd_t == 3) { //answer to PUT
                        //
                    }
                    
                    check = pthread_mutex_lock(&last_mutex);
                    if(check != 0) {
                        perror("pthread_mutex_lock");
                        exit(EXIT_FAILURE);
                    }
                    
                    last_in_order = msg_node->m->seq;   //update last in order
                    
                    check = pthread_mutex_unlock(&last_mutex);    //release the lock
                    if(check != 0) {
                        perror("pthread_mutex_unlock");
                        exit(EXIT_FAILURE);
                    }
                    
                    check = pthread_mutex_unlock(&order_mutex);    //release the lock
                    if(check != 0) {
                        perror("pthread_mutex_unlock");
                        exit(EXIT_FAILURE);
                    }
                    
                    //pthread_cond_signal(&cond);
                    pthread_cond_broadcast(&cond);
                }
            }
        }
        
        free(msg_node);     //delete the node from the queue
    }
    
    pthread_exit(NULL);
}

void * recv_answer(void * args)
{
    /*
     * Receive messages from the server
     */

    int i, t, check = 0;
    struct qnode ** snd_queue = (struct qnode **) args; 
    struct msg m;
    socklen_t addlen = sizeof(servaddr);
    pthread_t h_tid;
    fd_set rset;

    for(i = 0; i < N; i++) {
        check = pthread_mutex_init(&mutexes[i], NULL);
        if(check != 0) {
            fprintf(stderr, "Error in pthread_mutex_init\n");
            exit(EXIT_FAILURE);
        }
    }
    
    for(i = 0; i < N; i++) {
        check = pthread_cond_init(&ack_cond[i], NULL);
        if(check != 0) {
            fprintf(stderr, "Error in pthread_mutex_init\n");
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

            insert_sorted(&rec_queue, NULL, &m, 0);            

            check = pthread_mutex_unlock(&rec_mutex);
            if(check != 0) {
                fprintf(stderr, "Error in pthread_mutex_unlock\n");
                exit(EXIT_FAILURE);
            }

            t = pthread_create(&h_tid, NULL, msg_handler, (void *) snd_queue); //sending thread
            if(t != 0) {
                fprintf(stderr, "Error in pthread_create\n");
                exit(EXIT_FAILURE);
            }
        }
    }

    pthread_exit(NULL);
}

int send_cmd(struct qnode ** send_queue)
{
    /*
     * Routine used by the client to send commands to the server and receive the answers
     */

    int t;
    char * cmd;
    int cmd_len;
    char ** tokens;
    struct msg m;

    pthread_t * s_tid = (pthread_t *) malloc(N * sizeof(pthread_t));    //sendig threads
    if(!s_tid) {
        fprintf(stderr, "Error in malloc\n");
        exit(EXIT_FAILURE);
    }

    //printf("$");
    //fflush(stdout);
    while(1) {
        cmd = read_line();
        cmd_len = sizeof(cmd);

        tokens = split_line(cmd);

        if(strcmp(tokens[0], "list") == 0) {
            reset_msg(&m);
            myseq += 1;
            m.seq = myseq;
            m.data_size = 1;
            m.cmd_t = 1;

            insert_sorted(send_queue, &servaddr, &m, -1);        
            
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
            m.cmd_t = 2;
            m.data_size = strlen(tokens[1]);
            m.file_size = m.data_size;
            memcpy(m.data, tokens[1], strlen(tokens[1]));

            insert_sorted(send_queue, &servaddr, &m, -1);

            t = pthread_create(&s_tid[0], NULL, send_message, (void *) send_queue); //sending thread
            if(t != 0) {
                fprintf(stderr, "Error in pthread_create\n");
                exit(EXIT_FAILURE);
            }

            memset(req_file, 0, BUFF_SIZE);
            strcpy(req_file, tokens[1]);
        }
        else if(strcmp(tokens[0], "post") == 0 && tokens[1] != NULL) {
            //*cmd_type = 3;
            // manca la parte relativa al file da inviare
        }
        else if(strcmp(tokens[0], "help") == 0) {
            printf("\nInsert one of the following request for the server:\n"
                "1) list (for a list of the files)\n"
                "2) get <filename> (to download the expressed file)\n"
                "3) post <filename> (to upload the expressed file)\n"
                "4) help\n"
                "5) quit (to close the connection and the program)\n\n");
        }
        else if(strcmp(tokens[0], "quit") == 0) {
            //close connection
            exit(EXIT_SUCCESS);
        }
        else {
            printf("\nBad command, please try again.\n");
        }
    }

    return 0;
}

int main(int argc, char** argv) 
{
    int t, servport, check = 0;
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

    for(int i = 0; i < N; i++) {
        check = pthread_mutex_init(&mutexes[i], NULL);
        if(check != 0) {
            fprintf(stderr, "Error in pthread_mutex_init\n");
            exit(EXIT_FAILURE);
        }
    }

    srand(pthread_self());
    myseq = 1 + rand();   //choose a random sequence number

    open_connection();
    
    global_buffer = malloc(bsize * sizeof(char));
    if(!global_buffer) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    t = pthread_create(&r_tid, NULL, recv_answer, (void *) &s_head);  //receiving thread
    if(t != 0) {
        fprintf(stderr, "Error in pthread_create\n");
        exit(EXIT_FAILURE);
    } 
    
    send_cmd(&s_head);

    return 0;
}
