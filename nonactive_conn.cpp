#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <pthread.h>
#include "noactive/lst_timer.h"
#include"http/http_conn.h"
#include"locker/locker.h"
#include"thread_pool/threadpool.h"

#define MAX_FD 65535
#define MAX_EVENT_NUMBER 10000
#define TIMESLOT 5


// 添加文件描述符  向epoll中添加需要监听的文件描述符
extern void addfd( int epollfd, int fd, bool one_shot );
// 删除文件描述符
extern void removefd( int epollfd, int fd );
// 修改文件描述符
extern void modfd(int epollfd, int fd, int ev); // 第三个参数为要修改的事件

extern int setnonblocking(int fd); // 设置文件描述符非阻塞


static int pipefd[2];
static sort_timer_lst timer_lst;
static int epollfd = 0;

void addfd( int epollfd, int fd )
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl( epollfd, EPOLL_CTL_ADD, fd, &event );
    setnonblocking( fd );
}


void sig_handler( int sig )
{
    int save_errno = errno;
    int msg = sig;
    send( pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}

void addsig( int sig )
{
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = sig_handler;
    sa.sa_flags |= SA_RESTART;
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}


void timer_handler()
{
    // 定时处理任务，实际上就是调用tick()函数
    timer_lst.tick();
    // 因为一次 alarm 调用只会引起一次SIGALARM 信号，所以我们要重新定时，以不断触发 SIGALARM信号。
    alarm(TIMESLOT);
}

// 定时器回调函数，它删除非活动连接socket上的注册事件，并关闭之。
void cb_func( http_conn* m_http_conn )
{
    epoll_ctl( epollfd, EPOLL_CTL_DEL, m_http_conn->m_sockfd, 0 );
    assert( m_http_conn );
    close( m_http_conn->m_sockfd );
    printf( "close fd %d\n", m_http_conn->m_sockfd );
}

int main( int argc, char* argv[] ) {
    if( argc <= 1 ) {
        printf( "usage: %s port_number\n", basename( argv[0] ) );
        return 1;
    }
    int port = atoi( argv[1] );

    // 创建线程池，初始化线程池
    threadpool<http_conn>* pool = NULL; // 任务类指定为http的连接任务
    try{
        pool = new threadpool<http_conn>; // 线程池类所占空间较大，在堆上创建
    }catch(...){
        exit(-1);
    }

    // 创建一个数组用于保存所有的客户端的信息
    http_conn* users = new http_conn[MAX_FD]; // int* arr = new int[5];

    // 绑定
    int ret = 0;
    struct sockaddr_in address;
    bzero( &address, sizeof( address ) );
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons( port );

    // 网络编程服务端代码，为避免出现多客户端连接时的端口异常占用情况，我们下面需要设置端口复用
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    // assert( listenfd >= 0 );

    // 设置端口复用(一定要在绑定前设置，绑定后状态被锁定就无法更改了)
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));


    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    assert( ret != -1 );

    ret = listen( listenfd, 5 );
    assert( ret != -1 );

    epoll_event events[ MAX_EVENT_NUMBER ];
    int epollfd = epoll_create( 5 );
    assert( epollfd != -1 );
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    // 创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert( ret != -1 );
    setnonblocking( pipefd[1]);
    addfd(epollfd, pipefd[0]);

    // 设置信号处理函数
    addsig( SIGALRM );
    addsig( SIGTERM );
    bool stop_server = false;


    bool timeout = false;
    alarm(TIMESLOT);  // 定时,5秒后产生SIGALARM信号

    while( !stop_server )
    {
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        if ( ( number < 0 ) && ( errno != EINTR ) ) {
            printf( "epoll failure\n" );
            break;
        }
    
        for ( int i = 0; i < number; i++ ) {
            int sockfd = events[i].data.fd;
            if( sockfd == listenfd )
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                if(http_conn::m_user_count >= MAX_FD){
                    // 目标连接数满了
                    // 给客户端写一个信息：服务器内部正忙
                    close(connfd);
                    continue;
                }                
                addfd(epollfd, connfd);
                // 将新的客户的数据初始化放入数组中（用文件描述符作为数组下标）
                users[connfd].init(connfd, client_address);                

                // 创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_lst中
                util_timer* timer = new util_timer;
                timer->m_http_conn = &users[connfd];
                timer->cb_func = cb_func;
                time_t cur = time( NULL );
                timer->expire = cur + 3 * TIMESLOT;
                users[connfd].timer = timer;
                timer_lst.add_timer( timer );
            } else if( ( sockfd == pipefd[0] ) && ( events[i].events & EPOLLIN ) ) {
                // 处理信号
                int sig;
                char signals[1024];
                ret = recv( pipefd[0], signals, sizeof( signals ), 0 );
                if( ret == -1 ) {
                    continue;
                } else if( ret == 0 ) {
                    continue;
                } else  {
                    for( int i = 0; i < ret; ++i ) {
                        switch( signals[i] )  {
                            case SIGALRM:
                            {
                                // 用timeout变量标记有定时任务需要处理，但不立即处理定时任务
                                // 这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务。
                                timeout = true;
                                break;
                            }
                            case SIGTERM:
                            {
                                stop_server = true;
                            }
                        }
                    }
                }
            }
            else if(  events[i].events & EPOLLIN )
            {
                int bytes = -1;
                util_timer* timer = users[sockfd].timer;
                if(users[sockfd].read(bytes)) { // 一次性读完所有数据
                    pool->append(users + sockfd);
                    // 如果某个客户端上有数据可读，则我们要调整该连接对应的定时器，以延迟该连接被关闭的时间。
                    if( timer ) {
                    time_t cur = time( NULL );
                    timer->expire = cur + 3 * TIMESLOT;
                    printf( "adjust timer once\n" );
                    timer_lst.adjust_timer( timer );
                }
                } else {
                    users[sockfd].close_conn();
                    if( bytes < 0 )
                    {
                        // 如果发生读错误，则关闭连接，并移除其对应的定时器
                        if( errno != EAGAIN )
                        {
                            cb_func( &users[sockfd] );
                            if( timer )
                            {
                                timer_lst.del_timer( timer );
                            }
                        }
                    }
                    else if( bytes == 0 )
                    {
                        // 如果对方已经关闭连接，则我们也关闭连接，并移除对应的定时器。
                        cb_func( &users[sockfd] );
                        if( timer )
                        {
                            timer_lst.del_timer( timer );
                        }
                    }
                }
            }
            else if(events[i].events & EPOLLOUT)
            { // 当检测到文件描述符可写时，写入服务器的响应
                if(!users[sockfd].write()){ // 一次性写完所有数据
                    users[sockfd].close_conn();
                }
            } 
        }

        // 最后处理定时事件，因为I/O事件有更高的优先级。当然，这样做将导致定时任务不能精准的按照预定的时间执行。
        if( timeout ) {
            timer_handler();
            timeout = false;
        }
    }

    close(epollfd);
    close( listenfd );
    close( pipefd[1] );
    close( pipefd[0] );
    delete [] users;
    delete pool;
    return 0;
}
