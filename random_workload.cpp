#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <chrono>
#include <wiredtiger.h>

#define TIGER_HOME "/users/yuhong/tigerhome"
#define TIGER_TABLE_NAME "lsm:karaage"
#define TIGER_OPEN_CONN_CONFIG "create,direct_io=[data],buffer_alignment=512B,mmap=false"
#define TIGER_OPEN_SESSION_CONFIG "isolation=read-uncommitted"
#define TIGER_CREATE_TABLE_CONFIG "key_format=S,value_format=S,allocation_size=512B,internal_page_max=512B,leaf_page_max=512B"

#define BUFFER_SIZE 4096

using namespace std;
using namespace std::chrono;

enum thread_type {
	INIT,
	WORKLOAD,
};

struct thread_context {
	thread thread_v;
	thread_type type;
	int thread_index;

	long nr_entry;
	long value_size;

	WT_CONNECTION *conn;
};

struct init_thread_context : public thread_context {
	long start_key;
	long end_key;
	volatile long cur_key;
};

struct workload_thread_context : public thread_context {
	int nr_thread;
	float read_ratio;
	long nr_op;

	volatile long nr_read;
	volatile long nr_write;
	steady_clock::time_point start_time;
	steady_clock::time_point end_time;
	volatile bool finished;
};

enum {
	NR_ENTRY = 1,
	VALUE_SIZE,
	NR_THREAD,
	READ_RATIO,
	NR_OP,
};

void get_key_string(char *buffer, long key) {
	sprintf(buffer, "%016ld", key);
}

void get_random_string(char *buffer, int size, unsigned int *seedp) {
	for (int i = 0; i < size - 1; ++i) {
		buffer[i] = 'a' + (rand_r(seedp) % ('z' - 'a' + 1));
	}
	buffer[size - 1] = '\0';
}

void init_thread_fn(struct init_thread_context *context) {
	/* open session & cursor */
	WT_SESSION *session;
	WT_CURSOR *cursor;
	int ret;
	ret = context->conn->open_session(context->conn, NULL, NULL, &session);
	if (ret != 0) {
		printf("init thread %d open_session failed, ret: %s\n", context->thread_index, wiredtiger_strerror(ret));
		exit(1);
	}
	ret = session->open_cursor(session, TIGER_TABLE_NAME, NULL, "bulk", &cursor);
	if (ret != 0) {
		printf("init thread %d open_cursor, ret: %s\n", context->thread_index, wiredtiger_strerror(ret));
		exit(1);
	}

	/* populate database */
	unsigned int seed = context->thread_index + 'i';
	char *key_buffer = (char *) malloc(BUFFER_SIZE);
	char *value_buffer = (char *) malloc(BUFFER_SIZE);
	for (long i = context->start_key; i < context->end_key; ++i, ++context->cur_key) {
		get_key_string(key_buffer, i);
		get_random_string(value_buffer, context->value_size, &seed);
		cursor->set_key(cursor, key_buffer);
		cursor->set_value(cursor, value_buffer);
		ret = cursor->insert(cursor);
		if (ret != 0) {
			printf("init thread %d insert failed, ret: %s\n", context->thread_index, wiredtiger_strerror(ret));
			exit(1);
		}
	}
	session->close(session, NULL);
}

