#include <lua.hpp>

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
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
#include <string.h> 
#include <fcntl.h>
#include <stdarg.h>

extern "C" {
    int luaopen_tfmail (lua_State *L);
}

#define NAME_BUF_MAX (1024*1024)
#define CONTENT_BUF_MAX (1024*1024*4)
#define ERR_MSG_MAX (1024*1024)

int return2Lua(lua_State *L, int ret, const char *format, ...);

void sendemail(char *email,char *body);

//发邮件接口
static int sendM(lua_State *L)
{
    int n = lua_gettop(L);  //看看有多少个参数
    if (n <= 0) {
        return return2Lua(L, -1, "callArgCntToLow.cnt:%d", n);
    }

    //只保留第一个参数，丢弃多余的Lua参数
    lua_settop(L, 1);

    //仅仅看第一个参数的类型是否是表
    int type = lua_type(L, 1); 
    if (LUA_TTABLE != type) {
        return return2Lua(L, -2, "callArgTypeInvalid.type:%d", type);
    }

    //查看name域
    lua_getfield(L, 1, "name");
    type = lua_type(L, -1);
    if (LUA_TSTRING != type) {
        return return2Lua(L, -3, "mail.recvName.type must be string,not:%s", lua_typename(L, lua_type(L, -1)));
    }

    char name[NAME_BUF_MAX] = {0};
    const char *addr = lua_tolstring(L, -1, NULL);
    strncpy(name, addr, sizeof(name));
    printf("mail.name:%s\n", name);

    //弹掉上面的value
    lua_pop(L, 1);
    type = lua_getfield(L, 1, "content");
    if (LUA_TSTRING != type) {
        return return2Lua(L, -4, "mail.content.type must be string,not:%d", type);
    }

    char content[CONTENT_BUF_MAX] = {0};
    addr = lua_tolstring(L, -1, NULL);
    strncpy(content, addr, sizeof(content));
    printf("mail.content:%s\n", content);

    sendemail(name, content);

    return return2Lua(L, 0, NULL);
}

static const struct luaL_Reg mailLib[] =
{
    {"send", sendM},
    {NULL, NULL}
};

//这里的函数名和生存的库名必须"兼容",简单来说就是一样
extern "C" int luaopen_tfmail(lua_State* L)
{
    luaL_newlib(L, mailLib);
    return 1;
}

int return2Lua(lua_State *L, int ret, const char *fmt, ...)
{
    if (0 == ret) {
        return 0;
    }

    //申请错误字符串的空间
    char buf[1024*1024] = {0};

    //组装好字符串
    va_list valist;
    va_start(valist, fmt);
    sprintf(buf, fmt, valist);
    va_end(valist);

    //压栈，返回
    lua_pushstring(L, buf);
    return 1;
}

 struct data6
 {
      unsigned int d4:6;
      unsigned int d3:6;
      unsigned int d2:6;
      unsigned int d1:6;
};
// 协议中加密部分使用的是base64方法
 
char con628(char c6);
void base64(char *dbuf,char *buf128,int len);
int open_socket(struct sockaddr *addr);
  
int tmp()
{
  char recvLst[] = "18680292450@163.com";
  char body[] = 
  "From: \"彭焰峰\"<253696242@qq.com>\r\n"
  "To: \"18680292450\"<18680292450@163.com>\r\n"
  "Subject: Hello\r\n\r\n"
  "Hello World, Hello Email!";
  sendemail(recvLst, body);
  return 0;
}
  
char con628(char c6)
{
  char rtn = '\0';
  if (c6 < 26) rtn = c6 + 65;
  else if (c6 < 52) rtn = c6 + 71;
  else if (c6 < 62) rtn = c6 - 4;
  else if (c6 == 62) rtn = 43;
  else rtn = 47;
  return rtn;
}
 
 // base64的实现
 void base64(char *dbuf, char *buf128, int len)
{
    struct data6 *ddd = NULL;
    int i = 0;
	char buf[256] = {0};
	char *tmp = NULL;
	char cc = '\0';
	memset(buf, 0, 256);
	strcpy(buf, buf128);
	for(i = 1; i <= len/3; i++)
    {
	    tmp = buf+(i-1)*3;
	    cc = tmp[2];
	    tmp[2] = tmp[0];
	    tmp[0] = cc;
	    ddd = (struct data6 *)tmp;
	    dbuf[(i-1)*4+0] = con628((unsigned int)ddd->d1);
	    dbuf[(i-1)*4+1] = con628((unsigned int)ddd->d2);
	    dbuf[(i-1)*4+2] = con628((unsigned int)ddd->d3);
	    dbuf[(i-1)*4+3] = con628((unsigned int)ddd->d4);
    }
    if(len%3 == 1)
   	{
	  	tmp = buf+(i-1)*3;
	    cc = tmp[2];
	    tmp[2] = tmp[0];
	   	tmp[0] = cc;
	    ddd = (struct data6 *)tmp;
	    dbuf[(i-1)*4+0] = con628((unsigned int)ddd->d1);
	     dbuf[(i-1)*4+1] = con628((unsigned int)ddd->d2);
	     dbuf[(i-1)*4+2] = '=';
	     dbuf[(i-1)*4+3] = '=';
    }
  	if(len%3 == 2)
  	{
      	tmp = buf+(i-1)*3;
      	cc = tmp[2];
      	tmp[2] = tmp[0];
      	tmp[0] = cc;
      	ddd = (struct data6 *)tmp;
      	dbuf[(i-1)*4+0] = con628((unsigned int)ddd->d1);
      	dbuf[(i-1)*4+1] = con628((unsigned int)ddd->d2);
      	dbuf[(i-1)*4+2] = con628((unsigned int)ddd->d3);
      	dbuf[(i-1)*4+3] = '=';
  	}
    return;
}

  // 发送邮件
