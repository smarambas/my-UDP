#ifndef MYUDP_H
#define MYUDP_H

#define PAYLOAD_SIZE    512

#define N               10           //window width
#define P               0.05        //probability of losing a packet
#define T               1.0         //fixed timeout

struct msg {
    char syn;
    char ack;
    char fin;
    char startfile;
    char endfile;
    char cmd_t;
    enum codes {success = 1, clierror = 2, serverror = 3} ecode;    //error code
    unsigned long seq;
    unsigned long ack_num;
    unsigned long data_size;
    unsigned long file_size;  
    char data[PAYLOAD_SIZE];
};

#endif //MYUDP_H
