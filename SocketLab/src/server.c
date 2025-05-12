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
 #include <pthread.h>
 #include <stdatomic.h>
 #include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
 #include <unistd.h>

 #include "config.h"
 #include "llama/simple_llama_chat.h"
 #include "json/simple_json.h"

 __attribute__((unused)) static _Atomic int client_num = 0;
 static _Atomic int user_id_counter = 0;  // 用于生成唯一用户 ID

 void send_token(int conn, const char *token) {
     // TODO: convert the token with a specific format
     // 将 token 封装为 JSON 格式
     char json_str[MAX_BUFFER_SIZE];
     snprintf(json_str, sizeof(json_str), "{\"token\": \"%s\", \"eog\": false}", token);

     // TODO: send `message` to the client with file descriptor `conn`
     // 通过 socket 发送 JSON 数据到客户端
     if (send(conn, json_str, strlen(json_str), 0) < 0) {
         perror("send failed");
     }
 }

 void send_eog_token(int conn) {
     // TODO: convert the token with a specific format
     // 发送结束标记的 JSON 格式
     const char *json_str = "{\"token\": \"\", \"eog\": true}";

     // TODO: send `message` to the client with file descriptor `conn`
     // 通过 socket 发送结束标记到客户端
     if (send(conn, json_str, strlen(json_str), 0) < 0) {
         perror("send failed");
     }
 }

 static void set_socket_reusable(int socket) {
     // allow the socket to be reused immediately after the server is closed
     int opt = 1;
     if (setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
         perror("setsockopt");
         close(socket);
         exit(EXIT_FAILURE);
     }
 }

 static void bind_socket_addr_port(int socket, const char *addr, int port) {
     // TODO: bind the server socket to the specified IP address and port
     // 将服务器 socket 绑定到指定的 IP 地址和端口
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
     // 开始监听传入的连接请求
     if (listen(socket, 5) < 0) {
         perror("listen failed");
         close(socket);
         exit(EXIT_FAILURE);
     }
 }

 /********************** we need to create threads here **********************/

 void *handle_client(void *arg) {
    int conn = *(int *)arg;
    free(arg);

    // 生成唯一的 user_name
    char user_name[32];
    snprintf(user_name, sizeof(user_name), "user_%d", atomic_fetch_add(&user_id_counter, 1));

    // 为每个用户创建唯一的JSON对象名
    char json_obj_name[64];
    snprintf(json_obj_name, sizeof(json_obj_name), "request_%s", user_name);
    int request_created = 0;

    // 注册用户
    if (add_chat_user(user_name) != 0) {
        fprintf(stderr, "Failed to add chat user: %s\n", user_name);
        close(conn);
        return NULL;
    }

    printf("Client connected: %s\n", user_name);

    char buffer[MAX_BUFFER_SIZE];

    while (1) {
        int len = recv(conn, buffer, MAX_BUFFER_SIZE - 1, 0);
        if (len <= 0) {
            if (len == 0) {
                printf("Client disconnected: %s\n", user_name);
            } else {
                perror("recv failed");
            }
            break;
        }
        buffer[len] = '\0';

        // 如果已经创建了请求对象，先销毁它
        if (request_created) {
            destroy_json_object(json_obj_name);
            request_created = 0;
        }

        // 创建新的请求对象
        create_json_object(json_obj_name);
        request_created = 1;

        parse_json(json_obj_name, buffer);
        const char *token = get_json_value_str(json_obj_name, "token");
        if (token == NULL) {
            fprintf(stderr, "Failed to get token from request\n");
            destroy_json_object(json_obj_name);
            request_created = 0;
            continue;
        }

        printf("Received ('%s'): %s\n", user_name, buffer);

        // 调用 quest_for_response 处理请求
        int result = quest_for_response(conn, user_name, token);
        if (result != 0) {
            fprintf(stderr, "Failed to generate response for user %s\n", user_name);
        }

        destroy_json_object(json_obj_name);
        request_created = 0;
    }

    // 确保JSON对象被销毁
    if (request_created) {
        destroy_json_object(json_obj_name);
    }

    // 客户端断开后移除用户
    remove_chat_user(user_name);
    close(conn);
    return NULL;
}

 /********************** we need to create threads here **********************/

 int main(int argc, char *argv[]) {
     // initialize llama.cpp
     if (initialize_llama_chat(argc, argv) != 0) {
         fprintf(stderr, "Failed to initialize llama server.\n");
         return 1;
     }

     // TODO: create a socket file descriptor for the server socket
     // 创建服务器 socket
     int server_socket = socket(AF_INET, SOCK_STREAM, 0);
     if (server_socket < 0) {
         perror("socket creation failed");
         exit(EXIT_FAILURE);
     }


     set_socket_reusable(server_socket);

     // bind the socket to an address and port and start listening
     bind_socket_addr_port(server_socket, SERVER_ADDR, SERVER_PORT);
     listen_socket(server_socket);
     printf("The server has been started, listening on: %s:%d\n", SERVER_ADDR,
            SERVER_PORT);

     while (1) {
         // TODO: accept a new incoming connection
         // THINK: why we need to allocate memory on the heap for
         //        the client socket
         // 接受新的客户端连接，动态分配内存以在线程间传递
         struct sockaddr_in client_addr;
         socklen_t client_len = sizeof(client_addr);
         int *client_socket = (int *)malloc(sizeof(int));
         if (client_socket == NULL) {
             perror("malloc failed");
             continue;
         }
         *client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);
         if (*client_socket < 0) {
             perror("accept failed");
             free(client_socket);
             continue;
         }

         // TODO: create a new thread (`handle_client`) to handle the client
         //       you may need to invoke `pthread_detach` to avoid memory leaks
         // 创建线程处理客户端请求并分离线程
         pthread_t client_thread;
         if (pthread_create(&client_thread, NULL, handle_client, (void *)client_socket) != 0) {
             perror("pthread_create failed");
             close(*client_socket);
             free(client_socket);
             continue;
         }
         pthread_detach(client_thread); // 分离线程，避免内存泄漏
     }

     // close the server socket
     close(server_socket);

     // free llama.cpp resources
     free_llama_chat();
     return 0;
 }