void sendemail(char *recvLst, char *body)
{
    printf("name:%s\n", recvLst);
    printf("content:%s\n", body);
	int sockfd = 0;
    char buf[1500] = {0};
    char rbuf[1500] = {0};
    char login[128] = {0};
    char pass[128] = {0};
    struct sockaddr_in their_addr = {0};
    memset(&their_addr, 0, sizeof(their_addr));
    their_addr.sin_family = AF_INET;
    their_addr.sin_port = htons(25);
    struct hostent* phost = gethostbyname("smtp.qq.com");
    memcpy(&their_addr.sin_addr.s_addr, phost->h_addr_list[0], phost->h_length);
                                                        
     // 连接邮件服务器，如果连接后没有响应，则2 秒后重新连接
     sockfd = open_socket((struct sockaddr *)&their_addr);
     memset(rbuf,0,1500);
     while(recv(sockfd, rbuf, 1500, 0) == 0)
     {
         printf("reconnect...\n");
         sleep(2);
         //close(sockfd);
         sockfd = open_socket((struct sockaddr *)&their_addr);
 
         memset(rbuf,0,1500);
     }
 
     printf("%s\n", rbuf);
 
     // EHLO
     memset(buf, 0, 1500);
     sprintf(buf, "EHLO abcdefg-PC\r\n");
     send(sockfd, buf, strlen(buf), 0);
     memset(rbuf, 0, 1500);
     recv(sockfd, rbuf, 1500, 0);
     printf("%s\n", rbuf);
 
     // AUTH LOGIN
     memset(buf, 0, 1500);
     sprintf(buf, "AUTH LOGIN\r\n");
     send(sockfd, buf, strlen(buf), 0);
     printf("%s\n", buf);
     memset(rbuf, 0, 1500);
     recv(sockfd, rbuf, 1500, 0);
     printf("%s\n", rbuf);
 
     // USER
     memset(buf, 0, 1500);
     sprintf(buf,"253696242");//你的qq号
     memset(login, 0, 128);
     base64(login, buf, strlen(buf));
     sprintf(buf, "%s\r\n", login);
     send(sockfd, buf, strlen(buf), 0);
     printf("%s\n", buf);
     memset(rbuf, 0, 1500);
     recv(sockfd, rbuf, 1500, 0);
     printf("%s\n", rbuf);
 
     // PASSWORD
     sprintf(buf, "evnizedujgtxbiab");//你的qq密码,这里是QQ-SMTP授权码
     memset(pass, 0, 128);
     base64(pass, buf, strlen(buf));
     sprintf(buf, "%s\r\n", pass);
     send(sockfd, buf, strlen(buf), 0);
     // printf("%s\n", buf);
 
     // memset(rbuf, 0, 1500);
     // recv(sockfd, rbuf, 1500, 0);
     // printf("%s\n", rbuf);

     // MAIL FROM
     memset(buf, 0, 1500);
     sprintf(buf, "MAIL FROM: <253696242@qq.com>\r\n");
     send(sockfd, buf, strlen(buf), 0);
     // memset(rbuf, 0, 1500);
     // recv(sockfd, rbuf, 1500, 0);
     // printf("%s\n", rbuf);
 
     // RCPT TO 第一个收件人
     sprintf(buf, "RCPT TO:<%s>\r\n", recvLst);
     send(sockfd, buf, strlen(buf), 0);
     // memset(rbuf, 0, 1500);
     // recv(sockfd, rbuf, 1500, 0);
     // printf("%s\n", rbuf);
 
     // DATA 准备开始发送邮件内容
     sprintf(buf, "DATA\r\n");
     send(sockfd, buf, strlen(buf), 0);
     // memset(rbuf, 0, 1500);
     // recv(sockfd, rbuf, 1500, 0);
     // printf("%s\n", rbuf);

     // 发送邮件内容，\r\n.\r\n内容结束标记
     sprintf(buf, "%s\r\n.\r\n", body);
     send(sockfd, buf, strlen(buf), 0);
     // memset(rbuf, 0, 1500);
     // recv(sockfd, rbuf, 1500, 0);
     // printf("%s\n", rbuf);
 
     // QUIT
     sprintf(buf, "QUIT\r\n");
     send(sockfd, buf, strlen(buf), 0);
     memset(rbuf, 0, 1500);
     recv(sockfd, rbuf, 1500, 0);
     printf("%s\n", rbuf);
 
 	close(sockfd);
    return;
}

// 打开TCP Socket连接
int open_socket(struct sockaddr *addr)
{
	int sockfd = 0;
	sockfd=socket(PF_INET, SOCK_STREAM, 0);
	if(sockfd < 0)
	{
	    fprintf(stderr, "Open sockfd(TCP) error!\n");
	    exit(-1);
	}
	if(connect(sockfd, addr, sizeof(struct sockaddr)) < 0)
	{
	    fprintf(stderr, "Connect sockfd(TCP) error!\n");
	    exit(-1);
	}
	printf("connect server done......\n");
	return sockfd;
} 

