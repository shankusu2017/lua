int tmp_var;

#include "conversion.h"

void waitEnter(void)
{
    int c = getchar();
}

//等待外壳函数查看调试中的寄存器的值
void waitLookReg(void)
{
    return;
}

void conver2(void);

void conver(void)
{
    int ret = 0;
    unsigned char a1 = 0;
    char a2 = 15;

    ret = a1 - a2;
    unsigned char a3 = ret;
    printf("ret:%d", ret);
    unsigned char a5 = 300;
    char a6 = a5;
    a6 = (float)130;
    unsigned char a7 = 300.0;
    unsigned char a7777 = -100.0;
    char a77 = -200.0;
    char a777 = 200.0;
    unsigned char a8 = -3000;  // 256+256-300
    unsigned char a88 = -20;  // 256-20
    unsigned char a888 = 0;   // 256+0
    unsigned char a8888 = 50; // (256+50)%256
    unsigned char a88888 = 200;// 200%256
    unsigned char a888888 = 300;

    char a9 = -3000;
    char a91 = -200;
    char a92 = -20;
    char a93 = 0;
    char a94 = 50;
    char a95 = 200;
    char a96 = 300;
    int d = -200;
    unsigned char j = 0;
    char a100 = d;

    float ff = d + j;

    union ali {
        unsigned i;
        unsigned char c[sizeof(unsigned)/sizeof(char)];
    }lr;

    struct zeroS{
        //int a;
        char b[0];
    };

    printf("zeroS.size:%d\n", sizeof(struct zeroS));

    lr.i = 1;

    conver2();

    //void *p = __null;
    //printf("fun run over\n");
    long int li = RAND_MAX;
}

//没有特别说明，以下描述针对标准C而言
void conver2(void)
{
    unsigned int a = 1;
    char b = -100;
    float ret = a + b;
    float ret2 = b;
    char d = 300;
    d = 400;
    float e = d;

    int g =-1;
    a = g;
    a = 0xffffffff;
    g = a;

    {//无符号和无符号，取高阶的无符号
       unsigned int ui = 1;
       unsigned char uc = 2;
       double d = ui + uc;
       waitLookReg();
    }

    {//有符号和有符号，取高阶的有符号
        int i = 0x7effffff;
        char c = 3;
        double d = i + c;
        waitLookReg();
    }

    {//无符号和相同阶或低阶的有符号，则转为无符号版本
        unsigned int ui = 0;
        signed char sc = -1;
        double d = ui + sc;
        waitLookReg();  //4294967295
    }


    {//无符号和高阶的有符号类型，且高阶的有符号类型能容纳全部的无符号，则双方转换为高阶的有符号-标准C
        unsigned char uc = 0;
        int k = -1;
        double db = k + uc;
        waitLookReg();  // -1

        //无符号和高阶的有符号类型，且高阶的有符号类型能容纳全部的无符号，则双方转换为高阶的有符号-的无符号版本-传统C
        unsigned int UI = -4;
        long int LI = -3;
        if (UI < LI)
           printf("long + unsinged == long\n");
        else
           printf("long + unsinged == unsigned\n");
    }

    {//无符号和高阶有符号类型，高阶有符号无法容纳全部无符号的，则将双方转为高阶的无符号版本
        unsigned int i = 0;
        signed int sc = -1;
        double uisc = i + sc;       //4294967295
        waitLookReg();
    }

    waitEnter();
}
