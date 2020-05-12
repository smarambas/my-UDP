#ifndef MYUDP_H
#define MYUDP_H

#define PAYLOAD_SIZE    512
#define N               10           //window width
#define P               0.2        //probability of losing a packet
#define T               1.0         //timeout duration

struct msg {
    char syn;
    char ack;
    char fin;
    char startfile;
    char endfile;
    char cmd_t;
    enum codes {success = 1, clierror = 2, serverror = 3} ecode;    //error code
    unsigned int seq;
    unsigned int ack_num;
    unsigned int data_size;
    unsigned int file_size;  
    char data[PAYLOAD_SIZE];
};

#endif //MYUDP_H
