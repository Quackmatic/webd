#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "webd.h"
#include "threadlist.h"
#include "client.h"

#define ST_DONE 1
#define ST_ERR -1

volatile int line_number = 0;

void rq_free(struct rq * rq) {
	if(rq->path) {
		free(rq->path);
	}
	if(rq->syspath) {
		free(rq->syspath);
	}

	free(rq);
}

void rq_thread_cleanup(void * data) {
	rq_free((struct rq *)data);
}

int rq_parse_method(struct rq * rq, char * line, char ** rest) {
	while(**rest) {
		if(**rest == ' ') {
			if(*rest - line == 3 && strncmp(line, "GET", 3) == 0) {
				rq->method = RQ_METHOD_GET;
			} else {
				rq->method = RQ_METHOD_UNSUPPORTED;
			}

			(*rest)++;

			return 0;
		}
		(*rest)++;
	}

	return -1;
}

int rq_parse_path(struct rq * rq, char * line, char ** rest) {
	while(**rest) {
		if(**rest == ' ') {
			int i = 0, j = 0, len = 1 + (*rest - line);
			rq->path = malloc(len);
			rq->syspath = malloc(len);
			memcpy(rq->path, line, len);

			for(i = 0; i < len - 1; i++) {
				char c = line[i];

				if(c == '%' && i + 2 < len - 1) {
					char code[3] = {line[++i], line[++i], 0};
					c = (char)(strtol(code, NULL, 16) & 0xFF);
				}

				rq->path[j++] = c;
			}

			rq->path[j] = '\0';
			len = j;
			j = 0;
			for(i = 0; i < len; i++) {
				char c = line[i];

				if(c == '?') {
					break;
				} else if(c == '/' && i >= 2 && line[i - 2] == '.' && line[i - 1] == '.') {
					j = -1;
					break;
				}

				rq->syspath[j++] = c;
			}

			if(j > -1) {
				rq->syspath[j] = '\0';
			} else {
				free(rq->syspath);
				rq->syspath = NULL;
			}

			(*rest)++;

			return 0;
		}
		(*rest)++;
	}

	return -1;
}

int rq_is_hdr(char * header_name, char * line, char ** rest) {
	int result, hnl = strlen(header_name);
	char * compare_buf = malloc(hnl + 3);

	memcpy(compare_buf, header_name, hnl);
	compare_buf[hnl] = ':';
	compare_buf[hnl + 1] = ' ';
	compare_buf[hnl + 2] = '\0';

	if((result = (strncmp(compare_buf, line, hnl + 2) == 0))) {
		*rest = line + hnl + 2;
	}
	free(compare_buf);
	return result;
}

int rq_200(struct rq * rq, int socket_fd, FILE * file, char * mimetype, int length) {
	char * buffer = malloc(length + 2), * response =
		"HTTP/%d.%d 200 OK\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %d\r\n"
		"\r\n";
	size_t read_size = fread(buffer, 1, length + 2, file);
	dprintf(socket_fd, response, rq->pv_maj, rq->pv_min, mimetype, read_size);
	send(socket_fd, buffer, read_size, MSG_NOSIGNAL);
	free(buffer);

	close(socket_fd);

	return 0;
}

int rq_404(struct rq * rq, int socket_fd, char * msg) {
	char * response =
		"HTTP/%d.%d 404 Not Found\r\n"
		"Content-Type: text/plain\r\n"
		"Content-Length: %d\r\n"
		"\r\n%s";
	dprintf(socket_fd, response, rq->pv_maj, rq->pv_min, strlen(msg), msg);

	close(socket_fd);

	return 0;
}

int str_ends_with(char * str, char * end) {
	int str_len = strlen(str), end_len = strlen(end);
	return strncmp(str + (str_len - end_len), end, end_len) == 0;
}

