
#include "types.h"
#include "stat.h"
#include "user.h"

#define PGSIZE 4096
#define LOOPLENGTH 15



/***********************    MAIN  ***********************/


int main(int argc, char *argv[])
{  
    int i, j, num;
    int pid = 0;
    char * Fmem[LOOPLENGTH];
    char input[5];

    printf(1,"\n\n***************   myMemTest : START  ***************\n\n"); 

    for (i = 0; i < LOOPLENGTH - 3; i++)
    {
        Fmem[i] = sbrk(PGSIZE);
        *Fmem[i] = i;
    }


    for (i = 100; i < 120; i++)
    {
        *Fmem[(i % 3)] = i;
        printf(1, "*Fmem[%d] was accessed in tick %d\n", i % 3, uptime());
    }

    for (i = LOOPLENGTH - 3; i < LOOPLENGTH; i++)
    {
        printf(1,"\nPress enter to continue \n"); 
        gets(input,5);
        Fmem[i] = sbrk(PGSIZE);
        *Fmem[i] = i;
    }

    printf(1,"\n\n***********   myMemTest : FINISHED sbrking  ***********\n"); 

    printf(1,"\n\n***********   myMemTest : Performing fork()  ***********\n\n"); 

    if((pid = fork()) < 0)
    {
         printf(1,"Fork operation failed\n");
    }
    else if (pid == 0)
    {
        for(j = 0; j < LOOPLENGTH; j++)
        {
            if(Fmem[j])
            {
                printf(1,"\nPress enter to continue \n");
                gets(input,5);
                num = j + 50;
                *Fmem[j] = num;
                printf(1,"Process %d: Fmem[%d] is %d \n", getpid(), j, (int)*(Fmem[j])); 
            } 
        }
        exit();
    }
    else
    {
        wait();
        for(j = 0; j < LOOPLENGTH; j++)
        {
            if(Fmem[j])
            {
                printf(1,"\nPress enter to continue \n");
                gets(input,5);
                printf(1,"Process %d: Fmem[%d] is %d \n", getpid(), j, (int)*(Fmem[j])); 
            } 
        }
        exit();
    }
}