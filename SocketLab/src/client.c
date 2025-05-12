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

 #include "config.h"
 #include "json/simple_json.h"
 #include <arpa/inet.h>
 #include <netinet/in.h>
 #include <signal.h>
 #include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
 #include <sys/socket.h>
 #include <unistd.h>

 #define MAX_BUFFER_SIZE 1024

 int client_fd = -1;

 /* Signal handler for SIGINT */
 void sigint_handler(int sig) {
     if (client_fd != -1) {
         close(client_fd);
     }
     exit(0);
 }

 /* Connect to the server */
 int connect_to_server() {
     client_fd = socket(AF_INET, SOCK_STREAM, 0);
     if (client_fd < 0) {
         fprintf(stderr, "Socket creation failed\n");
         exit(1);
     }

     struct sockaddr_in server_addr;
     server_addr.sin_family = AF_INET;
     server_addr.sin_port = htons(SERVER_PORT);
     server_addr.sin_addr.s_addr = inet_addr(SERVER_ADDR);

     if (connect(client_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
         fprintf(stderr, "Connection failed\n");
         close(client_fd);
         exit(1);
     }

     fprintf(stdout, "Connected to %s:%d\n", SERVER_ADDR, SERVER_PORT);
     return client_fd;
 }

 int main() {
     /* Register SIGINT handler */
     signal(SIGINT, sigint_handler);

     /* Connect to server */
     int fd = connect_to_server();

     char input[MAX_BUFFER_SIZE];
     while (1) {
         /* Get user input */
         printf("> ");
         fflush(stdout);
         if (!fgets(input, MAX_BUFFER_SIZE, stdin)) {
             continue;
         }
         input[strcspn(input, "\n")] = 0; /* Remove newline */
         if (strlen(input) == 0) {
             continue;
         }

         /* Create JSON request */
         char json_str[MAX_BUFFER_SIZE];
         snprintf(json_str, sizeof(json_str), "{\"token\": \"%s\", \"eog\": true}", input);

         /* Send JSON string without extra newline */
         if (write(fd, json_str, strlen(json_str)) < 0) {
             perror("Write failed");
             close(fd);
             exit(1);
         }

         /* Receive and process responses */
         char buffer[MAX_BUFFER_SIZE];
         char response[MAX_BUFFER_SIZE] = {0};
         int len;
         while ((len = recv(fd, buffer, MAX_BUFFER_SIZE - 1, 0)) > 0) {
             buffer[len] = '\0';

             /* Parse the response */
             create_json_object("response");
             parse_json("response", buffer);
             const char *token = get_json_value_str("response", "token");
             if (token) {
                 strncat(response, token, MAX_BUFFER_SIZE - strlen(response) - 1);
             } else {
                 fprintf(stderr, "Failed to get token\n");
             }

             /* Get eog as boolean */
             int eog = get_json_value_bool("response", "eog");
             destroy_json_object("response");

             if (eog) {
                 break;
             }
         }
         printf("%s\n", response);
     }

     close(fd);
     return 0;
 }