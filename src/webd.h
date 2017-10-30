#ifndef SERVER_H
#define SERVER_H

struct webd_config {
	char * http_root;
	char * index_file;
	uint16_t port;
	int print_help;
	int daemon;
};

struct webd_config * config;

int config_init(int argc, char * argv[]);
void config_free();

void daemon_init();

void signal_handler(int signal);

#endif
