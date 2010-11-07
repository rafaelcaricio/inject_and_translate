#include <stdio.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/queue.h>
#include <string.h>
#include <stdlib.h>
#include <err.h>
#include <event.h>
#include <evhttp.h>
#include <libmemcached/memcached.h>
#include <mysql.h>


/*
 * Global 
 * gcc -o v_auth_and_cache -levent -I/usr/include/mysql  -DBIG_JOINS=1  -fno-strict-aliasing   -DUNIV_LINUX -DUNIV_LINUX verify_auth_and_cache.c -Wl,-Bsymbolic-functions -rdynamic -L/usr/lib/mysql -lmysqlclient -lmemcached
 */
typedef struct {
    char* host;
    int port;
} MServer;

int DEBUG = 0;
int NUM_OF_SERVERS = 0;
MServer memcache_servers[4];
memcached_st *tcp_client;

// mysql configs
MYSQL *conn;
char *server = "localhost";
char *user = "root";
char *password = "somepassword";
char *database = "somedb";

/*
 * Memcached info
 */
void add_mserver(char* host, int port) {
    memcache_servers[NUM_OF_SERVERS].host = host;
    memcache_servers[NUM_OF_SERVERS].port = port;
    NUM_OF_SERVERS++;
}

void init_memcache_servers() {
    add_mserver("127.0.0.1", 11211);
    //add_mserver("192.168.0.51", 11211);

    //Add to memcached client
    tcp_client = memcached_create(NULL);

    int i;
    for(i=0; i < NUM_OF_SERVERS; i++) {
        memcached_server_add(tcp_client, 
                             memcache_servers[i].host,
                             memcache_servers[i].port); 
    }
}


/*
 * Takes an `uri` and strips the query arguments.
 */
char* parse_path(const char* uri) {
	char c, *ret;
	int i, j, in_query = 0;

	ret = malloc(strlen(uri) + 1);

	for (i = j = 0; uri[i] != '\0'; i++) {
		c = uri[i];
		if (c == '?') {
            break;
        }
        else {
		    ret[j++] = c;
        }
    }
    ret[j] = '\0';

    return ret;
}

/*
 * Checks if the user session is active and if user is 
 * loggedin at this session.
 *
 */
int is_loggedin(struct evhttp_request *req, void *arg) {
    const char* cookie_value = NULL;
    char cookies[1024];

    // verifica se existe o cabecalho de cookie 
    if ((cookie_value = evhttp_find_header(req->input_headers, "Cookie")) != NULL) {

        if (strlen(cookie_value) < 1024) {

            sprintf(cookies, "%s", cookie_value);

            char* key = strtok(cookies, ";=");
            while (key != NULL) {
                if (strcmp(key, "sessionid") == 0) {
                    MYSQL_ROW row;
                    MYSQL_RES *res;
                    char query[256];
                    char *session_data_select = "SELECT session_data FROM django_session WHERE session_key = '%s'";

                    conn = mysql_init(NULL);

                    if (!mysql_real_connect(conn, server,
                        user, password, database, 0, NULL, 0)) {
                        fprintf(stderr, "%s\n", mysql_error(conn));
                        return 1;
                    }

                    key = strtok(NULL, ";=");
                    
                    fprintf(stderr, "the session key is: '%s'\n", key);
                    
                    sprintf(query, session_data_select, key);
                    
                    if (mysql_query(conn, query)) {
                        fprintf(stderr, "%s\n", mysql_error(conn));
                        mysql_close(conn);
                        return 0;
                    }

                    res = mysql_use_result(conn);

                    if ((row = mysql_fetch_row(res)) != NULL) {
                        fprintf(stderr, "session data is: '%s'\n", row[0]);

                        // verifica se eh um usuario autenticado pelo tamanho dos dados na sessao.
                        if (strlen(row[0]) > 60) {
                            mysql_free_result(res);
                            mysql_close(conn);
                            fprintf(stderr, "usuario autenticado!\n");
                            return 1;
                        }
                    }
                    /* close connection */
                    mysql_free_result(res);
                    mysql_close(conn);
                }
                key = strtok(NULL, ";=");
            }
        }
    }

    return 0;
}


/*
 * Checks if the path is in memcached, if not
 * the connection gets dropped without an answer
 * this is done so a proxy redirection 
 * can be dropped in nginx.
 */
void memcache_handler(struct evhttp_request *req, void *arg) {
    struct evbuffer *buf;
    buf = evbuffer_new();

    if (buf == NULL) {
        err(1, "failed to create response buffer");
    }

    if (is_loggedin(req, arg) == 0) {

        char key[200];

        strcpy(key, "atepassar:");

        char *request_uri = parse_path(req->uri);


        //Ensure no buffer overflows
        if(strlen(request_uri) > 125) {
            evhttp_connection_free(req->evcon);
        }
        else {
            strcat(key, request_uri);
            strcat(key, ":");

            /* Fetch from memcached */
            memcached_return rc;

            char *cached;
            size_t string_length;
            uint32_t flags;

            cached = memcached_get(tcp_client, key, strlen(key),
                                   &string_length, &flags, &rc);

            /* Return a result to the client */
            if(cached) {
                evhttp_add_header(req->output_headers, "Server", "AtePassarServer/0.0.1");
                evhttp_add_header(req->output_headers, "Content-Type", "text/html; charset=UTF-8");
                evbuffer_add_printf(buf, "%s", cached);
                evhttp_send_reply(req, HTTP_OK, "Client", buf);
                free(cached);
            }
            else {
                if (DEBUG) {
                    evbuffer_add_printf(buf, "Tentou achar a chave: %s", key);
                    evhttp_send_reply(req, HTTP_OK, "Client", buf);
                }
                else {
                    evhttp_connection_free(req->evcon);
                }
            }
        }

        free(request_uri);
    }
    else {
        if (DEBUG) {
            evbuffer_add_printf(buf, "Usuario estah autenticado!");
            evhttp_send_reply(req, HTTP_OK, "Client", buf);
        }
        else {
            evhttp_connection_free(req->evcon);
        }
    }

    evbuffer_free(buf);
}


int main(int argc, char **argv) {
    init_memcache_servers();

    struct evhttp *httpd;
    
    printf("Starting service...\n");

    event_init();
    httpd = evhttp_start(argv[1], atoi(argv[2]));

    evhttp_set_gencb(httpd, memcache_handler, NULL);

    event_dispatch();

    printf("Service started!\n");

    evhttp_free(httpd);
    memcached_free(tcp_client);
    return 0;
}
