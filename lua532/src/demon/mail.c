#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <netdb.h>

#define SMTP_BUFSIZE 1024

void EncodeBase64(char* src, char* encode);  // Base64编码
int SendMail(char *from, char *pwd, char* to, char* title, char* text); // 发送邮件
int send_fd(int fd, void *buf, size_t len, int flag);
int recv_fd(int fd, void *buf, size_t len, int flag);
typedef unsigned int byte;

int main()
{
    int ret = SendMail("m18680292450@163.com", "mm123456789", "18680292450@163.com", "Hello", "This is a test mail");
    return ret;
}

// Base64编码
void EncodeBase64(char* src, char* encode)
{
    char base64_table[] = {
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
        'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
        'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
        'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/', '='};
    int len = strlen(src);
    int i = 0;
    for(i = 0; i < len/3; i++)
    {
        int temp = (byte)src[3*i+2] + (byte)(src[3*i+1]) << 8 + (byte)(src[3*i]) << 16;
        encode[4*i] = base64_table[(temp & 0xfc0000) >> 18];
        encode[4*i+1] = base64_table[(temp & 0x3f000) >> 12];
        encode[4*i+2] = base64_table[(temp & 0xfc0) >> 6];
        encode[4*i+3] = base64_table[temp & 0x3f];
    }
    encode[4*i] = 0;
    if (1 == len % 3)
    {
        int temp = (byte)(src[3*i]) << 16;
        encode[4*i] = base64_table[(temp & 0xfc0000) >> 18];
        encode[4*i+1] = base64_table[(temp & 0x3f000) >> 12];
        encode[4*i+2] = base64_table[64];
        encode[4*i+3] = base64_table[64];
        encode[4*i+4] = 0;
    }
    else if (2 == len % 3)
    {
        int temp =(byte)(src[3*i+1]) << 8 + (byte)(src[3*i]) << 16;
        encode[4*i] = base64_table[(temp & 0xfc0000) >> 18];
        encode[4*i+1] = base64_table[(temp & 0x3f000) >> 12];
        encode[4*i+2] = base64_table[(temp & 0xfc0) >> 6];
        encode[4*i+3] = base64_table[64];
        encode[4*i+4] = 0;
    }
}

// 发送邮件
int SendMail(char *from, char *pwd, char* to, char* title, char* text)
{
    char buf[SMTP_BUFSIZE] = {0};
    char account[128] = {0};
    char password[128] = {0};
    // 连接邮件服务器
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(25);
    struct hostent* phost = gethostbyname("smtp.163.com");
    memcpy(&addr.sin_addr.s_addr, phost->h_addr_list[0], phost->h_length);
    int sockfd = 0;
    if ((sockfd = socket(PF_INET, SOCK_STREAM, 0)) < 0)
    {
        printf("smtp socket() error");
        return 1;
    }
    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(struct sockaddr)) < 0)
    {
        printf("smtp connect() error");
        return 2;
    }
    // EHLO
    char pcname[128] = "jf-dev";
    sprintf(buf, "EHLO %s\r\n", pcname);
    send_fd(sockfd, buf, strlen(buf), 0);
    recv_fd(sockfd, buf, SMTP_BUFSIZE, 0);

    // AUTH LOGIN
    sprintf(buf, "AUTH LOGIN\r\n");
    send_fd(sockfd, buf, strlen(buf), 0);
    recv_fd(sockfd, buf, SMTP_BUFSIZE, 0);

    // 邮箱账号
    EncodeBase64(from, account);
    sprintf(buf, "%s\r\n", account);
    send_fd(sockfd, buf, strlen(buf), 0);
    recv_fd(sockfd, buf, SMTP_BUFSIZE, 0);
    printf("######################\n");

    // 密码
    EncodeBase64(pwd, password);
    sprintf(buf, "%s\r\n", password);
    send_fd(sockfd, buf, strlen(buf), 0);
    recv_fd(sockfd, buf, SMTP_BUFSIZE, 0);

    // MAIL FROM 发件人
    sprintf(buf, "MAIL FROM:<%s>\r\n", from);
    send_fd(sockfd, buf, strlen(buf), 0);
    recv_fd(sockfd, buf, SMTP_BUFSIZE, 0);

    // RCPT TO 收件人
    sprintf(buf, "RCPT TO:<%s>\r\n", to);
    send_fd(sockfd, buf, strlen(buf), 0);
    recv_fd(sockfd, buf, SMTP_BUFSIZE, 0);

    // DATA 准备开始发送邮件内容
    sprintf(buf, "DATA\r\n");
    send_fd(sockfd, buf, strlen(buf), 0);
    recv_fd(sockfd, buf, SMTP_BUFSIZE, 0);

    // 发送邮件内容
    sprintf(buf, "From: \"yang\"<%s>\r\nTo: \"test\"<%s>\r\nSubject: %s\r\n\r\n%s\r\n.\r\n", from, to, title, text);
    send_fd(sockfd, buf, strlen(buf), 0);
    recv_fd(sockfd, buf, SMTP_BUFSIZE, 0);

    // QUIT 结束
    sprintf(buf, "QUIT\r\n");
    send_fd(sockfd, buf, strlen(buf), 0);
    recv_fd(sockfd, buf, SMTP_BUFSIZE, 0);
    if (strlen(buf) >= 3)
    {
        if (buf[0] == '2' && buf[1] == '5' && buf[2] == '0')
        {
            printf("sucess\n");
        }
    }

    close(sockfd);

    // for pause
    getchar();

    return 0;
}

int send_fd(int fd, void *buf, size_t len, int flags)
{
    return send(fd, buf, len, flags);
}

int recv_fd(int fd, void *buf, size_t len, int flags)
{   
    recv(fd, buf, len, flags);
    printf("recv:%s\n", buf);
}
