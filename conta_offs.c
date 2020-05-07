#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define MAXSIZE 510
#define OFFS    28

struct msg {
    char syn;
    char ack;
    char fin;
    char endfile;
    char cmd_t;
    enum codes {success = 1, clierror = 2, serverror = 3} ecode;
    unsigned int seq;
    unsigned int ack_num;
    char data[MAXSIZE+1-OFFS];
    int data_size;
    int file_size;
};

int main()
{
    printf("sizeof(msg) = %lu\n\n", sizeof(struct msg));

    return 0;
}
