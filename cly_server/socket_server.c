#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "command.h"
#include "./utils/color.h"
// #include "./utils/com_parse/parse.h"
#include "parse.h"
#include "./utils/com_validate/validate.h"

#define SERVER_PORT     8888    //端口号不能发生冲突,不常用的端口号通常大于5000
#define TMPF "/home/alientek/C_program/temp"

int tmpfd;
int measure_state = 0;
int ifsend;


char **command_parse(char *line)
{
    int bufsize = PARSE_MAX, position = 0;
    char **tokens = malloc(bufsize * sizeof(char*));
    char *token;

    if (!tokens) {
    fprintf(stderr, "lsh: allocation error\n");
    exit(EXIT_FAILURE);
    }

    token = strtok(line, PARCE_DELI);

    while (token != NULL) {
        tokens[position] = token;
        position++;
        token = strtok(NULL, PARCE_DELI);
    }
    tokens[position] = NULL;
    return tokens;
}


static void data2file(char tmpfile[], char buf[], int size)
{   
    int tmpfd;
    int ret;

    // printf("size:<%d>\n", size);

    tmpfd = open(tmpfile, O_WRONLY | O_CREAT | O_APPEND, S_IRWXU | S_IRGRP | S_IROTH);
    ret = lseek(tmpfd, 0, SEEK_END);
    ret = write(tmpfd, buf, size);  
    close(tmpfd);
}


void add_command(char command[])
{
    int len = strlen(command);

    if ((command[len-1] == 0xa) && (command[len-2] != 0xd))
    {
        command[len] = 0xa;
        command[len-1]=0xd;
    }
}


int main(void)
{
    struct sockaddr_in server_addr = {0};
    struct sockaddr_in client_addr = {0};
    char ip_str[20] = {0};
    int sockfd, connfd;
    int addrlen = sizeof(client_addr);
    char buf[1024];
    int ret;

    /* 打开套接字，得到套接字描述符 */
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (0 > sockfd) {
        perror("socket error");
        exit(EXIT_FAILURE);
    }

    /* 将套接字与指定端口号进行绑定 */
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(SERVER_PORT);

    ret = bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (0 > ret) {
        perror("bind error");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    /* 使服务器进入监听状态 */
    ret = listen(sockfd, 50);
    if (0 > ret) {
        perror("listen error");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    /* 阻塞等待客户端连接 */
    connfd = accept(sockfd, (struct sockaddr *)&client_addr, &addrlen);
    if (0 > connfd) {
        perror("accept error");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    printf("INFO:有客户端接入...\n");
    inet_ntop(AF_INET, &client_addr.sin_addr.s_addr, ip_str, sizeof(ip_str));
    printf("INFO:客户端主机的IP地址: %s\n", ip_str);
    printf("INFO:客户端进程的端口号: %d\n", client_addr.sin_port);

    /* 接收客户端发送过来的数据 */
    for ( ; ; ) {

        memset(buf, 0x0, sizeof(buf));
        
        // 接收用户输入的字符串数据
        // printf("client@cly001$：");
        printf(L_GREEN "client" L_BLUE "@cly001$" NONE ":");
        fgets(buf, sizeof(buf), stdin);
        // printf("已经得到指令");
        add_command(buf);
        // printf("add_command指令已执行\n");


        if (memcmp(sig_start_measure, buf, strlen(buf)) == 0)
        {
            if (measure_state == 0)
            {
                ret = send(connfd, buf, strlen(buf), 0);
                measure_state = 1;
            }
            else 
            {
                printf("The command has been excuted, don't write again!\n");
            }
        }

        if (memcmp(sig_stop_measure, buf, strlen(buf)) == 0)
        {
            if (measure_state == 0)
            {
                printf("It is not measuring now,why stop?\n");
            }
            else
            {
                ret = send(connfd, buf, strlen(buf), 0);
                measure_state = 0;
            }
        }

        if(memcmp(sig_transfer, buf, strlen(buf)) == 0)
        {
            ret = send(connfd, buf, strlen(buf), 0);
            printf("server开始测量\n");
            while (1)
            {
                memset(buf, 0x0, sizeof(buf));
                printf("server等待数据\n");
                ret = recv(connfd, buf, sizeof(buf), 0);
                printf("读取文件数据:\n%s\n", buf);


                printf("server准备写入数据\n");
                ret = strlen(buf);
                int sz = sizeof(buf);

                int size;
                if (ret > sz){
                    size = sz;
                }
                else{
                    size = ret;
                }
                data2file(TMPF, buf, size);

                if (ret < sz)
                {
                    break;
                }
            }
        }

        if (memcmp(buf, "get", 3) == 0){
            char **tokens;                             //tokens
            char file_name[1024] = {0};                //文件名字
            int cnt = 0;                               //用于计数，传输几个文件

            int tmpfd = 0;                            //传输文件所用标志服

            ret = send(connfd, buf, strlen(buf), 0);
            printf("send:%s\n", buf);

            tokens = command_parse(buf);
            for (int i = 0; NULL != tokens[i]; i++){
                // printf("tokens[%d]:%s", i, tokens[i]);
                if (memcmp(tokens[i], "day", 3) == 0){
                    cnt += 1;
                }
            }
            printf("INFO：待传输文件%d个\n", cnt);

            for(int i = 0; i < cnt; ++i){
                memset(buf, 0x0, sizeof(buf));
                ret = recv(connfd, buf, sizeof(buf), 0);
                if (memcmp(buf, "$", 1) == 0)
                    strncpy(file_name, buf+1, 10);
                else
                    printf("INFO:传输错误\n");
                printf("INFO：正在传输file_name:%s文件\n", file_name);

                tmpfd = open(file_name, O_WRONLY | O_CREAT | O_APPEND, S_IRWXU | S_IRGRP | S_IROTH);
                ret = lseek(tmpfd, 0, SEEK_END);
                while (1)
                {
                    memset(buf, 0x0, sizeof(buf));
                    ret = recv(connfd, buf, sizeof(buf), 0);

                    if (memcmp(buf, "&", 1) == 0){
                        close(tmpfd);
                        break;
                    }
                    else{
                        ret = strlen(buf);
                        int sz = sizeof(buf);

                        int size;
                        if (ret > sz){
                            size = sz;
                        }
                        else{
                            size = ret;
                        }
                        ret = write(tmpfd, buf, size);  
                    }
                }
            }
        }

    }

    /* 关闭套接字 */
    close(sockfd);
    exit(EXIT_SUCCESS);
}
