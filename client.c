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
#include <sys/stat.h>

#include <zlib.h>

#include "webd.h"
#include "threadlist.h"
#include "client.h"

#define ST_DONE 1
#define ST_ERR -1

volatile int line_number = 0;

void rq_free(struct rq * rq) {
	if(rq->path) {
		free(rq->path);
		rq->path = NULL;
	}
	if(rq->syspath) {
		free(rq->syspath);
		rq->syspath = NULL;
	}
	if(rq->read_buf) {
		free(rq->read_buf);
		rq->read_buf = NULL;
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

int rq_200(struct rq * rq, int socket_fd, FILE * file, char * mimetype, int length, char * encoding) {
	char * buffer = malloc(length + 2);
	size_t read_size = fread(buffer, 1, length + 2, file);
	if(encoding) {
		char * response =
			"HTTP/%d.%d 200 OK\r\n"
			"Content-Type: %s\r\n"
			"Content-Length: %d\r\n"
			"content-Encoding: %s\r\n"
			"Connection: close\r\n"
			"\r\n";
		dprintf(socket_fd, response, rq->pv_maj, rq->pv_min, mimetype, read_size, encoding);
	} else {
		char * response =
			"HTTP/%d.%d 200 OK\r\n"
			"Content-Type: %s\r\n"
			"Content-Length: %d\r\n"
			"Connection: close\r\n"
			"\r\n";
		dprintf(socket_fd, response, rq->pv_maj, rq->pv_min, mimetype, read_size);
	}
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
		"Connection: close\r\n"
		"\r\n%s";
	dprintf(socket_fd, response, rq->pv_maj, rq->pv_min, strlen(msg), msg);

	close(socket_fd);

	return 0;
}

int rq_500(struct rq * rq, int socket_fd, char * msg) {
	char * response =
		"HTTP/%d.%d 500 Internal Server Error\r\n"
		"Content-Type: text/plain\r\n"
		"Content-Length: %d\r\n"
		"Connection: close\r\n"
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

int is_directory(char * path) {
	if(str_ends_with(path, "/")) {
		return 1;
	} else {
		struct stat stat_info;
		if(stat(path, &stat_info) != 0) {
			return 0;
		} else {
			return S_ISDIR(stat_info.st_mode);
		}
	}
}

char * rq_compress(struct rq * rq, FILE ** file) {
	const int CHUNK = 1 << 16;
	FILE * source = *file;
	if((rq->supported_compression & RQ_COMP_DEFLATE) == RQ_COMP_DEFLATE) {
		int flush, have;
		z_stream strm;
		unsigned char in[CHUNK], out[CHUNK];

		printf("compress\n");

		/* allocate deflate state */
		strm.zalloc = Z_NULL;
		strm.zfree = Z_NULL;
		strm.opaque = Z_NULL;
		if(deflateInit(&strm, Z_DEFAULT_COMPRESSION) != Z_OK)
			goto err_0;

		*file = tmpfile();

		do {
			strm.avail_in = fread(in, 1, CHUNK, source);
			if (ferror(source)) {
				deflateEnd(&strm);
				goto err_1;
			}
			flush = feof(source) ? Z_FINISH : Z_NO_FLUSH;
			strm.next_in = in;
			
			do {
				strm.avail_out = CHUNK;
				strm.next_out = out;
				deflate(&strm, flush);

				have = CHUNK - strm.avail_out;
				if (fwrite(out, 1, have, *file) != have || ferror(*file)) {
					deflateEnd(&strm);
					goto err_1;
				}
			} while(strm.avail_out == 0);
		} while(flush != Z_FINISH);

		deflateEnd(&strm);
		fclose(source);
		return "deflate";
err_1:
		fclose(*file);
		rewind(source);
		*file = source;
err_0:
		return NULL;
	} else {
		return NULL;
	}
}

int rq_handle(struct rq * rq, int socket_fd) {
	int result = 0;
	
	if(rq->syspath) {
		char * path_rel = calloc(strlen(config->http_root) + strlen(rq->syspath) + 1, sizeof(char)), * path_abs, * server_error = NULL;
		strcpy(path_rel, config->http_root);
		strcat(path_rel, rq->syspath);
		if((path_abs = realpath(path_rel, NULL)) == NULL) {
			server_error = strerror(errno);
		} else {
			if(is_directory(path_abs)) {
				int append_slash = !str_ends_with(path_abs, "/");
				path_abs = realloc(path_abs, strlen(path_abs) + strlen(config->index_file) + (append_slash ? 1 : 0) + 1);
				if(append_slash) {
					strcat(path_abs, "/");
				}
				strcat(path_abs, config->index_file);
			}
		}
		free(path_rel);

		if(server_error) {
			result |= rq_500(rq, socket_fd, server_error);
		} else if(access(path_abs, F_OK) != -1) {
			FILE * f = fopen(path_abs, "r");
			char * encoding = rq_compress(rq, &f);
			int file_len;
			fseek(f, 0L, SEEK_END);
			file_len = ftell(f);
			rewind(f);
			result |= rq_200(rq, socket_fd, f, rq_mime_type(path_abs), file_len, encoding);
			fclose(f);
		} else {
			result |= rq_404(rq, socket_fd, "File not found");
		}

		free(path_abs);
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
			if(strstr(rest, "gzip") != NULL) rq->supported_compression |= RQ_COMP_GZ;
			if(strstr(rest, "deflate") != NULL) rq->supported_compression |= RQ_COMP_DEFLATE;
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
			rq->read_buf = realloc(rq->read_buf, line_size);
		}

		rq->read_buf[line_index++] = c;

		if(c == '\n' && line_index >= 2 && rq->read_buf[line_index - 2] == '\r') {
			int result;
			rq->read_buf[line_index] = '\0';
			result = rq_line(rq, rq->read_buf, t->socket_fd);
			
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

	rq_free(rq);
	t->data = NULL;

	return success ? NULL : NULL;
}
