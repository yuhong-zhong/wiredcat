#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <wiredtiger.h>

#define BUFFER_SIZE 4096

#define NR_ENTRY 2000000l
#define VALUE_LEN 8

using namespace std;

struct entry {
	mutex lock;
	uint8_t value[VALUE_LEN];
} table[NR_ENTRY];
WT_CONNECTION *conn;

void generate_random_string(uint8_t *buffer, int size, unsigned int *seedp) {
	for (int i = 0; i < size - 1; ++i) {
		buffer[i] = 'a' + (rand_r(seedp) % ('z' - 'a' + 1));
	}
	buffer[size - 1] = '\0';
}

void read_thread_fn() {
	WT_SESSION *session;
	WT_CURSOR *cursor;
	int ret;
	unsigned int seed = 'r';

	ret = conn->open_session(conn, NULL, "isolation=read-uncommitted", &session);
	if (ret != 0) {
		printf("read open_session failed, ret: %s\n", wiredtiger_strerror(ret));
		exit(1);
	}
	ret = session->open_cursor(session, "lsm:karaage", NULL, NULL, &cursor);
	if (ret != 0) {
		printf("read open_cursor, ret: %s\n", wiredtiger_strerror(ret));
		exit(1);
	}

	char *key_buffer = (char *) malloc(BUFFER_SIZE);
	char *value;
	while (true) {
		long key = rand_r(&seed) % NR_ENTRY;
		table[key].lock.lock();

		sprintf(key_buffer, "%ld", key);
		cursor->set_key(cursor, key_buffer);
		ret = cursor->search(cursor);
		if (ret != 0) {
			printf("read cursor->search failed\n");
			exit(1);
		}
		ret = cursor->get_value(cursor, &value);
		if (ret != 0) {
			printf("read cursor->get_value failed\n");
			exit(1);
		}
		if (strcmp(value, (char *)table[key].value) != 0) {
			printf("read inconsistent value - got %s, expected %s\n", value, (char *)table[key].value);
			exit(1);
		}
		table[key].lock.unlock();
	}
}

void write_thread_fn() {
	WT_SESSION *session;
	WT_CURSOR *cursor;
	int ret;
	unsigned int seed = 'w';

	ret = conn->open_session(conn, NULL, "isolation=read-uncommitted", &session);
	if (ret != 0) {
		printf("write open_session failed, ret: %s\n", wiredtiger_strerror(ret));
		exit(1);
	}
	ret = session->open_cursor(session, "lsm:karaage", NULL, NULL, &cursor);
	if (ret != 0) {
		printf("write open_cursor, ret: %s\n", wiredtiger_strerror(ret));
		exit(1);
	}

	char *key_buffer = (char *) malloc(BUFFER_SIZE);
	while (true) {
		long key = rand_r(&seed) % NR_ENTRY;
		table[key].lock.lock();

		sprintf(key_buffer, "%ld", key);
		generate_random_string(table[key].value, VALUE_LEN, &seed);
		cursor->set_key(cursor, key_buffer);
		cursor->set_value(cursor, table[key].value);
		ret = cursor->update(cursor);
		if (ret != 0) {
			printf("write cursor->update failed\n");
			exit(1);
		}
		table[key].lock.unlock();
	}
}

int main(int argc, char *argv[]) {
	/* space amplification */
	WT_SESSION *session;
	WT_CURSOR *cursor;
	int ret;

	ret = wiredtiger_open("/users/yuhong/tigerhome", NULL,
			      "create,direct_io=[data,checkpoint],buffer_alignment=512B,mmap=false,"
			      "verbose=[backup,compact,compact_progress,"
			      "log,history_store,history_store_activity,"
			      "overflow,rebalance,rts,salvage,"  // lsm,lsm_manager,transaction
			      "shared_cache,temporary,timestamp,verify]",
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

	/* populate the database */
	char *key_buffer = (char *) malloc(BUFFER_SIZE);
	unsigned int seed = 'm';
	for (long i = 0; i < NR_ENTRY; ++i) {
		sprintf(key_buffer, "%ld", i);
		generate_random_string(table[i].value, VALUE_LEN, &seed);
		cursor->set_key(cursor, key_buffer);
		cursor->set_value(cursor, table[i].value);
		ret = cursor->insert(cursor);
		if (ret != 0) {
			printf("cursor->insert failed\n");
			exit(1);
		}
	}
	printf("finished populating database\n");
	session->close(session, NULL);

	/* start read & write thread */
	thread read_thread = thread(read_thread_fn);
	thread write_thread = thread(write_thread_fn);
	read_thread.join();
	write_thread.join();
	ret = conn->close(conn, NULL);
	return (ret);
}
