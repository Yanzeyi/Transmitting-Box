#define _GNU_SOURCE     //在源文件开头定义_GNU_SOURCE宏
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <termios.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define DEVICE "/dev/ttymxc2"
#define GPS_DEVICE "/dev/ttymxc1"
#define TMP_FILE "/home/root/cly_data/"

//*******************网络相关*****************//

#define SERVER_PORT         8888          	//服务器的端口号
#define SERVER_IP   	"192.168.137.100"	//服务器的IP地址

//********************************************//

#define PARSE_MAX 64
#define PARCE_DELI " /+" 

//*********************************************//

char answer_sig_start_measure[] = {0x24, 0x37, 0x2C, 0x34, 0x2A, 0x32, 0x66, 0x0D, 0x0A}; //开始测量的应答指令
char sig_start_measure[] = {0x24, 0x34, 0x2C, 0x30, 0x2A, 0x32, 0x39, 0x0D, 0x0A};        //开始测量
char sig_stop_measure[] = {0x24, 0x35, 0x2C, 0x30, 0x2A, 0x32, 0x39, 0x0D, 0x0A};
char sig_transfer[] = {0x24, 0x39, 0x0D, 0x0A};

int mode = 1;

static struct termios old_cfg;  //用于保存终端的配置参数
static struct termios gps_old_cfg;  //用于保存串口配置参数

static int fd;                  //uart串口的标志符
// static int fd_read;
static int gpsfd;               //uart2串口的标志符
static int tmpfd;               //数据文件标志符


static pthread_rwlock_t rwlock;   //定义读写锁，避免写数据与传文件冲突
static pthread_mutex_t mutex;     //定义互斥锁，避免读线程与写线程冲突
static pthread_spinlock_t spin;//定义自旋锁

static char utc_date_g[1024] = {0};
// static char utc_time[1024] = {0};
static char tmp_file_dir[1024]  = {0};

static char num_data_file[10] = {0};       //使用Linux系统命令检测有多少个文件
static char isexist[10] = {0};             //查询当前日期的文件是否存在，存在则继续存储


typedef struct uart_hardware_cfg { 
    unsigned int baudrate;      /* 波特率 */
    unsigned char dbit;         /* 数据位 */
    char parity;                /* 奇偶校验 */
    unsigned char sbit;         /* 停止位 */
} uart_cfg_t;                   //uart配置

int sockfd;


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


