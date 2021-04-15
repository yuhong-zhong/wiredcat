#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <wiredtiger.h>

#define BUFFER_SIZE 4096

int main(int argc, char *argv[]) {
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	const char *key, *value;
	int ret;
	long i;

	ret = wiredtiger_open("/users/yuhong/tigerhome", NULL,
	                      "create,direct_io=[data],buffer_alignment=512B,mmap=false,"
	                      "verbose=[backup,compact,compact_progress,"
	                      "log,history_store,history_store_activity,"
	                      "lsm,lsm_manager,overflow,rebalance,rts,salvage,"
	                      "shared_cache,temporary,timestamp,transaction,verify]",
	                      &conn);
	if (ret != 0) {
		printf("wiredtiger_open failed, ret: %s\n", wiredtiger_strerror(ret));
		exit(1);
	}
	ret = conn->open_session(conn, NULL, NULL, &session);
	if (ret != 0) {
		printf("open_session failed, ret: %s\n", wiredtiger_strerror(ret));
		exit(1);
	}
	ret = session->create(session, "lsm:karaage", "key_format=S,value_format=S,"
	                                              "allocation_size=512B,internal_page_max=512B,leaf_page_max=512B,");
	if (ret != 0) {
		printf("create failed, ret: %s\n", wiredtiger_strerror(ret));
		exit(1);
	}
	ret = session->open_cursor(session, "lsm:karaage", NULL, NULL, &cursor);
	if (ret != 0) {
		printf("open_cursor, ret: %s\n", wiredtiger_strerror(ret));
		exit(1);
	}
	char *key_buffer = (char *) malloc(BUFFER_SIZE);
	char *value_buffer = (char *) malloc(BUFFER_SIZE);
	for (i = 0; i < 2000000l; ++i) {
		sprintf(key_buffer, "key%08ld", i);
		sprintf(value_buffer, "value%08ld", i);
		cursor->set_key(cursor, key_buffer);
		cursor->set_value(cursor, value_buffer);
		cursor->insert(cursor);
	}
	printf("finished populating database\n");
	cursor->reset(cursor);
	for (i = 0; i < 2000000l; ++i) {
		sprintf(key_buffer, "key%08ld", i);
		sprintf(value_buffer, "value%08ld", i);
		cursor->set_key(cursor, key_buffer);
		ret = cursor->search(cursor);
		if (ret != 0) {
			printf("search failed\n");
			continue;
		}
		cursor->get_value(cursor, &value);
		if (strcmp(value, value_buffer) != 0) {
			printf("inconsistent value: %s (expected %s)\n", value, value_buffer);
		}
	}
	printf("finished iterating database\n");
	ret = conn->close(conn, NULL);
	return (ret);
}
