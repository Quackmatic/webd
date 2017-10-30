#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <zlib.h>

#include "webd.h"
#include "threadlist.h"
#include "client.h"

#define ST_DONE 1 << 1
#define ST_ERR 1 << 4

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
			} else if(*rest - line == 4 && strncmp(line, "POST", 4) == 0) {
				rq->method = RQ_METHOD_POST;
			} else if(*rest - line == 4 && strncmp(line, "HEAD", 4) == 0) {
				rq->method = RQ_METHOD_HEAD;
			} else if(*rest - line == 7 && strncmp(line, "OPTIONS", 7) == 0) {
				rq->method = RQ_METHOD_OPTIONS;
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
				} else if(c == '.' && i >= 2 && line[i - 2] == '/' && line[i - 1] == '.') {
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

void rq_write_date(int socket_fd) {
	char date[64];
	time_t now = time(0);
	strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S %Z", gmtime(&now));
	dprintf(socket_fd, "Date: %s\n", date);
}

int rq_200(struct rq * rq, int socket_fd, FILE * file, char * mimetype, int length, char * encoding) {
	dprintf(socket_fd, "HTTP/%d.%d 200 OK\r\n", rq->pv_maj, rq->pv_min);
	rq_write_date(socket_fd);
	dprintf(socket_fd, "Connection: Close\r\n");
	dprintf(socket_fd, "Content-Length: %d\r\n", length);
	if(rq->method == RQ_METHOD_OPTIONS) {
		dprintf(socket_fd, "Allow: GET, POST, HEAD, OPTIONS\r\n");
		dprintf(socket_fd, "\r\n");
	} else {
		dprintf(socket_fd, "Content-Type: %s\r\n", mimetype);
		if(encoding != NULL)
			dprintf(socket_fd, "Content-Encoding: %s\r\n", encoding);
		dprintf(socket_fd, "\r\n");

		if(rq->method != RQ_METHOD_HEAD) {
			int read;
			const int buf_size = 4096;
			char buffer[buf_size];

			while((read = fread(buffer, 1, buf_size, file)) > 0) {
				send(socket_fd, buffer, read, MSG_NOSIGNAL);
			}
		}
	}
	close(socket_fd);
	return 0;
}

int rq_xxx(struct rq * rq, int socket_fd, char * method, char * msg) {
	dprintf(socket_fd, "HTTP/%d.%d %s\r\n", rq->pv_maj, rq->pv_min, method);
	rq_write_date(socket_fd);
	dprintf(socket_fd, "Connection: Close\r\n");
	dprintf(socket_fd, "Content-Type: text/plain\r\n");
	dprintf(socket_fd, "Content-Length: %d\r\n", (int)strlen(msg));
	dprintf(socket_fd, "\r\n%s", msg);
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
					goto err_1;
				}
			} while(strm.avail_out == 0);
		} while(flush != Z_FINISH);

		deflateEnd(&strm);
		fclose(source);
		return "deflate";
err_1:
		deflateEnd(&strm);
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
	
	if(rq->method == RQ_METHOD_OPTIONS) {
		result |= rq_200(rq, socket_fd, NULL, NULL, 0, NULL);
	} else if(rq->method == RQ_METHOD_UNSUPPORTED) {
		result |= rq_xxx(rq, socket_fd, "405 Method Not Allowed", "Method not allowed");
	} else {
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
				result |= rq_xxx(rq, socket_fd, "500 Internal Server Error", server_error);
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
				result |= rq_xxx(rq, socket_fd, "404 Not Found", "File not found");
			}

			free(path_abs);
		} else {
			result |= rq_xxx(rq, socket_fd, "400 Bad Request", "Illegal directory traversal in path");
		}
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
