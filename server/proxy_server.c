#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <signal.h>


enum {
	CATEGORIES_COUNT = 17,
	MAX_PROJECT_SIZE = 16384,
	BUFFER_OUT_READ_SIZE = 150,		// Must be bigger than any permitted user request
	READ_SOCKETS_COUNT = 1000			// How many client sockets read simultaneously
};


struct user{
	short int min_money, max_money;		// Money range, in rubbles
	unsigned short int categories[CATEGORIES_COUNT];
	short int without_money;			// Is client accept projects without specified budget
	short int last_project, last_project_id;
	int hash;		// User verification, to prevent situation than someone get your projects and get nothing
};
struct project {
	char str[MAX_PROJECT_SIZE];	// JSON with all projects data
	unsigned short int strlen;
	unsigned short int money, id;
	unsigned short int categories[CATEGORIES_COUNT];
};
struct read_socket {
	int fd;
	char data[BUFFER_OUT_READ_SIZE];
	unsigned short int data_len;
};
struct link{
	// Element of linked list with pointer to socket
	unsigned short int id; // Socket id
	struct link *prev, *next;
};

// This variable must be global to provide access to them from SIG_PIPE handle
int current_socket_ind = -1;
struct read_socket read_sockets[READ_SOCKETS_COUNT];
struct link free_read_sockets_chain[READ_SOCKETS_COUNT], *last_free_read_socket, *first_free_read_socket;
int read_socket_max_fd = -1;
fd_set master_out_read_set, master_out_write_set;

void error(char *msg)
{
    perror(msg);
}
void client_error()
{
    printf("error\n");
}

void remove_socket(int i) {
	if(current_socket_ind >= 0) {
		int fd = read_sockets[i].fd;
		int j;
		read_sockets[i].fd = -1;
		// Remove socket
		close(fd);
		FD_CLR(fd, &master_out_read_set);
		if(read_socket_max_fd == fd){
			read_socket_max_fd = -1;
			for(j = 0; j < READ_SOCKETS_COUNT; j++){
				if(read_sockets[j].fd > read_socket_max_fd) read_socket_max_fd = read_sockets[j].fd;
			}
		}
		// Put freed socket to beginning
		if((&free_read_sockets_chain[i] != first_free_read_socket) && (&free_read_sockets_chain[i] != last_free_read_socket)){
			(*free_read_sockets_chain[i].next).prev = free_read_sockets_chain[i].prev;
			(*free_read_sockets_chain[i].prev).next = free_read_sockets_chain[i].next;
			free_read_sockets_chain[i].next = first_free_read_socket;
			free_read_sockets_chain[i].prev = NULL;
			(*first_free_read_socket).prev = &free_read_sockets_chain[i];
			first_free_read_socket = &free_read_sockets_chain[i];
		}
		if(&free_read_sockets_chain[i] == last_free_read_socket){
			// If socket not in the middle of the chain, but in the end
			(*last_free_read_socket).next = first_free_read_socket;
			(*first_free_read_socket).prev = last_free_read_socket;
			first_free_read_socket = last_free_read_socket;
			last_free_read_socket = (*first_free_read_socket).prev;
			(*first_free_read_socket).prev = NULL;
			(*last_free_read_socket).next = NULL;
		}
	}
}

void sig_pipe_handler(int signum) {
	remove_socket(current_socket_ind);
	current_socket_ind = -1;
}

