#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <pthread.h>




enum {
	CATEGORIES_COUNT = 17,
	MAX_PROJECT_SIZE = 32767,
	BUFFER_OUT_READ_SIZE = 180,		// Must be bigger than any permitted user request
	READ_SOCKETS_COUNT = 1000,			// How many client sockets read simultaneously
	LISTEN_BACKLOG = 5,
	MAX_PROJECTS_COUNT = 100
};
const char SEMAPHORE_NAME[] = "/fviewer_projects_semaphore";
const char SHARED_MEMORY_OBJECT_NAME[] = "fviewer_projects_shared_memory";



struct _projects {
	int start;	// First project element in the array
	int end;
	int count;
	int now;	// Project used now by the user queries handler
	sem_t *sem;	// Semaphore for organized access to projects
	struct project *arr;
};

struct user{
	int min_money, max_money;		// Money range, in rubbles
	unsigned int categories[CATEGORIES_COUNT];
	int without_money;			// Is client accept projects without specified budget
	int last_project, last_project_id;
	int hash;		// User verification, to prevent situation than someone get your projects and get nothing
};

struct project {
	char str[MAX_PROJECT_SIZE + 1];	// JSON with all projects data
	unsigned int strlen;
	unsigned int money, id;
	unsigned int categories[CATEGORIES_COUNT];
};

struct read_socket {
	int fd;
	char data[BUFFER_OUT_READ_SIZE + 1];
	unsigned short int data_len;
};

struct link{
	// Element of linked list with pointer to socket
	unsigned short int id; // Socket id
	struct link *prev, *next;
};

// This variable must be global to provide access to them from SIG_PIPE handler or from getting_projects thread
int current_socket_ind = -1;
struct read_socket read_sockets[READ_SOCKETS_COUNT];
struct link free_read_sockets_chain[READ_SOCKETS_COUNT], *last_free_read_socket, *first_free_read_socket;
int read_socket_max_fd = -1;
fd_set master_out_read_set, master_out_write_set;
struct _projects projects;
int project_now_id = -1;

void error(char *msg)
{
    perror(msg);
}

void remove_socket(int i) {
	if(current_socket_ind >= 0) {
		int fd = read_sockets[i].fd;
		int j;
		struct link *currient;
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
		currient = &free_read_sockets_chain[i];
		// Put freed socket to beginning
		if((currient != first_free_read_socket) && (&free_read_sockets_chain[i] != last_free_read_socket)){
			currient->next->prev = currient->prev;
			currient->prev->next = currient->next;
			currient->next = first_free_read_socket;
			currient->prev = NULL;
			first_free_read_socket->prev = currient;
			first_free_read_socket = currient;
		}
		if(currient == last_free_read_socket){
			// If socket not in the middle of the chain, but in the end
			last_free_read_socket->next = first_free_read_socket;
			first_free_read_socket->prev = last_free_read_socket;
			first_free_read_socket = last_free_read_socket;
			last_free_read_socket = first_free_read_socket->prev;
			first_free_read_socket->prev = NULL;
			last_free_read_socket->next = NULL;
		}
	}
}

void sig_pipe_handler(int signum) {
	remove_socket(current_socket_ind);
	current_socket_ind = -1;
}

int not_escaped(char *str) {
	// Determines whether an even number of slashes in front of *str
	int slash = 0;
	int pos = 1;
	while(*(str - pos) == '\\') {
		slash = !slash;
		pos++;
	}
	return !slash;
}

