#ifndef MYUDP_H
#define MYUDP_H

#define PAYLOAD_SIZE                512

#define N                           10           //window width
#define P                           0.05        //probability of losing a packet    

#define ALFA                        0.125
#define BETA                        0.25

#define MAX_TIMEOUT_INTERVAL        10.0   

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
