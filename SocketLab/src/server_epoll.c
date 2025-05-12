/*
 * Copyright (c) 2025 Institute of Parallel And Distributed Systems (IPADS),
 * Shanghai Jiao Tong University (SJTU) Licensed under the Mulan PSL v2. You can
 * use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *     http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY
 * KIND, EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
 * NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE. See the
 * Mulan PSL v2 for more details.
 */

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "config.h"
#include "llama/simple_llama_chat.h"
#include "json/simple_json.h"

#define MAX_EVENTS 10
#define MAX_CONNECTIONS 1024

// 用于跟踪每个连接的状态（是否为首次响应）
static int first_response[MAX_CONNECTIONS] = {0};
static pthread_mutex_t conn_mutex = PTHREAD_MUTEX_INITIALIZER;

// 移除字符串中的所有粗体标记 **
void remove_bold_marks(const char *input, char *output) {
    int i = 0, j = 0;
    int in_bold = 0;

    while (input[i]) {
        // 检测是否遇到粗体标记 **
        if (input[i] == '*' && input[i+1] == '*') {
            in_bold = !in_bold;  // 切换粗体状态
            i += 2;  // 跳过这两个字符
        } else {
            output[j++] = input[i++];
        }
    }
    output[j] = '\0';  // 添加字符串结束符
}

void send_token(int conn, const char *token) {
    char json_str[MAX_BUFFER_SIZE];
    char processed_token[MAX_BUFFER_SIZE];

    // 检查连接状态并处理token
    pthread_mutex_lock(&conn_mutex);
    int is_first = (first_response[conn % MAX_CONNECTIONS] == 0);
    if (is_first) {
        // 首次响应，不修改标记 - 直到EOG才更新标记
        // 直接使用原始token（保留粗体标记）
        snprintf(json_str, sizeof(json_str), "{\"token\": \"%s\", \"eog\": false}", token);
    } else {
        // 非首次响应，移除粗体标记
        remove_bold_marks(token, processed_token);
        snprintf(json_str, sizeof(json_str), "{\"token\": \"%s\", \"eog\": false}", processed_token);
    }
    pthread_mutex_unlock(&conn_mutex);

    // 发送处理后的消息
    if (send(conn, json_str, strlen(json_str), 0) < 0) {
        perror("send failed");
    }
}

void send_eog_token(int conn) {
    // 发送EOG标记并更新连接状态
    const char *json_str = "{\"token\": \"\", \"eog\": true}";

    if (send(conn, json_str, strlen(json_str), 0) < 0) {
        perror("send failed");
    }

    // 更新该连接的状态
    pthread_mutex_lock(&conn_mutex);
    // 如果是首次响应，将其标记为非首次响应
    if (first_response[conn % MAX_CONNECTIONS] == 0) {
        first_response[conn % MAX_CONNECTIONS] = 1;
    }
    pthread_mutex_unlock(&conn_mutex);
}

static void set_socket_reusable(int socket) {
    int opt = 1;
    if (setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror("setsockopt");
        close(socket);
        exit(EXIT_FAILURE);
    }
}

static void bind_socket_addr_port(int socket, const char *addr, int port) {
    // TODO: bind the server socket to the specified IP address and port
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    inet_pton(AF_INET, addr, &serv_addr.sin_addr);

    if (bind(socket, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("bind failed");
        close(socket);
        exit(EXIT_FAILURE);
    }
}

static void listen_socket(int socket) {
    // TODO: listen for incoming connections
    if (listen(socket, 5) < 0) {
        perror("listen failed");
        close(socket);
        exit(EXIT_FAILURE);
    }
}

static void set_nonblocking(int fd) {
    // set the socket to non-blocking mode
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        perror("fcntl(F_GETFL)");
        exit(EXIT_FAILURE);
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl(F_SETFL)");
        exit(EXIT_FAILURE);
    }
}

static void add_to_epoll(int epoll_fd, int fd) {
    // TODO: add the file descriptor `fd` to the epoll instance `epoll_fd`
    //       for watching input events
    struct epoll_event event;
    event.events = EPOLLIN;  // 监听读取事件
    event.data.fd = fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) == -1) {
        perror("epoll_ctl: add");
        exit(EXIT_FAILURE);
    }
}

/********************** we need to create threads here **********************/