void *getting_projects( void *arg) {
	/* 
	 * 
	 * Special thread for getting projects from another processes through shared memory
	 * 
	 */
	 
	sem_t *sem;
	int shm;
	char *new_project, *tmp, *next, *next_virgule;
	int current_id, i, cat_id, start_len, value;
	int new_project_id = 0;
	struct project *current;
	
	// Create shared memory object
	if ( (shm = shm_open(SHARED_MEMORY_OBJECT_NAME, O_CREAT|O_RDWR, 0777)) == -1 ) {
        error("shm_open");
    }
    
	if ( ftruncate(shm, MAX_PROJECT_SIZE + 1) == -1 ) {
		error("ftruncate");
	}
	
	new_project = mmap(0, MAX_PROJECT_SIZE+1, PROT_WRITE|PROT_READ, MAP_SHARED, shm, 0);
    if ( new_project == (char*)-1 ) {
        error("mmap");
    }
    
    //Create semaphore for the possibility of using shared memory asynchronous 
    if ( (sem = sem_open(SEMAPHORE_NAME, O_CREAT, 0777, 0)) == SEM_FAILED ) {
		perror("sem_open");
	}
	// Unlock semaphore. Default value is 1
	sem_getvalue(sem, &value);
	while(value > 1) {
		sem_wait(sem);
		value--;
	}
	while(value < 1) {
		sem_post(sem);
		value++;
	}
	
	// By default no data in shared memory
	new_project[0] = '\0';
	
	for(;;) {
		// Wait free shared memory and when -- lock it (rather lock semaphore)
		sem_wait(sem);
		if(new_project[0] == '{') {
			//printf("New project\n");
			// There are new project
			if(projects.count == MAX_PROJECTS_COUNT) {
				// Delete oldest project in list
				current_id = projects.start;
				sem_wait(projects.sem);
				while(projects.now == current_id)
					usleep(10);
				projects.start = (projects.start + 1) % MAX_PROJECTS_COUNT;
				projects.arr[current_id].id = -2; // To be shure that no one will get this project until we update it in full
				sem_post(projects.sem);
				projects.count--;
			} else if(projects.count == 0) {
				projects.end = -1;
				current_id = 0;
			} else {
				current_id = (projects.end + 1) % MAX_PROJECTS_COUNT;
			}
			current = &projects.arr[current_id];
			sprintf(current->str, "{\"id\": %d, \"loc_id\": %d, ", new_project_id, current_id);
			//new_project	++; // Skip '{' symbol
			start_len = strlen(current->str);
			current->strlen = strlen(new_project + 1); // '+ 1' to skip '{' symbol
			memcpy(current->str + start_len, new_project + 1, current->strlen);
			current->strlen += start_len;
			current->str[current->strlen] = '\0';
			//printf("%s\n\n", current->str);
			
			// Free shared memory
			new_project[0] = '\0';
			// Unlock shared memory
			sem_post(sem);
			
			// Parse json representation
			for(i = 0; i < CATEGORIES_COUNT; i++)
				current->categories[i] = 0;
			tmp = current->str;
			while((tmp = strchr(tmp, '"')) != NULL) {
				if(not_escaped(tmp)) {
					tmp++; // skip '"' symbol
					next = strchr(tmp, '"');
					if(next != NULL && next[1] == ':' && not_escaped(next)) {
						*next = '\0';
						if(strcmp(tmp, "money") == 0) {
							sscanf(next + 1, ":%d", &(current->money));
						} else if(strcmp(tmp, "categ") == 0) {
							tmp = strchr(next + 1, '[') + 1;
							next_virgule = tmp; // Fit any not NULL value
							while(next_virgule != NULL && sscanf(tmp, "%d", &cat_id)) {
								current->categories[cat_id] = 1;
								next_virgule = strchr(tmp, ',');
								if(next_virgule != NULL) {
									tmp = next_virgule + 1;
								}
							}
						}
						*next = '"';
						tmp = next + 1;
					}
				} else {
					tmp++; // Skip escapted qoute
				}
			}
			current->id = new_project_id++;
			projects.end = (projects.end + 1) % MAX_PROJECTS_COUNT;
			projects.count++;
		} else {
			// Unlock shared memory
			sem_post(sem);
		}
	}
}