void workload_thread_fn(struct workload_thread_context *context) {
	/* open session & cursor */
	WT_SESSION *session;
	WT_CURSOR *cursor;
	int ret;
	ret = context->conn->open_session(context->conn, NULL, TIGER_OPEN_SESSION_CONFIG, &session);
	if (ret != 0) {
		printf("workload thread %d open_session failed, ret: %s\n", context->thread_index, wiredtiger_strerror(ret));
		exit(1);
	}
	ret = session->open_cursor(session, TIGER_TABLE_NAME, NULL, NULL, &cursor);
	if (ret != 0) {
		printf("workload thread %d open_cursor, ret: %s\n", context->thread_index, wiredtiger_strerror(ret));
		exit(1);
	}

	/* run workload */
	unsigned int seed = context->thread_index + 'w';
	char *key_buffer = (char *) malloc(BUFFER_SIZE);
	char *value_buffer = (char *) malloc(BUFFER_SIZE);
	context->start_time = steady_clock::now();
	for (long i = 0; i < context->nr_op; ++i) {
		bool read = rand_r(&seed) < (int)((double)RAND_MAX * context->read_ratio);
		long key = (((long)rand_r(&seed) << (sizeof(int) * 8)) | rand_r(&seed)) % context->nr_entry;
		get_key_string(key_buffer, key);
		cursor->set_key(cursor, key_buffer);
		if (read) {
			ret = cursor->search(cursor);
			if (ret != 0) {
				printf("workload thread %d search failed, ret: %s\n", context->thread_index, wiredtiger_strerror(ret));
				exit(1);
			}
			++context->nr_read;
		} else {
			get_random_string(value_buffer, context->value_size, &seed);
			cursor->set_value(cursor, value_buffer);
			ret = cursor->update(cursor);
			if (ret != 0) {
				printf("workload thread %d update failed, ret: %s\n", context->thread_index, wiredtiger_strerror(ret));
				exit(1);
			}
			++context->nr_write;
		}
	}
	context->end_time = steady_clock::now();
	session->close(session, NULL);
	context->finished = true;
}

