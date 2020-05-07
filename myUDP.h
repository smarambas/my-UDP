#ifndef MYUDP_H
#define MYUDP_H

#define MAXSIZE 510
#define OFFS    28
#define N       5       //window width
#define P       0.1     //probability of losing a packet
#define T       1.0     //timeout duration

struct msg {
    char syn;
    char ack;
    char fin;
    char endfile;
    char cmd_t;
    enum codes {success = 1, clierror = 2, serverror = 3} ecode;    //error code
    unsigned int seq;
    unsigned int ack_num;
    char data[MAXSIZE+1-OFFS];
    unsigned int data_size;
    unsigned int file_size;  
};

#endif //MYUDP_H