char * rq_mime_type(char * path) {
	if(str_ends_with(path, ".html")) return "text/html";
	if(str_ends_with(path, ".txt")) return "text/plain";
	if(str_ends_with(path, ".css")) return "text/css";
	if(str_ends_with(path, ".js")) return "application/javascript";
	if(str_ends_with(path, ".jpg") || str_ends_with(path, ".jpeg")) return "image/jpeg";
	if(str_ends_with(path, ".png")) return "image/png";
	if(str_ends_with(path, ".gif")) return "image/gif";

	return "application/octet-stream";
}

int rq_handle(struct rq * rq, int socket_fd) {
	int result = 0;
	
	if(rq->syspath) {
		char * fname = calloc(strlen(config->http_root) + strlen(rq->syspath) + 1, 1);
		strcpy(fname, config->http_root);
		strcat(fname, rq->syspath);

		if(access(fname, F_OK) != -1) {
			FILE * f = fopen(fname, "r");
			int file_len;
			fseek(f, 0L, SEEK_END);
			file_len = ftell(f);
			rewind(f);
			result |= rq_200(rq, socket_fd, f, rq_mime_type(rq->syspath), file_len);
			fclose(f);
		} else {
			result |= rq_404(rq, socket_fd, "File not found");
		}

		free(fname);
	} else {
		result |= rq_404(rq, socket_fd, "Illegal directory traversal in path");
	}
	result |= ST_DONE;

	return result;
}

int rq_line(struct rq * rq, char * line, int socket_fd) {
	rq->line_recv++;
	if(rq->line_recv == 1) {
		char * rest = line;
		if(rq_parse_method(rq, rest, &rest) == 0 &&
		   rq_parse_path(rq, rest, &rest) == 0 &&
		   strncmp(rest, "HTTP/", 5) == 0 &&
		   (rq->pv_maj = (char)(strtol(rest + 5, &rest, 10) & 0xFF), rest[0] == '.') &&
		   (rq->pv_min = (char)(strtol(rest + 1, &rest, 10) & 0xFF), rest[0] == '\r')) {
			return 0;
		} else {
			return -1;
		}
	} else if(line[0] == '\r') {
		return rq_handle(rq, socket_fd);
	} else {
		char * rest = line;

		if(rq_is_hdr("Accept-Encoding", rest, &rest)) {
			if(strtok(rest, "gzip")) rq->supported_compression |= RQ_COMP_GZ;
			if(strtok(rest, "deflate")) rq->supported_compression |= RQ_COMP_DEFLATE;
		}

		return 0;
	}
}

void * thread_client(void * arg) {
	struct thread_list * t = (struct thread_list *)arg;
	int success = 1, read_count, index = -1, line_index = 0, line_size = 0;
	struct rq * rq = calloc(1, sizeof(struct rq));
	char socket_buf[128];

	t->data = rq;
	t->cleanup = &rq_thread_cleanup;

	rq->path = NULL;
	rq->syspath = NULL;
	rq->line_recv = 0;

	while(1) {
		char c;

		if(index == -1 || index == read_count) {
			index = 0;

			if((read_count = read(t->socket_fd, socket_buf, 128)) == -1) {
				if(t->thread_running) {
					perror("Could not read from socket");
					success = 0;
				}
				break;
			} else if(read_count == 0) {
				break;
			}
		}

		c = socket_buf[index];

		if(line_index + 1 >= line_size) {
			line_size += 256;
			t->read_buf = realloc(t->read_buf, line_size);
		}

		t->read_buf[line_index++] = c;

		if(c == '\n' && line_index >= 2 && t->read_buf[line_index - 2] == '\r') {
			int result;
			t->read_buf[line_index] = '\0';
			result = rq_line(rq, t->read_buf, t->socket_fd);
			
			if((result & ST_ERR) == ST_ERR) {
				success = 0;
				break;
			}
			if((result & ST_DONE) == ST_DONE) {
				break;
			}

			line_index = 0;
		}

		fflush(stdin);
		
		index += 1;
	}
	free(t->read_buf);
	t->read_buf = NULL;

	rq_free(rq);
	t->data = NULL;

	return success ? NULL : NULL;
}
