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

unsigned long myseq;
unsigned long expected_seq = 0;  //next expected sequence number
int connsd, filed, closed = 0, opened = 0, end = 0;  
struct sockaddr_in cliaddr;
struct qnode * rec_queue = NULL;    //receiving queue
struct qnode * send_base = NULL;    //pointer to the first element of the sending window
int acked[N] = {0}; //array to report to sending threads that an ack is received
int snd_indexes[N] = {0};   //array of snd_indexes
int rcv_indexes[N] = {0};   //array of indexes for the receiving threads
pthread_mutex_t index_mutex = PTHREAD_MUTEX_INITIALIZER;    //to sync the index choice between threads
pthread_mutex_t rec_index_mutex = PTHREAD_MUTEX_INITIALIZER;    //to sync the index choice between handling threads
pthread_mutex_t mutexes[N];     //to sync the sending threads and the receiving thread
pthread_mutex_t rec_mutex = PTHREAD_MUTEX_INITIALIZER;   //to sync the accesses to rec_queue
pthread_mutex_t snd_mutex = PTHREAD_MUTEX_INITIALIZER;   //to sync the accesses to snd_queue
pthread_mutex_t exp_mutex = PTHREAD_MUTEX_INITIALIZER;  //to sync the updates to expected_seq
pthread_mutex_t close_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t index_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t rec_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t exp_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t sb_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t ack_cond[N];
pthread_t * s_tid;  //array of sending threads

int accept_connection(int listensd, struct sockaddr_in * cliaddr, unsigned long * cliseq)
{
    /*
     * The server wait for a SYN message and create a new socket for the connection
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
            fprintf(stderr, "Error in recvfrom\n");
            exit(EXIT_FAILURE);
        }
        
        if(m.syn == 1) {
            check = 1;
        }
    }

    printf("Received SYN message from %s on port %u\n", inet_ntoa((*cliaddr).sin_addr), ntohs((*cliaddr).sin_port));
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
     * The server send a SYN-ACK to the client and wait for an answer to establish a connection
     */

    int t;
    struct msg m;
    pthread_t s_tid;
    
    //send SYN-ACK message
    reset_msg(&m);
    m.seq = myseq;
    m.syn = 1;
    m.ack = 1;
    m.ack_num = *cliseq;
    
    insert_sorted(send_queue, &cliaddr, &m, -1);
    send_base = *send_queue;
        
    t = pthread_create(&s_tid, NULL, send_message, (void *) send_queue);
    if(t != 0) {
        fprintf(stderr, "Error in pthread_create\n");
        exit(EXIT_FAILURE);
    }
        
    return;
}