int main(int argc, char *argv[]) {
	if (argc != 6) {
		printf("Usage: %s <number of entries> <value size> <number of threads> <read ratio> <number of ops>\n", argv[0]);
		exit(1);
	}
	long nr_entry = atol(argv[NR_ENTRY]);
	long value_size = atol(argv[VALUE_SIZE]);
	int nr_thread = atoi(argv[NR_THREAD]);
	float read_ratio = atof(argv[READ_RATIO]);
	long nr_op = atol(argv[NR_OP]);

	/* open WiredTiger connection */
	WT_CONNECTION *conn;
	int ret;
	ret = wiredtiger_open(TIGER_HOME, NULL, TIGER_OPEN_CONN_CONFIG, &conn);
	if (ret != 0) {
		printf("wiredtiger_open failed, ret: %s\n", wiredtiger_strerror(ret));
		exit(1);
	}
	printf("opened WiredTiger connection\n");

	/* drop existing table & create new table */
	WT_SESSION *session;
	ret = conn->open_session(conn, NULL, NULL, &session);
	if (ret != 0) {
		printf("open_session failed, ret: %s\n", wiredtiger_strerror(ret));
		exit(1);
	}
	session->drop(session, TIGER_TABLE_NAME, NULL);
	printf("existing table dropped\n");
	ret = session->create(session, TIGER_TABLE_NAME, TIGER_CREATE_TABLE_CONFIG);
	if (ret != 0) {
		printf("create failed, ret: %s\n", wiredtiger_strerror(ret));
		exit(1);
	}
	printf("new table created\n");
	session->close(session, NULL);

	/* populate database */
	int nr_init_thread = 1;
	struct init_thread_context *init_context_arr = new struct init_thread_context[nr_init_thread];
	long entry_per_thread = (nr_entry + nr_init_thread - 1) / nr_init_thread;
	for (int thread_index = 0; thread_index < nr_init_thread; ++thread_index) {
		struct init_thread_context *context = &init_context_arr[thread_index];
		context->thread_index = thread_index;
		context->type = INIT;
		context->nr_entry = nr_entry;
		context->value_size = value_size;
		context->conn = conn;

		context->start_key = entry_per_thread * thread_index;
		context->end_key = min(nr_entry, entry_per_thread * (thread_index + 1));
		context->cur_key = context->start_key;
	}
	for (int thread_index = 0; thread_index < nr_init_thread; ++thread_index) {
		init_context_arr[thread_index].thread_v = thread(init_thread_fn, &init_context_arr[thread_index]);
	}
	/* display real-time progress */
	long prev_nr_entry = 0;
	steady_clock::time_point prev_check_time = steady_clock::now();
	this_thread::sleep_for(chrono::seconds(1));
	for (;;) {
		long cur_nr_entry = 0;
		for (int thread_index = 0; thread_index < nr_init_thread; ++thread_index) {
			struct init_thread_context *context = &init_context_arr[thread_index];
			cur_nr_entry += (context->cur_key - context->start_key);
		}
		steady_clock::time_point cur_check_time = steady_clock::now();
		long duration = duration_cast<milliseconds>(cur_check_time - prev_check_time).count();
		double cur_insert_throughput = 1000 * (double)(cur_nr_entry - prev_nr_entry) / duration;
		double progress = (double) cur_nr_entry / nr_entry;
		printf("populating progress: %.1f%%, insert throughput: %.2f ops/sec\n", progress * 100, cur_insert_throughput);
		prev_nr_entry = cur_nr_entry;
		prev_check_time = cur_check_time;

		if (cur_nr_entry == nr_entry) {
			break;
		}
		this_thread::sleep_for(chrono::seconds(1));
	}
	for (int thread_index = 0; thread_index < nr_init_thread; ++thread_index) {
		struct init_thread_context *context = &init_context_arr[thread_index];
		context->thread_v.join();
	}
	printf("database populated\n");

	/* start workload */
	struct workload_thread_context *workload_context_arr = new struct workload_thread_context[nr_thread];
	for (int thread_index = 0; thread_index < nr_thread; ++thread_index) {
		struct workload_thread_context *context = &workload_context_arr[thread_index];
		context->thread_index = thread_index;
		context->type = WORKLOAD;
		context->nr_entry = nr_entry;
		context->value_size = value_size;
		context->conn = conn;

		context->nr_thread = nr_thread;
		context->read_ratio = read_ratio;
		context->nr_op = nr_op;
		context->nr_read = 0;
		context->nr_write = 0;
		context->finished = false;
	}
	for (int thread_index = 0; thread_index < nr_thread; ++thread_index) {
		workload_context_arr[thread_index].thread_v = thread(workload_thread_fn, &workload_context_arr[thread_index]);
	}

	/* display real-time perf */
	long prev_nr_read = 0, prev_nr_write = 0;
	steady_clock::time_point prev_visit_time = steady_clock::now();
	this_thread::sleep_for(chrono::seconds(1));
	for (;;) {
		long cur_nr_read = 0, cur_nr_write = 0;
		bool all_finished = true;
		for (int thread_index = 0; thread_index < nr_thread; ++thread_index) {
			struct workload_thread_context *context = &workload_context_arr[thread_index];
			cur_nr_read += context->nr_read;
			cur_nr_write += context->nr_write;
			all_finished = all_finished && context->finished;
		}

		steady_clock::time_point cur_visit_time = steady_clock::now();
		long duration = duration_cast<milliseconds>(cur_visit_time - prev_visit_time).count();
		double cur_read_throughput = 1000 * (double)(cur_nr_read - prev_nr_read) / duration;
		double cur_write_throughput = 1000 * (double)(cur_nr_write - prev_nr_write) / duration;
		printf("Read Throughput: %.2lf ops/sec, Write Throughput: %.2lf ops/sec, Total Throughput: %.2lf ops/sec\n",
		       cur_read_throughput, cur_write_throughput, cur_read_throughput + cur_write_throughput);

		prev_nr_read = cur_nr_read;
		prev_nr_write = cur_nr_write;
		prev_visit_time = cur_visit_time;

		if (all_finished)
			break;
		this_thread::sleep_for(chrono::seconds(1));
	}
	for (int thread_index = 0; thread_index < nr_thread; ++thread_index) {
		struct workload_thread_context *context = &workload_context_arr[thread_index];
		context->thread_v.join();
	}

	/* display overall perf */
	double read_throughput = 0;
	double write_throughput = 0;
	for (int thread_index = 0; thread_index < nr_thread; ++thread_index) {
		struct workload_thread_context *context = &workload_context_arr[thread_index];
		context->thread_v.join();
		long duration = duration_cast<milliseconds>(context->end_time - context->start_time).count();
		read_throughput += 1000 * (double)context->nr_read / duration;
		write_throughput += 1000 * (double)context->nr_write / duration;
	}
	printf("Overall Read Throughput: %.2lf ops/sec, Overall Write Throughput: %.2lf ops/sec, Overall Total Throughput: %.2lf ops/sec\n",
	       read_throughput, write_throughput, read_throughput + write_throughput);
}
