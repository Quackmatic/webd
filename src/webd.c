#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pthread.h>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "webd.h"
#include "threadlist.h"
#include "client.h"

struct webd_config * config;
volatile int fd_server;

void signal_handler(int signal) {
	thread_list_clean();
	close(fd_server);
	config_free();
	exit(0);
}

void config_free() {
	free(config);
}

int config_init(int argc, char * argv[]) {
	int i, success = 1;

	config = malloc(sizeof(struct webd_config));
	config->http_root = ".";
	config->index_file = "index.html";
	config->port = 80;
	config->print_help = 0;

	for(i = 1; i < argc; i++) {
		if(strcmp(argv[i], "-?") == 0 || strcmp(argv[i], "--help") == 0) {
			config->print_help = 1;
		} else if(strcmp(argv[i], "-I") == 0 || strcmp(argv[i], "--index") == 0) {
			if(i + 1 < argc) {
				config->index_file = argv[i + 1];
				i += 1;
			} else {
				fprintf(stderr, "Must provide filename after flag -I\n");
				success = 0;
				break;
			}
		} else if(strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--root") == 0) {
			if(i + 1 < argc) {
				config->http_root = argv[i + 1];
				i += 1;
			} else {
				fprintf(stderr, "Must provide path after flag -r\n");
				success = 0;
				break;
			}
		} else if(strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
			if(i + 1 < argc) {
				long port = strtol(argv[i + 1], NULL, 10);
				if(port < 1 || port >= 1 << 16) {
					fprintf(stderr, "First argument must be valid port\n");
					success = 0;
					break;
				} else {
					config->port = (uint16_t)(port & 0xFFFF);
				}
				i += 1;
			} else {
				fprintf(stderr, "Must provide port number after flag -p\n");
				success = 0;
				break;
			}
		}
	}
	return success ? 0 : -1;
}

int main(int argc, char * argv[]) {
	int fd_client;
	struct sockaddr_in ep = {0};

	if(config_init(argc, argv) == -1) {
		goto exit_0;
	}

	if(config->print_help) {
		printf("webd v1.0.0\n");
		printf("Flags:\n");
		printf("(-? | --help)           : Prints this info\n");
		printf("(-p | --port) PORT      : Specify port number\n");
		printf("(-r | --root) DIR       : Specify root directory to serve files from\n");
		printf("(-I | --index) FILENAME : Specify default index filename to use\n");
		goto exit_0;
	}
	if(signal(SIGINT, signal_handler) == SIG_ERR ||
 			  signal(SIGTERM, signal_handler) == SIG_ERR) {
 		fprintf(stderr, "Cannot register signal handler\n");
		goto exit_0;
	}

	ep.sin_family = AF_INET;
	ep.sin_port = htons(config->port);
	ep.sin_addr.s_addr = INADDR_ANY;

	if((fd_server = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		perror("Cannot open socket");
		goto exit_1;
	}
	if(bind(fd_server, (struct sockaddr*)&ep, sizeof(ep)) < 0) {
		perror("Cannot bind socket");
		goto exit_1;
	}
	if(listen(fd_server, 5) == -1) {
		perror("Cannot open socket for listening");
		goto exit_1;
	}

	while(1) {
		if((fd_client = accept(fd_server, NULL, NULL)) < 0) {
			perror("Cannot open client socket");
			goto exit_1;
		}
		if(thread_list_create(fd_client, thread_client) == -1) {
			perror("Could not create thread");
			goto exit_1;
		}
	}

exit_1:
	close(fd_server);
exit_0:
	thread_list_clean();
	config_free();
	return 0;
}
