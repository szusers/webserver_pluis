#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h> //文件状态头文件
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include "../locker/locker.h"
#include <sys/uio.h>
#include "/home/ssdstation/webserver/noactive/lst_timer.h"

class util_timer;  // 前向声明（当编译交叉包含的类的时候这部操作非常重要！！！！）

class http_conn
{
public:

    // 所有对象共享一个epollfd（一个epollfd就足以管理所有连接上服务器的客户端了，而且本来epoll就是干管理和统筹的事的）
    static int m_epollfd; // 所有的socket上的事件都被注册到同一个epoll对象中
    static int m_user_count; // 统计用户数量
    static const int READ_BUFFER_SIZE = 2048; // 读缓冲区大小
    static const int WRITE_BUFFER_SIZE = 2048; // 写缓冲区大小
    static const int FILENAME_LEN = 200;        // 文件名的最大长度


    ///////////////////////////////////////////////////////////////////////////////////////////
    // HTTP请求方法，这里只支持GET
    enum METHOD {GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT};
    
    /*
        解析客户端请求时，主状态机的状态
        CHECK_STATE_REQUESTLINE:当前正在分析请求行
        CHECK_STATE_HEADER:当前正在分析头部字段
        CHECK_STATE_CONTENT:当前正在解析请求体
    */
    enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT };
    
    /*
        服务器处理HTTP请求的可能结果，报文解析的结果
        NO_REQUEST          :   请求不完整，需要继续读取客户数据
        GET_REQUEST         :   表示获得了一个完成的客户请求
        BAD_REQUEST         :   表示客户请求语法错误
        NO_RESOURCE         :   表示服务器没有资源
        FORBIDDEN_REQUEST   :   表示客户对资源没有足够的访问权限
        FILE_REQUEST        :   文件请求,获取文件成功
        INTERNAL_ERROR      :   表示服务器内部错误
        CLOSED_CONNECTION   :   表示客户端已经关闭连接了
    */
    enum HTTP_CODE { NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION };
    
    // 从状态机的三种可能状态，即行的读取状态，分别表示
    // 1.读取到一个完整的行 2.行出错 3.行数据尚且不完整
    enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };
    //////////////////////////////////////////////////////////////////////////////////////////////////////////

    // 该HTTP对象连接的socket
    int m_sockfd; 
    // 通信的socket地址
    sockaddr_in m_address; 
    // 读缓冲区
    char m_read_buf[READ_BUFFER_SIZE]; 

    http_conn();
    ~http_conn();

    // 处理客户端请求(也处理相应，并解析http请求报文) 主线程检测到可写就交给process处理相应
    void process(); 

    // 初始化新接收的连接
    void init(int sockfd, const sockaddr_in& addr);

    // 关闭连接
    void close_conn();

    // 非阻塞的读（也就是我们在没有数据可读的情况下不等待，直接往下执行）
    bool read(int&);

    // 非阻塞的写（也就是我们在没有数据可写的情况下不等待，直接往下执行）
    bool write();

    util_timer* timer;          // 定时器


private:
    // 标识读缓冲区中以及读入的客户端数据的最后一个字节的下一位（下一次开始读数据的起点）
    int m_read_idx; 
    // 当前分析字符在读缓冲区位置
    int m_check_index;
    // 当前正在解析的行的起始位置
    int m_start_line;
    // 请求目标文件的文件名
    char* m_url;
    // 协议版本，只支持HTTP1.1
    char* m_version;
    // 请求方法
    METHOD m_method;
    // 主机名
    char* m_host;
    // 判断http请求是否要保持连接
    bool m_linger;
    // http请求的消息总长度
    int m_content_length; 

    // 客户请求的目标文件的完整路径，其内容等于 doc_root + m_url, doc_root是网站根目录
    char m_real_file[ FILENAME_LEN ];       

    // 主状态机当前所处的状态
    CHECK_STATE m_check_state;

    // 初始化连接的其余信息
    void init();
    // 解析http请求
    HTTP_CODE process_read();
    // 填充HTTP应答
    bool process_write( HTTP_CODE ret );    


    // 下面这一组函数被process_read调用以分析HTTP请求
    
    // 解析请求首行
    HTTP_CODE parse_request_line( char* text );
    
    // 解析请求头
    HTTP_CODE parse_headers( char* text );
    
    // 解析请求体
    HTTP_CODE parse_content( char* text );
    
    
    HTTP_CODE do_request();
    
    
    // 解析具体某一行（从状态机）
    LINE_STATUS parse_line();

    // 获取到的信息首地址 + 我们目前遍历到的行数就是我们要返回的报头中的某一行
    char* get_line(){
        return m_read_buf + m_start_line;
    }


    char m_write_buf[ WRITE_BUFFER_SIZE ];  // 写缓冲区
    int m_write_idx;                        // 写缓冲区中待发送的字节数
    char* m_file_address;                   // 客户请求的目标文件被mmap到内存中的起始位置
    struct stat m_file_stat;                // 目标文件的状态。通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息
    struct iovec m_iv[2];                   // 我们将采用writev来执行写操作，所以定义下面两个成员，其中m_iv_count表示被写内存块的数量。
    int m_iv_count;

    int bytes_to_send;              // 将要发送的数据的字节数
    int bytes_have_send;            // 已经发送的字节数



    // 这一组函数被process_write调用以填充HTTP应答。
    void unmap();
    bool add_response( const char* format, ... );
    bool add_content( const char* content );
    bool add_content_type();
    bool add_status_line( int status, const char* title );
    bool add_headers( int content_length );
    bool add_content_length( int content_length );
    bool add_linger();
    bool add_blank_line();

    
};



#endif