static int uart_init(const char *device)
{
    /* 打开串口终端 */
    fd = open(device, O_RDWR | O_NOCTTY);
    if (0 > fd) {
        fprintf(stderr, "open error: %s: %s\n", device, strerror(errno));
        return -1;
    }

    /* 获取串口当前的配置参数 */
    if (0 > tcgetattr(fd, &old_cfg)) {
        fprintf(stderr, "tcgetattr error: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    return 0;
}


static int uart_init_gps(const char *device)
{
    /* 打开串口终端 */
    gpsfd = open(device, O_RDWR | O_NOCTTY);
    if (0 > gpsfd) {
        fprintf(stderr, "open error: %s: %s\n", device, strerror(errno));
        return -1;
    }

    /* 获取串口当前的配置参数 */
    if (0 > tcgetattr(gpsfd, &gps_old_cfg)) {
        fprintf(stderr, "tcgetattr error: %s\n", strerror(errno));
        close(gpsfd);
        return -1;
    }
    return 0;
}

static int uart_cfg(const uart_cfg_t *cfg)
{
    static int cnt = 0;
    struct termios new_cfg = {0};   //将new_cfg对象清零
    speed_t speed;

    /* 设置为原始模式 */
    cfmakeraw(&new_cfg);

    /* 使能接收 */
    new_cfg.c_cflag |= CREAD;

    /* 设置波特率 */
    switch (cfg->baudrate) {
    case 1200: speed = B1200;
        break;
    case 1800: speed = B1800;
        break;
    case 2400: speed = B2400;
        break;
    case 4800: speed = B4800;
        break;
    case 9600: speed = B9600;
        break;
    case 19200: speed = B19200;
        break;
    case 38400: speed = B38400;
        break;
    case 57600: speed = B57600;
        break;
    case 115200: speed = B115200;
        break;
    case 230400: speed = B230400;
        break;
    case 460800: speed = B460800;
        break;
    case 500000: speed = B500000;
        break;
    default:    //默认配置为115200
        speed = B115200;
        // printf("default baud rate: 115200\n");
        break;
    }

    if (0 > cfsetspeed(&new_cfg, speed)) {
        fprintf(stderr, "cfsetspeed error: %s\n", strerror(errno));
        return -1;
    }

    /* 设置数据位大小 */
    new_cfg.c_cflag &= ~CSIZE;  //将数据位相关的比特位清零
    switch (cfg->dbit) {
    case 5:
        new_cfg.c_cflag |= CS5;
        break;
    case 6:
        new_cfg.c_cflag |= CS6;
        break;
    case 7:
        new_cfg.c_cflag |= CS7;
        break;
    case 8:
        new_cfg.c_cflag |= CS8;
        break;
    default:    //默认数据位大小为8
        new_cfg.c_cflag |= CS8;
        // printf("default data bit size: 8\n");
        break;
    }

    /* 设置奇偶校验 */
    switch (cfg->parity) {
    case 'N':       //无校验
        new_cfg.c_cflag &= ~PARENB;
        new_cfg.c_iflag &= ~INPCK;
        break;
    case 'O':       //奇校验
        new_cfg.c_cflag |= (PARODD | PARENB);
        new_cfg.c_iflag |= INPCK;
        break;
    case 'E':       //偶校验
        new_cfg.c_cflag |= PARENB;
        new_cfg.c_cflag &= ~PARODD; /* 清除PARODD标志，配置为偶校验 */
        new_cfg.c_iflag |= INPCK;
        break;
    default:    //默认配置为无校验
        new_cfg.c_cflag &= ~PARENB;
        new_cfg.c_iflag &= ~INPCK;
        // printf("default parity: N\n");
        break;
    }

    /* 设置停止位 */
    switch (cfg->sbit) {
    case 1:     //1个停止位
        new_cfg.c_cflag &= ~CSTOPB;
        break;
    case 2:     //2个停止位
        new_cfg.c_cflag |= CSTOPB;
        break;
    default:    //默认配置为1个停止位
        new_cfg.c_cflag &= ~CSTOPB;
        // printf("default stop bit size: 1\n");
        break;
    }
    // printf("当前cnt值是:%d", cnt);
    /* 将MIN和TIME设置为0 */
    new_cfg.c_cc[VTIME] = 0;
    new_cfg.c_cc[VMIN] = 0;

    /* 清空缓冲区 */
    if (0 > tcflush(fd, TCIOFLUSH)) {
        fprintf(stderr, "tcflush error: %s\n", strerror(errno));
        return -1;
    }


    /* 写入配置、使配置生效 */
    if (0 > tcsetattr(fd, TCSANOW, &new_cfg)) {
        fprintf(stderr, "tcsetattr error: %s\n", strerror(errno));
        return -1;
    }

    /* 配置OK 退出 */
    return 0;
}


static int uart_cfg_gps(const uart_cfg_t *cfg)
{
    struct termios new_cfg = {0};   //将new_cfg对象清零
    speed_t speed;

    /* 设置为原始模式 */
    cfmakeraw(&new_cfg);

    /* 使能接收 */
    new_cfg.c_cflag |= CREAD;

    /* 设置波特率 */
    switch (cfg->baudrate) {
    case 1200: speed = B1200;
        break;
    case 1800: speed = B1800;
        break;
    case 2400: speed = B2400;
        break;
    case 4800: speed = B4800;
        break;
    case 9600: speed = B9600;
        break;
    case 19200: speed = B19200;
        break;
    case 38400: speed = B38400;
        break;
    case 57600: speed = B57600;
        break;
    case 115200: speed = B115200;
        break;
    case 230400: speed = B230400;
        break;
    case 460800: speed = B460800;
        break;
    case 500000: speed = B500000;
        break;
    default:    //默认配置为115200
        speed = B115200;
        printf("default baud rate: 115200\n");
        break;
    }

    if (0 > cfsetspeed(&new_cfg, speed)) {
        fprintf(stderr, "cfsetspeed error: %s\n", strerror(errno));
        return -1;
    }

    /* 设置数据位大小 */
    new_cfg.c_cflag &= ~CSIZE;  //将数据位相关的比特位清零
    switch (cfg->dbit) {
    case 5:
        new_cfg.c_cflag |= CS5;
        break;
    case 6:
        new_cfg.c_cflag |= CS6;
        break;
    case 7:
        new_cfg.c_cflag |= CS7;
        break;
    case 8:
        new_cfg.c_cflag |= CS8;
        break;
    default:    //默认数据位大小为8
        new_cfg.c_cflag |= CS8;
        printf("default data bit size: 8\n");
        break;
    }

    /* 设置奇偶校验 */
    switch (cfg->parity) {
    case 'N':       //无校验
        new_cfg.c_cflag &= ~PARENB;
        new_cfg.c_iflag &= ~INPCK;
        break;
    case 'O':       //奇校验
        new_cfg.c_cflag |= (PARODD | PARENB);
        new_cfg.c_iflag |= INPCK;
        break;
    case 'E':       //偶校验
        new_cfg.c_cflag |= PARENB;
        new_cfg.c_cflag &= ~PARODD; /* 清除PARODD标志，配置为偶校验 */
        new_cfg.c_iflag |= INPCK;
        break;
    default:    //默认配置为无校验
        new_cfg.c_cflag &= ~PARENB;
        new_cfg.c_iflag &= ~INPCK;
        printf("default parity: N\n");
        break;
    }

    /* 设置停止位 */
    switch (cfg->sbit) {
    case 1:     //1个停止位
        new_cfg.c_cflag &= ~CSTOPB;
        break;
    case 2:     //2个停止位
        new_cfg.c_cflag |= CSTOPB;
        break;
    default:    //默认配置为1个停止位
        new_cfg.c_cflag &= ~CSTOPB;
        printf("default stop bit size: 1\n");
        break;
    }
    // printf("当前cnt值是:%d", cnt);
    /* 将MIN和TIME设置为0 */
    new_cfg.c_cc[VTIME] = 0;
    new_cfg.c_cc[VMIN] = 0;

    /* 清空缓冲区 */

    if (0 > tcflush(gpsfd, TCIOFLUSH)) {
        fprintf(stderr, "tcflush error: %s\n", strerror(errno));
        return -1;
    }

    /* 写入配置、使配置生效 */
    if (0 > tcsetattr(gpsfd, TCSANOW, &new_cfg)) {
        fprintf(stderr, "tcsetattr error: %s\n", strerror(errno));
        return -1;
    }

    /* 配置OK 退出 */
    return 0;
}

static void data2file(char tmpfile[], char buf[])
{   
    int ret = 0;
    tmpfd = open(tmpfile, O_WRONLY | O_CREAT | O_APPEND, S_IRWXU | S_IRGRP | S_IROTH);
    ret = lseek(tmpfd, 0, SEEK_END);
    ret = write(tmpfd, buf, strlen(buf));  
    close(tmpfd);
}


static void *start_measure(void *arg)
{
    int ret;
    fd_set rdfds;            //读就绪符号集
    int cnt = 0;              //用作计数器    

    char buf[1024] = {0};
    char utc_time[10] = {0};    
    char data_with_date[100];

    int sizeof_gps_data = 0;          //存储每次得到的gos数据长度

    char tmp_utc_date[10] = {0};
    int first_file_id = 0;

    if (atoi(isexist) == 0)
        first_file_id = atoi(num_data_file) + 1;
    else
        first_file_id = atoi(num_data_file);

    strcpy(tmp_utc_date, utc_date_g);

    char gps_data_tmp[1024] = {0};     //存储每次得到的gps变量

    while (1)
    {  
        sizeof_gps_data = 0;
        memset(buf, 0x0, sizeof(buf));
        do{
            FD_ZERO(&rdfds);
            FD_SET(gpsfd, &rdfds); //添加串口
            ret = select(gpsfd + 1, &rdfds, NULL, NULL, NULL);
            
            if(FD_ISSET(gpsfd, &rdfds)){
                memset(gps_data_tmp, 0x0, sizeof(gps_data_tmp));
                ret = read(gpsfd, gps_data_tmp, sizeof(gps_data_tmp));
                sizeof_gps_data += strlen(gps_data_tmp);
                strcat(buf, gps_data_tmp);
            }
        } while (sizeof_gps_data < 87);
        cnt += 1;

        printf("INFO:gps数据recv:%s\n", buf);

        strncpy(utc_date_g, buf+72, 6);         
        strncpy(utc_time, buf+20, 10);

        printf("INFO:tmp_utc_date:%s\n", tmp_utc_date);
        printf("INFO:utc_date_g:%s\n", utc_date_g);

        if (memcmp(utc_date_g, tmp_utc_date, strlen(utc_date_g)) != 0){
            printf("新建文件夹存储测量数据\n");
            first_file_id += 1;
            strcpy(tmp_utc_date, utc_date_g);
        }

        sprintf(tmp_file_dir, "%s%s_%03d", TMP_FILE, utc_date_g, first_file_id);

        if ((cnt % mode) == 0){
            printf("执行测量-写线程-utc_tine:%s,发送测量指令\n", utc_time);
            ret = write(fd, sig_start_measure, strlen(sig_start_measure));
            cnt = 0;

            FD_ZERO(&rdfds);
            FD_SET(fd, &rdfds); //添加串口
            ret = select(fd + 1, &rdfds, NULL, NULL, NULL);
            memset(buf, 0x0, sizeof(buf));
            if(FD_ISSET(fd, &rdfds)) 
            {
                ret = read(fd, buf, sizeof(buf));
                if (0 < ret) 
                printf("应答信号recv:%s\n", buf);

                if (memcmp(answer_sig_start_measure, buf, strlen(buf)) == 0)
                {
                    printf("INFO:成功接收到应答信号\n");
                    FD_ZERO(&rdfds);
                    FD_SET(fd, &rdfds); //添加串口

                    ret = select(fd + 1, &rdfds, NULL, NULL, NULL);
                    memset(buf, 0x0, sizeof(buf));

                    if(FD_ISSET(fd, &rdfds))
                    {
                        ret = read(fd, buf, sizeof(buf));
                        printf("INFO:读取数据测量<%d>个字节数据\n", ret);
                        printf("接收数据recv:%s\n", buf);

                        sprintf(data_with_date, "%s:%s", utc_time, buf);
                                    
                        pthread_rwlock_wrlock(&rwlock);
                        data2file(tmp_file_dir, data_with_date);    
                        pthread_rwlock_unlock(&rwlock);
                    }
                }
            }
        }
    }   
    return (void*)0;
}


void executeCMD(const char *cmd, char *result)   
{   
    char buf_ps[1024] = {0};   
    char ps[1024]={0};   
    FILE *ptr = NULL;
    strcpy(ps, cmd);   
    
    if((ptr=popen(ps, "r"))!=NULL)   
    {   
        while(fgets(buf_ps, 1024, ptr)!=NULL)   
        {   
           strcat(result, buf_ps);   
           if(strlen(result)>1024)   
               break;   
        }   
        pclose(ptr);   
        ptr = NULL;   
    }   
    else  
    {   
        printf("popen %s error\n", ps);   
    }   
}  


static void* file_transfer(void *arg)
{   
    char buf[1024] = {0};
    char cmd_buf[1024] = {0};
    strcpy(cmd_buf, (char *) arg);

    char** tokens;
    char utc_date_cp[10] = {0};               //utc日期的副本
    char cmd[1024] = {0};                     //cmd命令字符串
    char cmd_result[1024] = {0};              //存储上一行cmd的结果

    char day_init_char[10] = {0};             //当前的传输日期对应的id
    int day_init_int = 0;                    

    char day_delta_char[10] = {0};            //相对Id的字符串类型
    int day_delta_int = 0;                    //相对id，即传输协议的day0,day1

    char day_abs_char[100] = {0};             //待传输文件名+路径
    int day_abs_int = 0;                      //文件名尾缀的绝对Id
    char file_name[20] = {0};

    int ret;
    int st_fd;                                //作为传输文件时打开文件的标识符

    char utc_date[10] = {0};                  //开启gps解析日期

    if (strlen(utc_date_g) == 0){
        fd_set rdfds;                        //读就绪符号集
        uart_cfg_t cfg = {0};                //uart 串口配置

        if (uart_init_gps(GPS_DEVICE))
            exit(EXIT_FAILURE);
    
        if (uart_cfg_gps(&cfg)) 
        {
            tcsetattr(gpsfd, TCSANOW, &gps_old_cfg);   //恢复到之前的配置
            close(gpsfd);
            exit(EXIT_FAILURE);
        }

        char gps_data_tmp[1024] = {0};
        int sizeof_gps_data = 0;

        memset(buf, 0x0, sizeof(buf));
        do{
            FD_ZERO(&rdfds);
            FD_SET(gpsfd, &rdfds); //添加串口
            ret = select(gpsfd + 1, &rdfds, NULL, NULL, NULL);
            
            if(FD_ISSET(gpsfd, &rdfds)){
                memset(gps_data_tmp, 0x0, sizeof(gps_data_tmp));
                ret = read(gpsfd, gps_data_tmp, sizeof(gps_data_tmp));
                sizeof_gps_data += strlen(gps_data_tmp);
                strcat(buf, gps_data_tmp);
            }
        } while (sizeof_gps_data != 87);

        close(gpsfd);
        strncpy(utc_date, buf+72, 6);
        strcpy(utc_date_g, utc_date);
    }

    printf("utc_date_g:%s\n", utc_date_g);
    strcpy(utc_date_cp, utc_date_g);
    memset(utc_date_g, 0x0, sizeof(utc_date_g));
    printf("utc_date_cp:%s\n", utc_date_cp);
    sprintf(cmd, "%s%s%s", "find /home/root/cly_data/ -name ", utc_date_cp, "*");
    printf("cmd:%s\n", cmd);
    executeCMD(cmd, cmd_result);
    printf("cmd_result:%s\n", cmd_result);

    strncpy(day_init_char, cmd_result+27, 3);
    printf("day_init_char:%s\n", day_init_char);

    day_init_int = atoi(day_init_char);
    printf("day_init_int:%d\n", day_init_int);

    tokens = command_parse(cmd_buf);
    for (int i = 0; NULL != tokens[i]; i++){
        printf("tokens[%d]:%s", i, tokens[i]);
        if (memcmp(tokens[i], "day", 3) == 0){
            strncpy(day_delta_char, tokens[i]+3, 1);
            day_delta_int = atoi(day_delta_char);
            day_abs_int = day_init_int - day_delta_int;
            
            memset(cmd, 0x0, sizeof(cmd));
            memset(cmd_result, 0x0, sizeof(cmd_result));
            sprintf(cmd, "%s%03d", "find /home/root/cly_data/ -name *", day_abs_int);
            executeCMD(cmd, cmd_result);
            printf("cmd_result:%s\n", cmd_result);

            strncpy(day_abs_char, cmd_result, 30);
            strncpy(file_name, day_abs_char+20, 10);
            printf("file_name:%s\n", file_name);
            
            memset(buf, 0x0, sizeof(buf));
            sprintf(buf, "%s%s", "$", file_name);                 //插入开始传输文件符
            // strcpy(buf, file_name);
            ret = send(sockfd, buf, sizeof(buf), 0);

            st_fd = open(day_abs_char, O_RDWR, S_IRWXU | S_IRGRP | S_IROTH);
            printf("st_fd:%d\n", st_fd);
            ret = lseek(st_fd, 0, SEEK_SET);
            printf("client开始文件传输:偏移量设置\n");
            while (1){
                printf("client开始文件传输:进入循环\n");
                memset(buf, 0x0, sizeof(buf));    
                
                // pthread_rwlock_rdlock(&rwlock);
                ret = read(st_fd, buf, sizeof(buf));    
                // pthread_rwlock_unlock(&rwlock);

                printf("client开始文件传输:成功读取<%d>个字节数据\n", ret);
                printf("client开始文件传输:读取文件数据:%s\n", buf);

                ret = send(sockfd, buf, sizeof(buf), 0);
                ret = strlen(buf);
                int sz = sizeof(buf);
                if (ret < sz)
                {
                    printf("client开始文件传输:传输完成\n");
                    break;
                }
            }
            close(st_fd);

            memset(buf, 0x0, sizeof(buf));
            sprintf(buf, "%s", "&");                       //插入传输文件停止符  
            ret = send(sockfd, buf, sizeof(buf), 0);       //发送停止符号
        }
    }


    // pthread_rwlock_rdlock(&rwlock);
    // int ret;
    // char buf[1024];

    // printf("client开始文件传输\n");

    // int st_fd;
    // st_fd = open(TMP_FILE, O_RDWR, S_IRWXU | S_IRGRP | S_IROTH);
    // printf("client开始文件传输:打开文件\n");
    // ret = lseek(st_fd, 0, SEEK_SET);
    // printf("client开始文件传输:偏移量设置\n");
    // while (1)
    // {
    //     printf("client开始文件传输:进入循环\n");
    //     memset(buf, 0x0, sizeof(buf));    
        
    //     pthread_rwlock_rdlock(&rwlock);
    //     ret = read(st_fd, buf, sizeof(buf));    
    //     pthread_rwlock_unlock(&rwlock);

    //     printf("client开始文件传输:成功读取<%d>个字节数据\n", ret);
    //     printf("client开始文件传输:读取文件数据:%s\n", buf);
    //     // printf("\n");
    //     ret = send(sockfd, buf, sizeof(buf), 0);
    //     ret = strlen(buf);
    //     int sz = sizeof(buf);
    //     if (ret < sz)
    //     {
    //         printf("client开始文件传输:传输完成\n");
    //         break;
    //     }
    // }
    // close(st_fd);

    return (void *)0;
    
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


int main(int argc, char *argv[])
{
    uart_cfg_t cfg = {0};                //uart 串口配置
    fd_set rdfds;                        //读就绪符号集
    int ret;

    char buf[1024];

    pthread_t tid_measure_send;          //该线程用于发送测量数据
    pthread_t tid_file_trans;            //该线程用于文件传输
 
    pthread_rwlock_init(&rwlock, NULL);  //文件锁初始化
    pthread_mutex_init(&mutex, NULL);    //互斥锁初始化
    pthread_spin_init(&spin, PTHREAD_PROCESS_PRIVATE);

    /*********************uart 3************************/
    if (uart_init(DEVICE))
        exit(EXIT_FAILURE);
    
    if (uart_cfg(&cfg)) 
    {
        tcsetattr(fd, TCSANOW, &old_cfg); //恢复到之前的配置
        close(fd);
        exit(EXIT_FAILURE);
    }

    printf("INFO:与磁力仪通信已准备好\n");

    /*********************uart 3************************/
    
    /*********************socket************************/
    struct sockaddr_in server_addr = {0};

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (0 > sockfd) {
        perror("socket error");
        exit(EXIT_FAILURE);
    }
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);  //端口号
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);//IP地址

    ret = connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (0 > ret) {
        perror("connect error");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    // printf("服务器连接成功...\n\n");
    printf("INFO:与上位机通信已准备好\n");
    /*********************socket************************/
    while (1)
    {
        memset(buf, 0x0, sizeof(buf));
        ret = recv(sockfd, buf, sizeof(buf), 0);
        if(0 >= ret) {
            perror("INFO:socket recv error");
            close(sockfd);
            break;
        }
        printf("INFO:成功读取<%d>个字节命令\n", ret);
        add_command(buf);
        // printf("add_command指令已执行\n", ret);
        for (int n = 0; n < strlen(buf); n++)
            printf("0x%hhx ", buf[n]);
        printf("\n");
        // printf("\ncommand指令打印\n", ret);

        if (memcmp(sig_start_measure, buf, strlen(buf)) == 0)
        {
            printf("INFO:接收到开始测量命令\n");

            /***************查看文件下有多少文件******************/
            char cmd[1024] = "ls /home/root/cly_data/ -lR | grep \"^-\" | wc -l";
            executeCMD(cmd, num_data_file);

            printf("INFO:当前文件夹下有%s个数据文件\n", num_data_file);
            /*****************************************************/

            /*********************GPS 准备************************/
            if (uart_init_gps(GPS_DEVICE))
                exit(EXIT_FAILURE);
    
            if (uart_cfg_gps(&cfg)) 
            {
                tcsetattr(gpsfd, TCSANOW, &gps_old_cfg);   //恢复到之前的配置
                close(gpsfd);
                exit(EXIT_FAILURE);
            }
            printf("INFO:GPS已准备完毕\n");            
            /*********************GPS 准备************************/

            char utc_time[15] = {0};           //用于存放utc时间，utc时间占用字符串多，防止溢出
            char utc_date[10] = {0};           //用于存放utc的年月日

            /************寻找分隔符进行数据解析，该方法目前没用***********/
            // char *token;                       //用于存储时间
            // const char *delimiters = ",";      //分隔符
            /*************************************************************/

            char gps_data_tmp[1024] = {0};      //用于存储每次读到的数据
            int sizeof_gps_data = 0;            //计算接收到数据的长度
            memset(buf, 0x0, sizeof(buf));
            do{
                FD_ZERO(&rdfds);
                FD_SET(gpsfd, &rdfds); //添加串口
                ret = select(gpsfd + 1, &rdfds, NULL, NULL, NULL);
                
                if(FD_ISSET(gpsfd, &rdfds)){
                    memset(gps_data_tmp, 0x0, sizeof(gps_data_tmp));
                    ret = read(gpsfd, gps_data_tmp, sizeof(gps_data_tmp));
                    sizeof_gps_data += strlen(gps_data_tmp);
                    strcat(buf, gps_data_tmp);
                }
            } while (sizeof_gps_data != 87);
            
            // printf("gps buf:%s\n", buf);

            strncpy(utc_time, buf+20, 10);         //局部变量赋值
            strncpy(utc_date, buf+72, 6);          //局部变量赋值
            strcpy(utc_date_g, utc_date);          //全局变量赋值

            printf("INFO:当前UTC时间：%s\n", utc_time);
            printf("INFO:当前UTC日期：%s\n", utc_date);

            sprintf(cmd, "%s%s%s", "ls /home/root/cly_data/ -lR | grep \"", utc_date, "\" | wc -l");
            executeCMD(cmd, isexist);              //isexist注意此处有一个回车符
            
            ret = pthread_create(&tid_measure_send, NULL, start_measure, NULL);
            if (ret) {
                fprintf(stderr, "pthread_create error: %s\n", strerror(ret));
                exit(-1);
            }
        }

        if (memcmp(sig_stop_measure, buf, strlen(buf)) == 0)
        {
            printf("INFO:停止测量\n");
            ret = pthread_cancel(tid_measure_send);
            ret = pthread_join(tid_measure_send, NULL);

            memset(utc_date_g, 0x0, sizeof(utc_date_g));
            memset(tmp_file_dir, 0x0, sizeof(tmp_file_dir));
            close(gpsfd);
        }

        char dest[10] = {0};
        strncpy(dest, buf, 3);

        char buf_cp[1024] = {0};
        strcpy(buf_cp, buf);

        if(memcmp(dest, "get", strlen(dest)) == 0)
        {   
            printf("接收到get指令\n");
            ret = pthread_create(&tid_file_trans, NULL, file_transfer, buf_cp);    
        }

        printf("INFO:当前命令处理完成\n");
    }

    close(sockfd);

    tcsetattr(fd, TCSANOW, &old_cfg);   //恢复到之前的配置
    close(fd);
    
    exit(EXIT_SUCCESS);

}