int main(int argc, char *argv[])
{
	int PORT_OUT = 23143;
	int USERS_COUNT = 1000;		// Information about USERS_COUNT users we will store in memory
	int PROJECTS_TO_ONE_USER_COUNT = 30;	// Only PROJECTS_TO_ONE_USER_COUNT projects you can get in one connection
	
	int bool;
	int len, i, j, k, counter, id, hash, sock, error_id, handle, prev_len, cat_id;
	pthread_t thread;
	int user_size = sizeof(struct user);
	struct timespec req = {0},rem = {0};
	req.tv_sec = 0;
	req.tv_nsec = 1000L;
	
	
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
	char tmp_buffer[MAX_PROJECT_SIZE], *tmp, *next, *next_virgule, *next_bracket;
	
	struct user users[USERS_COUNT], tmp_user;
	projects.arr = (struct project *) malloc(sizeof(struct project) * MAX_PROJECTS_COUNT);
	projects.start = 0;
	projects.end = 0;
	projects.count = 0;
	projects.sem = (sem_t *) malloc(sizeof(sem_t));
    if (sem_init(projects.sem, 0, 1) < 0) {
		perror("sem_init");
	}
	
	//users[0].min_money = 0;
	//users[0].max_money = 0;
	//users[0].without_money = 1;
	//users[0].categories[0] = 1;
	//users[0].categories[1] = 1;
	//users[0].last_project = MAX_PROJECTS_COUNT;
	//users[0].last_project_id = 0;
	//users[0].hash = 12345;
	int first_user = USERS_COUNT-1;
	//int last_project = PROJECTS_COUNT-1;
	//for(i = 0; i < 17; i++) users[0].categories[i] = 1;
	
	signal(SIGPIPE, sig_pipe_handler);
	
	
	struct sockaddr_in serv_out_l, serv_out;
	int sockfd_out;
	socklen_t serv_out_len;
	
	
	
	sockfd_out = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd_out < 0) {
        error("ERROR opening OUT socket");
        return 1;
	}
	bzero((char *) &serv_out_l, sizeof(serv_out_l));
	serv_out_l.sin_family = AF_INET;
	serv_out_l.sin_addr.s_addr = INADDR_ANY;
	serv_out_l.sin_port = htons(PORT_OUT);
	if (bind(sockfd_out, (struct sockaddr *) &serv_out_l, sizeof(serv_out_l)) < 0) {
		error("ERROR on binding");
		return 1;
	}
	listen(sockfd_out, LISTEN_BACKLOG);
	serv_out_len = sizeof(serv_out);
	
	// Start getting_projects thread
	pthread_create(&thread, NULL, &getting_projects, NULL );
	
	
	while(projects.count == 0) {
		// Wait first projects
		sleep(1);
	}
	for(;;) {
		FD_ZERO(&tmp_set);
		FD_SET(sockfd_out, &tmp_set);
		while((select(sockfd_out+1, &tmp_set, NULL, NULL, &tv) > 0) && (FD_ISSET(sockfd_out, &tmp_set) != 0)) {
			sock = accept(sockfd_out, (struct sockaddr *) &serv_out, &serv_out_len);
			if(sock >= 0) {
				// First free socket
				id = (*first_free_read_socket).id;
				if(read_sockets[id].fd != -1) {
					// If it was not free -- clean it
					close(read_sockets[id].fd);
					FD_CLR(read_sockets[id].fd, &master_out_read_set);
					if(read_socket_max_fd == read_sockets[id].fd) {
						// Updating read_socket_max_fd
						read_socket_max_fd = -1;
						for(j = 0; j < READ_SOCKETS_COUNT; j++) {
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
			for(i = 0; i < READ_SOCKETS_COUNT; i++) {
				if(FD_ISSET(read_sockets[i].fd, &tmp_set)) {
					// Handling request from user
					handle = 0;
					current_socket_ind = i;
					if(read_sockets[i].data_len == 0) {
						len = recv(read_sockets[i].fd, read_sockets[i].data, BUFFER_OUT_READ_SIZE, 0);
						read_sockets[i].data_len = len;
						read_sockets[i].data[len] = '\0';
						if(strchr(read_sockets[i].data, '}')) {
							handle = 1;
						}
					} else {
						len = recv(read_sockets[i].fd, tmp_buffer, BUFFER_OUT_READ_SIZE - read_sockets[i].data_len, 0);
						handle = 0;
						prev_len = read_sockets[i].data_len;
						tmp_buffer[len] = '\0';
						if(strchr(tmp_buffer, '}')) {
							handle = 1;
						}
						memcpy(read_sockets[i].data + prev_len, tmp_buffer, len);
						read_sockets[i].data_len += len;
					}
					if(handle) {
						//printf(" %d\n", projects.count);
						read_sockets[i].data[read_sockets[i].data_len] = '\0';
						error_id = 1;	// Wrong request
						if(read_sockets[i].data_len >= 24 && read_sockets[i].data_len <= 30) {
							// "Old" user
							error_id = 0; // Everything is good
							tmp = read_sockets[i].data;
							id = -1;
							hash = -1;
							while((tmp = strchr(tmp, '"')) != NULL) {
								tmp++; // skip '"' symbol
								next = strchr(tmp, '"');
								if(next != NULL && next[1] == ':') {
									*next = '\0';
									if(strcmp(tmp, "id") == 0) {
										sscanf(next + 1, ":%d", &id);
									} else if(strcmp(tmp, "hash") == 0) {
										sscanf(next + 1, ":%d", &hash);
									}
									*next = '"';
								}
								tmp = next + 1;
							}
							
							if((id >= 0) && (id < USERS_COUNT) && (hash == users[id].hash)) {
								sem_wait(projects.sem);
								projects.now = users[id].last_project + 1;
								if(projects.arr[users[id].last_project].id != users[id].last_project_id){
									// User last request was too long ago, we don't have so old projects
									projects.now = projects.start;
									users[id].last_project = projects.start - 1;
								}
								sem_post(projects.sem);
								counter = 0;
								//printf("t %d %d\n", users[id].last_project, projects.end);
								if(users[id].last_project != projects.end) {
									k = users[id].last_project;
									last_user_project = PROJECTS_TO_ONE_USER_COUNT - 1;
									do{
										k = (k + 1) % MAX_PROJECTS_COUNT;
										//printf("k %d\n", k);
										bool = 0;
										for(j = 0; j < CATEGORIES_COUNT; j++) {
											if(projects.arr[k].categories[j] == 1 && users[id].categories[j] == 1){
												bool = 1;
												break;
											}
										}
										if(bool == 0) continue;
										//printf("12\n");
										bool = 0;
										if(projects.arr[k].money == 0) {
											if(users[id].without_money == 1) bool = 1;
										} else {
											if(projects.arr[k].money >= users[id].min_money) {
												if(users[id].max_money == 0) {
													bool = 1;
												} else {
													if(projects.arr[k].money <= users[id].max_money) bool = 1;
												}
											}
										}
										if(bool == 0) continue;
										//printf("13\n");
										last_user_project = (last_user_project + 1) % PROJECTS_TO_ONE_USER_COUNT;
										user_projects[last_user_project] = k;
										counter++;
										//printf("2\n");
									} while(k != projects.end);
								}
								if(read_sockets[i].fd >= 0 && send(read_sockets[i].fd, "[", 1, 0) < 0) {
									//error("ERROR writing to socket");
									remove_socket(i);
									continue;
								}
								if(counter > 0) {
									if(counter <= PROJECTS_TO_ONE_USER_COUNT) {
										j = PROJECTS_TO_ONE_USER_COUNT - 1;
										last_user_project = counter - 1;
									} else {
										j = last_user_project;	
									}
									do{
										j = (j + 1) % PROJECTS_TO_ONE_USER_COUNT;
										if(read_sockets[i].fd >= 0 && send(read_sockets[i].fd, projects.arr[user_projects[j]].str, projects.arr[user_projects[j]].strlen, 0) < 0) {
											//error("ERROR writing to socket");
											remove_socket(i);
											break;
										}
										if(j != last_user_project && read_sockets[i].fd >= 0 && send(read_sockets[i].fd, ", ", 2, 0) < 0) {
											//error("ERROR writing to socket");
											remove_socket(i);
											break;
										}
									} while(j != last_user_project);
								}
								if(read_sockets[i].fd >= 0 && send(read_sockets[i].fd, "]", 1, 0) < 0) {
									//error("ERROR writing to socket");
									remove_socket(i);
									continue;
								}
								users[id].last_project = user_projects[last_user_project];
								users[id].last_project_id = projects.arr[users[id].last_project].id;
							}else{
								error_id = 2;	// Authorization error
							}
						}
						if(read_sockets[i].fd >= 0 && read_sockets[i].data_len > 30 && read_sockets[i].data_len <= BUFFER_OUT_READ_SIZE){
							// New user
							error_id = 0;	// Everything is good
							//printf("new user, '%s'", read_sockets[i].data);
							tmp_user.hash = -1;
							tmp_user.min_money = -1;
							tmp_user.max_money = -1;
							tmp_user.without_money = -1;
							tmp_user.last_project = MAX_PROJECTS_COUNT;
							tmp_user.last_project_id = -1;
							for(j = 0; j < CATEGORIES_COUNT; j++) tmp_user.categories[j] = 0;
							len = -1;
							
							
							bool = 1;
							tmp = read_sockets[i].data;
							while(bool && (tmp = strchr(tmp, '"')) != NULL) {
								tmp++; // Skip '"' symbol
								next = strchr(tmp, '"');
								if(next != NULL && next[1] == ':') {
									*next = '\0';
									if(strcmp(tmp, "hash") == 0) {
										sscanf(next + 1, ":%d", &tmp_user.hash);
									} else if(strcmp(tmp, "min_mon") == 0) {
										sscanf(next + 1, ":%d", &tmp_user.min_money);
									} else if(strcmp(tmp, "max_mon") == 0) {
										sscanf(next + 1, ":%d", &tmp_user.max_money);
									} else if(strcmp(tmp, "wth_mon") == 0) {
										sscanf(next + 1, ":%d", &tmp_user.without_money);
									} else if(strcmp(tmp, "last_proj") == 0) {
										sscanf(next + 1, ":%d", &tmp_user.last_project);
									} else if(strcmp(tmp, "last_proj_id") == 0) {
										sscanf(next + 1, ":%d", &tmp_user.last_project_id);
									} else if(strcmp(tmp, "categ") == 0) {
										next_bracket = strchr(next + 1, '[');
										if(next_bracket != NULL) {
											tmp = next_bracket + 1;
											next_virgule = tmp; // Fit any not NULL value
											while(next_virgule != NULL && sscanf(tmp, "%d", &cat_id)) {
												if(cat_id >= CATEGORIES_COUNT || cat_id < 0){
													bool = 0;
													break;
												}
												tmp_user.categories[cat_id] = 1;
												next_virgule = strchr(tmp, ',');
												if(next_virgule != NULL)
													tmp = next_virgule + 1;
											}
										}
									}
									*next = '"';
								}
								tmp = next + 1;
							}
							
							if(tmp_user.hash == -1)
								bool = 0;
							if(tmp_user.min_money < 0 || tmp_user.max_money < 0)
								bool = 0;
							if(tmp_user.without_money != 0 && tmp_user.without_money != 1)
								bool = 0;
							
							if(!bool) {
								error_id = 1;	// Wrong request
							} else {
								if(tmp_user.last_project >= MAX_PROJECTS_COUNT || tmp_user.last_project < 0) {
									tmp_user.last_project = 0;
									tmp_user.last_project_id = -1;
								}
								first_user = (first_user + 1) % USERS_COUNT;
								memcpy(users + first_user, &tmp_user, user_size);
								sprintf(read_sockets[i].data, "{\"id\": %d}", first_user);
								if(read_sockets[i].fd >= 0 && send(read_sockets[i].fd, read_sockets[i].data, strlen(read_sockets[i].data), 0) < 0) {
									//error("ERROR writing to socket");
									remove_socket(i);
									continue;
								}
							}
						}
						if(error_id == 1){
							if(read_sockets[i].fd >= 0) send(read_sockets[i].fd, "{\"error\": \"Wrong data\"}", 23, 0);
						}
						if(error_id == 2){
							if(read_sockets[i].fd >= 0) send(read_sockets[i].fd, "{\"error\": \"Wrong userid\"}", 25, 0);
						}
						remove_socket(i);
					} else {
						if(read_sockets[i].data_len == BUFFER_OUT_READ_SIZE)
							remove_socket(i);
					}
					current_socket_ind = -1;
				}
			}
		}
		nanosleep(&req,&rem);
	}
	close(sockfd_out);
	
	return 0; 
}