int main(int argc, char *argv[])
{
	int PORT_IN = 23142;
	int PORT_OUT = 23143;
	int USERS_COUNT = 1000;		// Information about USERS_COUNT users we will store in memory
	int PROJECTS_COUNT = 100;
	int PROJECTS_TO_ONE_USER_COUNT = 30;	// Only PROJECTS_TO_ONE_USER_COUNT projects you can get in one connection
	char *client_end_of_the_string = "&";
	
	unsigned short int bool;
	int len, i, j, k, counter, id, hash, sock, error_id, is_slash_was_last, count_projects_in_buffer, handle;
	int buffer_in_len = 0;
	int user_size = sizeof(struct user);
	struct timespec req = {0},rem = {0};
	req.tv_sec = 0;
	req.tv_nsec=1000L;
	
	
	for(i = 0; i < READ_SOCKETS_COUNT; i++) read_sockets[i].fd = -1;
	first_free_read_socket = &free_read_sockets_chain[0];
	free_read_sockets_chain[0].id = 0;
	free_read_sockets_chain[0].prev = NULL;
	free_read_sockets_chain[0].next = &free_read_sockets_chain[1];
	for(i = 1; i < (READ_SOCKETS_COUNT-1); i++){
		free_read_sockets_chain[i].id = i;
		free_read_sockets_chain[i].prev = &free_read_sockets_chain[i-1];
		free_read_sockets_chain[i].next = &free_read_sockets_chain[i+1];
	}
	last_free_read_socket = &free_read_sockets_chain[READ_SOCKETS_COUNT-1];
	free_read_sockets_chain[READ_SOCKETS_COUNT-1].id = READ_SOCKETS_COUNT-1;
	free_read_sockets_chain[READ_SOCKETS_COUNT-1].prev = &free_read_sockets_chain[READ_SOCKETS_COUNT-2];
	free_read_sockets_chain[READ_SOCKETS_COUNT-1].next = NULL;
	
	
	
	int user_projects[PROJECTS_TO_ONE_USER_COUNT];
	int last_user_project;
	
	
	fd_set tmp_set;
	FD_ZERO(&tmp_set);
	FD_ZERO(&master_out_read_set);
	FD_ZERO(&master_out_write_set);
	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	char buffer_in[MAX_PROJECT_SIZE];
	unsigned int separators_buffer_in[PROJECTS_COUNT * 4];
	int separators_buffer_in_empty;
	int separators_buffer_in_start;
	char tmp_buffer[MAX_PROJECT_SIZE], *tmp;
	unsigned int position;
	
	struct user users[USERS_COUNT], tmp_user;
	struct project projects[PROJECTS_COUNT];
	//users[0].min_money = 0;
	//users[0].max_money = 0;
	//users[0].without_money = 1;
	//users[0].categories[0] = 1;
	//users[0].categories[1] = 1;
	//users[0].last_project = PROJECTS_COUNT;
	//users[0].last_project_id = 0;
	//users[0].hash = 12345;
	int first_user = USERS_COUNT-1;
	int last_project = PROJECTS_COUNT-1;
	//for(i = 0; i < 17; i++) users[0].categories[i] = 1;
	
	signal(SIGPIPE, sig_pipe_handler);
	
	
	struct sockaddr_in serv_in_l, serv_in, serv_out_l, serv_out;
	int sockfd_in_l, sockfd_in, sockfd_out;
	socklen_t serv_in_len, serv_out_len;
	
	sockfd_in_l = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd_in_l < 0)
        error("ERROR opening IN socket");
	bzero((char *) &serv_in_l, sizeof(serv_in_l));
	
	serv_in_l.sin_family = AF_INET;
	serv_in_l.sin_addr.s_addr = INADDR_ANY;
	serv_in_l.sin_port = htons(PORT_IN);
	if (bind(sockfd_in_l, (struct sockaddr *) &serv_in_l, sizeof(serv_in_l)) < 0)
		error("ERROR on binding");
	listen(sockfd_in_l,5);
	serv_in_len = sizeof(serv_in);
	sockfd_in = accept(sockfd_in_l, (struct sockaddr *) &serv_in, &serv_in_len);
	if (sockfd_in < 0)
		error("ERROR on accept");
	
	
	
	sockfd_out = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd_out < 0)
        error("ERROR opening OUT socket");
	bzero((char *) &serv_out_l, sizeof(serv_out_l));
	serv_out_l.sin_family = AF_INET;
	serv_out_l.sin_addr.s_addr = INADDR_ANY;
	serv_out_l.sin_port = htons(PORT_OUT);
	if (bind(sockfd_out, (struct sockaddr *) &serv_out_l, sizeof(serv_out_l)) < 0)
		error("ERROR on binding");
	listen(sockfd_out, 5);
	serv_out_len = sizeof(serv_out);
	
	
	while(1) {
		FD_ZERO(&tmp_set);
		FD_SET(sockfd_in, &tmp_set);
		if (select(sockfd_in+1, &tmp_set, NULL, NULL, &tv)){
			if(buffer_in_len == 0){
				is_slash_was_last = 0;
				count_projects_in_buffer = 0;
				separators_buffer_in_empty = 1;
				separators_buffer_in_start = 0;
				separators_buffer_in[0] = -1;
				buffer_in_len = recv(sockfd_in, buffer_in, MAX_PROJECT_SIZE, 0);
				for(i = 0; i < buffer_in_len; i++){
					switch(buffer_in[i]) {
						case '\\':
							is_slash_was_last = !is_slash_was_last;
							break;
						case '&':
							if(!is_slash_was_last) {
								separators_buffer_in[separators_buffer_in_empty++] = i;
							} else {
								is_slash_was_last = 0;
							}
							break;
						case ';':
							if(!is_slash_was_last) {
								separators_buffer_in[separators_buffer_in_empty++] = i;
								count_projects_in_buffer++;
							} else {
								is_slash_was_last = 0;
							}
							break;
						default:
							is_slash_was_last = 0;
					}
				}
			}else{
				len = recv(sockfd_in, tmp_buffer, MAX_PROJECT_SIZE-buffer_in_len, 0);
				for(i = 0; i < len; i++){
					switch(tmp_buffer[i]) {
						case '\\':
							is_slash_was_last = !is_slash_was_last;
							break;
						case '&':
							if(!is_slash_was_last) {
								separators_buffer_in[separators_buffer_in_empty++] = buffer_in_len+i;
							} else {
								is_slash_was_last = 0;
							}
							break;
						case ';':
							if(!is_slash_was_last) {
								separators_buffer_in[separators_buffer_in_empty++] = buffer_in_len+i;
								count_projects_in_buffer++;
							} else {
								is_slash_was_last = 0;
							}
							break;
						default:
							is_slash_was_last = 0;
					}
					buffer_in[buffer_in_len+i] = tmp_buffer[i];
				}
				buffer_in_len += len;
			}
			buffer_in[buffer_in_len] = '\0';
			tmp = buffer_in;
			if(count_projects_in_buffer > 0) {
				while(count_projects_in_buffer > 0){
					last_project = (last_project + 1) % PROJECTS_COUNT;
					
					len = separators_buffer_in[separators_buffer_in_start + 2] - separators_buffer_in[separators_buffer_in_start + 1] - 1; // Length of json part
					memcpy(projects[last_project].str, buffer_in + separators_buffer_in[separators_buffer_in_start + 1] + 1, len);
					
					projects[last_project].str[len] = '\0';
					projects[last_project].strlen = len;
					position = separators_buffer_in[separators_buffer_in_start] + 1;		// Skip stop symbol "&"
					sscanf(buffer_in + position, "%hd", &projects[last_project].id);
					position = separators_buffer_in[separators_buffer_in_start + 2] + 1;	// Skip stop symbol "&"
					sscanf(buffer_in + position, "%hd", &projects[last_project].money);
					position = separators_buffer_in[separators_buffer_in_start + 3] + 2;	// Skip stop symbol "&" and symbol "["
					tmp = buffer_in + position;
					sscanf(tmp, "%d", &len);
					tmp += 3;
					if(len >= 10) tmp += 1;
					for(i = 0; i < CATEGORIES_COUNT; i++) projects[last_project].categories[i] = 0;
					for(i = 0; i < len; i++){
						sscanf(tmp, "%d", &j);
						projects[last_project].categories[j] = 1;
						tmp += 3;
						if(j >= 10) tmp += 1;
					}
					count_projects_in_buffer--;
					separators_buffer_in_start += 4;
				}
				tmp = buffer_in + separators_buffer_in[separators_buffer_in_start] + 1;
				is_slash_was_last = 0;
				separators_buffer_in_empty = 1;
				separators_buffer_in_start = 0;
				separators_buffer_in[0] = -1;
				buffer_in_len = strlen(tmp);
				for(i = 0; i < buffer_in_len; i++){
					switch(tmp[i]) {
						case '\\':
							is_slash_was_last = !is_slash_was_last;
							break;
						case '&':
							if(!is_slash_was_last) {
								separators_buffer_in[separators_buffer_in_empty++] = i;
							} else {
								is_slash_was_last = 0;
							}
							break;
						default:
							is_slash_was_last = 0;
					}
					buffer_in[i] = tmp[i];
				}
			}
		}
		FD_ZERO(&tmp_set);
		FD_SET(sockfd_out, &tmp_set);
		while((select(sockfd_out+1, &tmp_set, NULL, NULL, &tv) > 0) && (FD_ISSET(sockfd_out, &tmp_set) != 0)){
			sock = accept(sockfd_out, (struct sockaddr *) &serv_out, &serv_out_len);
			if(sock >= 0){
				// First free socket
				id = (*first_free_read_socket).id;
				if(read_sockets[id].fd != -1){
					// If it was not free -- clean it
					close(read_sockets[id].fd);
					FD_CLR(read_sockets[id].fd, &master_out_read_set);
					if(read_socket_max_fd == read_sockets[id].fd){
						// Updating read_socket_max_fd
						read_socket_max_fd = -1;
						for(j = 0; j < READ_SOCKETS_COUNT; j++){
							if(read_sockets[j].fd > read_socket_max_fd) read_socket_max_fd = read_sockets[j].fd;
						}
					}
				}
				read_sockets[id].fd = sock;
				read_sockets[id].data_len = 0;
				// Throw used socket unto the end of chain
				(*first_free_read_socket).prev = last_free_read_socket;
				(*last_free_read_socket).next = first_free_read_socket;
				last_free_read_socket = first_free_read_socket;
				first_free_read_socket = (*last_free_read_socket).next;
				(*first_free_read_socket).prev = NULL;
				(*last_free_read_socket).next = NULL; 
				FD_SET(sock, &master_out_read_set);
				if(sock > read_socket_max_fd) read_socket_max_fd = sock;
			}
		}
		tmp_set = master_out_read_set;
		len = select(read_socket_max_fd+1, &tmp_set, NULL, NULL, &tv);
		if(len > 0){
			for(i = 0; i < READ_SOCKETS_COUNT; i++){
				if(FD_ISSET(read_sockets[i].fd, &tmp_set)){
					// Handling request from user
					handle = 0;
					current_socket_ind = i;
					if(read_sockets[i].data_len == 0){
						len = recv(read_sockets[i].fd, read_sockets[i].data, BUFFER_OUT_READ_SIZE, 0);
						read_sockets[i].data_len = len;
						for(k = 0; k < len; k++){
							if(read_sockets[i].data[k] == '&')
								handle = 1;
						}
					}else{
						len = recv(read_sockets[i].fd, tmp_buffer, BUFFER_OUT_READ_SIZE-read_sockets[i].data_len, 0);
						handle = 0;
						for(k = 0; k < len; k++){
							if(tmp_buffer[k] == '&')
								handle = 1;
							read_sockets[i].data[read_sockets[i].data_len+k] = tmp_buffer[k];
						}
						read_sockets[i].data_len += len;
					}
					if(handle){
						read_sockets[i].data_len--;		// Skip end of the string symbol
						read_sockets[i].data[read_sockets[i].data_len] = '\0';
						error_id = 1;	// Wrong request
						if(read_sockets[i].data_len > 5 && read_sockets[i].data_len <= 12){
							// "Old" user
							error_id = 0; // Everything is good
							sscanf(read_sockets[i].data, "%d%d", &id, &hash);
							if((id >= 0) && (id < USERS_COUNT) && (hash == users[id].hash)){
								if(projects[users[id].last_project].id != users[id].last_project_id){
									// User last request was too long ago, we don't have so old projects
									users[id].last_project = PROJECTS_COUNT;	// Unattainable value -- means, that user need everything that we have have
								}
								counter = 0;
								if(users[id].last_project != last_project) {
									k = users[id].last_project;
									if(users[id].last_project == PROJECTS_COUNT) k = last_project;
									last_user_project = PROJECTS_TO_ONE_USER_COUNT-1;
									do{
										k = (k+1)%PROJECTS_COUNT;
										bool = 0;
										for(j = 0; j < CATEGORIES_COUNT; j++){
											if(projects[k].categories[j] == 1 && users[id].categories[j] == 1){
												bool = 1;
												break;
											}
										}
										if(bool == 0) continue;
										bool = 0;
										if(projects[k].money == 0){
											if(users[id].without_money == 1) bool = 1;
										}else{
											if(projects[k].money >= users[id].min_money){
												if(users[id].max_money == 0){
													bool = 1;
												}else{
													if(projects[k].money <= users[id].max_money) bool = 1;
												}
											}
										}
										if(bool == 0) continue;
										last_user_project = (last_user_project+1)%PROJECTS_TO_ONE_USER_COUNT;
										user_projects[last_user_project] = k;
										counter++;
									}while(k != last_project);
								}
								if(counter == 0){
									if (read_sockets[i].fd >= 0 && send(read_sockets[i].fd, client_end_of_the_string, 1, 0) < 0) {
										//error("ERROR writing to socket");
										remove_socket(read_sockets[i].fd);
										continue;
									}
								}else{
									if(counter <= PROJECTS_TO_ONE_USER_COUNT){
										for(j = 0; j < counter; j++){
											if(read_sockets[i].fd >= 0 && send(read_sockets[i].fd, projects[user_projects[j]].str, projects[user_projects[j]].strlen, 0) < 0) {
												//error("ERROR writing to socket");
												remove_socket(read_sockets[i].fd);
												continue;
											}
											//printf("%s%s\n\n", projects[user_projects[j]].str, client_end_of_the_string);
											if(read_sockets[i].fd >= 0 && send(read_sockets[i].fd, client_end_of_the_string, 1, 0) < 0) {
												//error("ERROR writing to socket");
												remove_socket(read_sockets[i].fd);
												continue;
											}
										}
									}else{
										last_user_project = (last_user_project+1)%PROJECTS_TO_ONE_USER_COUNT;
										j = last_user_project;
										do{
											if(read_sockets[i].fd >= 0 && send(read_sockets[i].fd, projects[user_projects[j]].str, projects[user_projects[j]].strlen, 0) < 0) {
												//error("ERROR writing to socket");
												remove_socket(read_sockets[i].fd);
												break;
											}
											if(read_sockets[i].fd >= 0 && send(read_sockets[i].fd, client_end_of_the_string, 1, 0) < 0) {
												//error("ERROR writing to socket");
												remove_socket(read_sockets[i].fd);
												break;
											}
											j = (j+1)%PROJECTS_TO_ONE_USER_COUNT;
										}while(j != last_user_project);
									}
								}
								if(read_sockets[i].fd >= 0 && send(read_sockets[i].fd, client_end_of_the_string, 1, 0) < 0) {
									//error("ERROR writing to socket");
									remove_socket(read_sockets[i].fd);
									continue;
								}
								//fprintf(fh, "%s", client_end_of_the_string);
								users[id].last_project = last_project;
								users[id].last_project_id = projects[users[id].last_project].id;
							}else{
								error_id = 2;	// Authorization error
								//printf("e:1|%d||%d||%d|\n", id, hash, users[id].hash);
							}
						}
						if(read_sockets[i].fd >= 0 && read_sockets[i].data_len > 12 && read_sockets[i].data_len <= BUFFER_OUT_READ_SIZE){
							// New user
							error_id = 0;	// Everything is good
							//printf("new user, '%s'", read_sockets[i].data);
							tmp_user.hash = -1;
							tmp_user.min_money = -1;
							tmp_user.max_money = -1;
							tmp_user.without_money = -1;
							tmp_user.last_project = PROJECTS_COUNT;
							tmp_user.last_project_id = -1;
							for(j = 0; j < CATEGORIES_COUNT; j++) tmp_user.categories[j] = 0;
							len = -1;
							sscanf(read_sockets[i].data, "%d%hd%hd%hd%hd%hd%d", &tmp_user.hash, &tmp_user.min_money, &tmp_user.max_money, &tmp_user.without_money, &tmp_user.last_project, &tmp_user.last_project_id, &len);
							if((tmp_user.hash != -1) && (tmp_user.min_money >= 0) && (tmp_user.max_money >= 0) && (tmp_user.without_money == 0 || tmp_user.without_money == 1) && (len > 0) && (len <= CATEGORIES_COUNT)){
								if(tmp_user.last_project > PROJECTS_COUNT || tmp_user.last_project < 0) tmp_user.last_project = PROJECTS_COUNT;
								bool = 1;
								tmp = strstr(read_sockets[i].data, "$")+2;
								for(k = 0; k < len; k++){
									j = -1;
									sscanf(tmp, "%d", &j);
									tmp += 2;
									if(j >= 10){
										tmp += 1;
									}
									if(j >= CATEGORIES_COUNT || j < 0){
										bool = 0;
										break;
									}
									tmp_user.categories[j] = 1;
								}
								if(bool == 1){
									first_user = (first_user+1)%USERS_COUNT;
									memcpy(&(users[first_user]), &tmp_user, user_size);
									sprintf(read_sockets[i].data, "%d%s%s", first_user, client_end_of_the_string, client_end_of_the_string);
									//printf("out: %s|%d||%d|\n", read_sockets[i].data, tmp_user.hash, users[first_user].hash);
									if(read_sockets[i].fd >= 0 && send(read_sockets[i].fd, read_sockets[i].data, strlen(read_sockets[i].data), 0) < 0) {
										//error("ERROR writing to socket");
										remove_socket(read_sockets[i].fd);
										continue;
									}
								}else{
									error_id = 1;	// Wrong request
									//client_error();
									//printf("e:4\n");
								}
							}else{
								error_id = 1;		// Wrong request
								//client_error();
								//printf("e:2\n");
							}
						}
						if(error_id == 1){
							if(read_sockets[i].fd >= 0) send(read_sockets[i].fd, "Wrong userid", 12, 0);
						}
						if(error_id == 2){
							if(read_sockets[i].fd >= 0) send(read_sockets[i].fd, "Wrong data", 10, 0);
						}
						remove_socket(i);
					}
					current_socket_ind = -1;
				}
			}
		}
		nanosleep(&req,&rem);
	}
	close(sockfd_in);
	close(sockfd_out);
	
	return 0; 
}