struct quest_args {
    int conn;
    char message[MAX_BUFFER_SIZE];
};

void *handle_response(void *arg) {
    // 处理客户端请求并生成响应
    struct quest_args *args = (struct quest_args *)arg;

    // 创建JSON对象并解析请求
    char json_obj_name[32];
    snprintf(json_obj_name, sizeof(json_obj_name), "request_%p", (void *)args);

    create_json_object(json_obj_name);
    parse_json(json_obj_name, args->message);

    // 提取token字段
    const char *token = get_json_value_str(json_obj_name, "token");
    if (token == NULL) {
        fprintf(stderr, "Failed to get token from request\n");
        destroy_json_object(json_obj_name);
        free(args);
        return NULL;
    }

    printf("Received request: %s\n", args->message);

    // 生成用户名称
    char user_name[32];
    snprintf(user_name, sizeof(user_name), "user_%p", (void *)args);

    // 添加聊天用户
    if (add_chat_user(user_name) != 0) {
        fprintf(stderr, "Failed to add chat user: %s\n", user_name);
        destroy_json_object(json_obj_name);
        free(args);
        return NULL;
    }

    // 使用quest_for_response生成回复
    int result = quest_for_response(args->conn, user_name, token);
    if (result != 0) {
        fprintf(stderr, "Failed to generate response\n");
    }

    // 清理资源
    destroy_json_object(json_obj_name);
    remove_chat_user(user_name);
    free(args);
    return NULL;
}

/********************** we need to create threads here **********************/

int main(int argc, char *argv[]) {
    // 初始化LLaMA模型
    if (initialize_llama_chat(argc, argv) != 0) {
        fprintf(stderr, "Failed to initialize llama server.\n");
        return 1;
    }

    // 创建服务器套接字
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    // 设置套接字可重用
    set_socket_reusable(server_socket);

    // 绑定地址和端口并开始监听
    bind_socket_addr_port(server_socket, SERVER_ADDR, SERVER_PORT);
    listen_socket(server_socket);
    printf("Server listening on %s:%d\n", SERVER_ADDR, SERVER_PORT);

    // 设置服务器套接字为非阻塞模式
    set_nonblocking(server_socket);

    // 创建epoll实例
    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll_create1");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    // 将服务器套接字添加到epoll实例
    add_to_epoll(epoll_fd, server_socket);

    // 事件循环
    struct epoll_event events[MAX_EVENTS];
    while (1) {
        // 等待epoll事件
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            // 检查是否是因为信号中断
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait");
            break;
        }

        // 处理所有就绪的事件
        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == server_socket) {
                // 处理新连接
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);

                if (client_socket == -1) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        perror("accept");
                    }
                    continue;
                }

                // 将新客户端套接字设为非阻塞
                set_nonblocking(client_socket);

                // 添加到epoll监听
                add_to_epoll(epoll_fd, client_socket);

                // 打印客户端连接信息
                char client_ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
                printf("New connection from %s:%d\n", client_ip, ntohs(client_addr.sin_port));
            } else {
                // 处理客户端数据
                int fd = events[i].data.fd;
                struct quest_args *args = malloc(sizeof(struct quest_args));
                if (!args) {
                    perror("malloc");
                    continue;
                }

                args->conn = fd;
                int len = recv(fd, args->message, MAX_BUFFER_SIZE - 1, 0);

                if (len <= 0) {
                    if (len == 0) {
                        printf("Client disconnected\n");
                    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        perror("recv");
                    }

                    // 重置连接状态
                    pthread_mutex_lock(&conn_mutex);
                    first_response[fd % MAX_CONNECTIONS] = 0;
                    pthread_mutex_unlock(&conn_mutex);

                    // 从epoll中移除
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                    free(args);
                    continue;
                }

                args->message[len] = '\0';

                // 创建线程处理响应
                pthread_t thread;
                if (pthread_create(&thread, NULL, handle_response, args) != 0) {
                    perror("pthread_create");
                    free(args);
                    continue;
                }

                // 分离线程，避免资源泄漏
                pthread_detach(thread);
            }
        }
    }

    // 关闭服务器套接字和epoll实例
    close(server_socket);
    close(epoll_fd);

    // 释放LLaMA资源
    free_llama_chat();
    return 0;
}