void * send_message(void * args)
{
    int i = -1, j, check = 0;
    unsigned long timeout;
    struct qnode ** snd_queue = (struct qnode **) args;
    struct qnode * node = NULL;
    struct msg m;
    socklen_t addlen;
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

    addlen = sizeof(cliaddr);
    
    srand(time(NULL));
    
    while(node != NULL) {
#ifdef debug
        printf("Thread %u sending msg #%u\n", (unsigned long) pthread_self(), node->m->seq);
#endif
        timeout = T;
        
        reset_msg(&m);
        memcpy(&m, node->m, sizeof(struct msg));
        
        if(rand_value() > P) {
            check = sendto(connsd, (void *) &m, sizeof(struct msg), 0, (struct sockaddr *) &cliaddr, addlen);
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
            time_to_wait.tv_sec = now.tv_sec + timeout;
            time_to_wait.tv_nsec = now.tv_usec * 1000UL;
            
            check = pthread_cond_timedwait(&ack_cond[i], &mutexes[i], &time_to_wait);
            if(check != 0) {
                if(check == ETIMEDOUT) {
                    if(rand_value() > P) {
                        check = sendto(connsd, (void *) &m, sizeof(struct msg), 0, (struct sockaddr *) &cliaddr, addlen);
                        if(check < 0) {
                            fprintf(stderr, "Error in sendto\n");
                            perror("sendto");
                            exit(EXIT_FAILURE);
                        }
                        
                        //timeout = 2 * timeout;
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
                    fprintf(stderr, "Error in pthread_cond_timedwait with code: %d\n", check);
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
        
#ifdef debug
        printf("Thread %u: Deleting message with seq #%u\n", pthread_self(), m.seq);
        print_queue(*snd_queue);
#endif 
        
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
    
    if(closed == 1) {
        end = 1;
    }

    pthread_exit(NULL);
}

void send_list(struct qnode ** send_queue)
{
    char * buff;    //the list to send
    unsigned long bsize = PAYLOAD_SIZE; 
    size_t blen = 0;
    struct msg m;
    int t, dim, i, sizetocpy, check, first = 1;
    struct dirent ** filelist;

    buff = malloc(bsize * sizeof(char));
    if(!buff) {
        //fprintf(stderr, "Error in malloc\n");
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    
    memset(buff, 0, bsize);
    
    check = scandir("./files_server/", &filelist, 0, versionsort);
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
    sizetocpy = (int) blen;

    while(sizetocpy > 0) {
        reset_msg(&m);

        if(sizetocpy > PAYLOAD_SIZE) {  
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
        //m.nump = packs;
        memcpy(m.data, buff, dim);
        str_cut(buff, 0, dim);

        insert_sorted(send_queue, &cliaddr, &m, -1);
#ifdef debug
            printf("Inserted message with seq #%d in the queue\n", m.seq);
            print_queue(*send_queue);
#endif
        
        sizetocpy = (int) sizetocpy - dim; 
    }
    
    free(buff);
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

void send_file(struct qnode ** send_queue, char * filename)
{
    int fd, i, t, check, first = 1;
    unsigned long filesize, dim = 0, bsize = PAYLOAD_SIZE;    
    struct msg m;
    char file[BUFF_SIZE] = "files_server/";
    
    strcat(file, filename);

    fd = open(file, O_RDONLY);
    if(fd == -1) {
        if(errno == ENOENT) {   //the file doesn't exist, send an error code
            reset_msg(&m);
            
            myseq += 1;
            m.seq = myseq;
            m.cmd_t = 2;
            m.data_size = 1;
            m.startfile = 1;
            m.ecode = clierror;
            
            insert_sorted(send_queue, &cliaddr, &m, -1);
            send_base = *send_queue;
            
            t = pthread_create(&s_tid[0], NULL, send_message, (void *) send_queue);
            if(t != 0) {
                fprintf(stderr, "Error in pthread_create\n");
                exit(EXIT_FAILURE);
            }
            return;
        }
        else {
            reset_msg(&m);
            
            myseq += 1;
            m.seq = myseq;
            m.cmd_t = 2;
            m.data_size = 1;
            m.startfile = 1;
            m.ecode = serverror;
            
            insert_sorted(send_queue, &cliaddr, &m, -1);
            send_base = *send_queue;
            
            t = pthread_create(&s_tid[0], NULL, send_message, (void *) send_queue);
            if(t != 0) {
                fprintf(stderr, "Error in pthread_create\n");
                exit(EXIT_FAILURE);
            }
            fprintf(stderr, "Error in open\n");
            exit(EXIT_FAILURE);
        }
    }

    filesize = (unsigned long) lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    
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
        m.data_size = check;
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
                
        insert_sorted(send_queue, &cliaddr, &m, -1);
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
    
    memset(file, 0, BUFF_SIZE);
    strcat(file, "server_files/");

    return;
}

void * msg_handler(void * args) 
{
    int i, t, check, rec_base = 0;
    struct qnode ** snd_queue = (struct qnode **) args;
    struct qnode * node = NULL;
    struct qnode * msg_node = NULL;
    struct msg m;
    
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
    
    while(1) {
        check = pthread_mutex_lock(&rec_mutex);
        if(check != 0) {
            fprintf(stderr, "Error in pthread_mutex_lock\n");
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
            if(msg_node->m->startfile == 1) {   // || (expected_seq - 1 + msg_node->m->data_size) == msg_node->m->seq) {
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
                    if(!opened) {
                        opened = 1;
                        printf("Connection established with:\n"
                        "Client address\t%s\n"
                        "Client port\t%u\n\n", inet_ntoa((cliaddr).sin_addr), ntohs((cliaddr).sin_port));
#ifdef debug 
                        printf("myseq = %u\n\n", myseq);
#endif        
                    }
                }
            }
            else {
                if(!rec_base) {
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
                    
                    //rec_base = 1;
                }
                
                if(msg_node->m->cmd_t == 1) {  //answer to LIST
                    send_list(snd_queue);
                }
                else if(msg_node->m->cmd_t == 2) {  //answer to GET
                    send_file(snd_queue, msg_node->m->data);
                }
                else if(msg_node->m->cmd_t == 3) {  //answer to PUT
                    //recv_file
                }
                else if(msg_node->m->fin == 1) {    //close connection
                    reset_msg(&m);
                    myseq += 1;
                    m.seq = myseq;
                    m.data_size = 1;
                    m.startfile = 1;
                    m.endfile = 1;
                    m.fin = 1;
                    
                    insert_sorted(snd_queue, &cliaddr, &m, -1);
                    send_base = *snd_queue;
                    closed = 1;
                    
                    t = pthread_create(&s_tid[0], NULL, send_message, (void *) snd_queue); //sending thread
                    if(t != 0) {
                        fprintf(stderr, "Error in pthread_create\n");
                        exit(EXIT_FAILURE);
                    }
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
            
            delete_node(&rec_queue, msg_node->m);
            
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
    int check, t;
    struct msg m;
    socklen_t addlen = sizeof(cliaddr);
    fd_set rset;
    struct timeval tv = {10, 0};
    
    pthread_t * h_tid = (pthread_t *) malloc(N * sizeof(pthread_t));    //msg handler threads
    if(!h_tid) {
        fprintf(stderr, "Error in malloc\n");
        exit(EXIT_FAILURE);
    }
    
    for(int i = 0; i < N; i++) {
        check = pthread_cond_init(&ack_cond[i], NULL);
        if(check != 0) {
            fprintf(stderr, "Error in pthread_mutex_init\n");
            exit(EXIT_FAILURE);
        }
        
        t = pthread_create(&h_tid[i], NULL, msg_handler, (void *) send_queue);
        if(t != 0) {
            fprintf(stderr, "Error in pthread_create\n");
            exit(EXIT_FAILURE);
        }
    }
    
    FD_ZERO(&rset);
    
    while(!end) {
        FD_SET(connsd, &rset);
        
        //check = select(connsd+1, &rset, NULL, NULL, NULL);
        check = select(connsd+1, &rset, NULL, NULL, &tv);
        if(check < 0) {
            perror("select");
            exit(EXIT_FAILURE);
        }
        else if(check > 0) {
            if(FD_ISSET(connsd, &rset)) {
                reset_msg(&m);
                check = recvfrom(connsd, (void*) &m, sizeof(struct msg), 0, (struct sockaddr*) &cliaddr, &addlen);
                if(check < 0) {
                    fprintf(stderr, "Error in recvfrom\n");
                    exit(EXIT_FAILURE);
                }
                
#ifdef debug
                printf("Received message from the client with seq #%d\n", m.seq);
#endif
                
                check = pthread_mutex_lock(&rec_mutex);
                if(check != 0) {
                    fprintf(stderr, "Error in pthread_mutex_lock\n");
                    exit(EXIT_FAILURE);
                }
                
                if(m.ack != 1) {
                    send_ack(connsd, &cliaddr, ++myseq, m.seq);    //send an ack for the message
                }
                
                if(m.seq >= expected_seq) {  //the message is not old
                    insert_sorted(&rec_queue, NULL, &m, -1);
                }

                check = pthread_mutex_unlock(&rec_mutex);
                if(check != 0) {
                    fprintf(stderr, "Error in pthread_mutex_unlock\n");
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
    int listensd, port, check = 0;
    unsigned long cliseq;
    struct sockaddr_in servaddr;
    struct qnode * s_head = NULL;   //send queue
    pid_t pid;
    
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
        fprintf(stderr, "Error in atoi\n");
        exit(EXIT_FAILURE);
    }

    if(bind(listensd, (struct sockaddr*) &servaddr, sizeof(servaddr)) < 0) {
        perror("Error in bind");
        exit(EXIT_FAILURE);
    }
    
    while(1) {
        connsd = accept_connection(listensd, &cliaddr, &cliseq);
        pid = fork();
        if(pid == -1) {
            fprintf(stderr, "Error in fork\n");
            exit(EXIT_FAILURE);
        }
        else if(pid == 0) {     //child process
            close(listensd);    //close the listening socket  
            srand(pthread_self());  // + ntohs((cliaddr).sin_port));
            myseq = 1 + rand();
            
            for(int i = 0; i < N; i++) {
                check = pthread_mutex_init(&mutexes[i], NULL);
                if(check != 0) {
                    fprintf(stderr, "Error in pthread_mutex_init\n");
                    exit(EXIT_FAILURE);
                }
            }

            s_tid = (pthread_t *) malloc(N * sizeof(pthread_t));    //sending threads
            if(!s_tid) {
                fprintf(stderr, "Error in malloc\n");
                exit(EXIT_FAILURE);
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
