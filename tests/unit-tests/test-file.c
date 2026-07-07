/*
 * Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <fcntl.h>

#include <osdp.h>
#include "test.h"

#define SEND_FILE "test-file-tx-send.txt"
#define REC_FILE "test-file-tx-receive.txt"
#define FILE_CONTENT_REPS (200)
#define FILE_CONTENT_CHUNK "0123456789abcde\n"
#define FILE_CONTENT_CHUNK_LEN (16)

struct test_data {
	bool is_cp;
	int file_id;
	int fd;
	/* Sender-side busy injection (CP read callback only) */
	int read_count;        /* total read() calls */
	int read_busy_mod;     /* if >0, every Nth read returns 0 */
	bool read_always_busy; /* if true, every read returns 0 */
	int empty_read_count;  /* observed 0-length reads */
};

struct test_data sender_data;
struct test_data receiver_data;

static int test_fops_open(void *arg, int file_id, int *size)
{
	struct test_data *t = arg;

	if (file_id != 1 || t->fd != 0) {
		printf("%s_open: fd:%d send_fd:%d\n",
			t->is_cp ? "sender" : "receiver", file_id, t->fd);
		return -1;
	}

	t->file_id = file_id;

	if (t->is_cp)
		t->fd = open(SEND_FILE, O_RDONLY);
	else
		t->fd = open(REC_FILE, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);

	if (t->fd < 0) {
		printf(SUB_1 "%s_open: source re-open failed\n",
			t->is_cp ? "sender" : "receiver");
		return -1;
	}

	*size = FILE_CONTENT_REPS * FILE_CONTENT_CHUNK_LEN;

	return 0;
}

static int test_fops_read(void *arg, void *buf, int size, int offset)
{
	struct test_data *t = arg;
	ssize_t ret;

	if (t->fd <= 0) {
		printf(SUB_1 "%s_read: fd:%d\n",
			t->is_cp ? "sender" : "receiver", t->fd);
		return -1;
	}

	t->read_count++;

	/* Simulate a momentarily-busy host: returning 0 means "no data
	 * ready right now"; libosdp must keep the transfer alive instead
	 * of aborting. */
	if (t->read_always_busy ||
	    (t->read_busy_mod > 0 && (t->read_count % t->read_busy_mod) == 0)) {
		t->empty_read_count++;
		return 0;
	}

	ret = pread(t->fd, buf, (size_t)size, (size_t)offset);

	return (int)ret;
}

static int test_fops_write(void *arg, const void *buf, int size, int offset)
{
	ssize_t ret;
	struct test_data *t = arg;

	if (t->fd <= 0) {
		printf(SUB_1 "%s_write: fd:%d\n",
			t->is_cp ? "sender" : "receiver", t->fd);
		return -1;
	}

	ret = pwrite(t->fd, buf, (size_t)size, (size_t)offset);

	return (int)ret;
}

static int test_fops_close(void *arg)
{
	struct test_data *t = arg;

	if (t->fd == 0) {
		printf(SUB_1 "%s_close: fd:%d\n",
			t->is_cp ? "sender" : "receiver", t->fd);
		return -1;
	}
	close(t->fd);
	t->fd = 0;
	return 0;
}

static int test_create_file()
{
	int fd, rc, i;

	fd = open(SEND_FILE, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		perror(SUB_1 "sender_open: source file open failed");
		return -1;
	}

	for (i = 0; i < FILE_CONTENT_REPS; i++) {
		rc = write(fd, FILE_CONTENT_CHUNK, FILE_CONTENT_CHUNK_LEN);
		if (rc != FILE_CONTENT_CHUNK_LEN) {
			printf(SUB_1 "source file write failed at chunk%i\n", i);
			return -1;
		}
	}
	close(fd);
	return 0;
}

static bool test_check_rec_file()
{
	int i, rc, rec_fd;
	char buf[1024];

	rec_fd = open(REC_FILE, O_RDONLY);
	if (rec_fd < 0) {
		printf(SUB_1 "check_rec_file: open rec file failed\n");
		return false;
	}

	for (i = 0; i < FILE_CONTENT_REPS; i++) {
		rc = read(rec_fd, buf, FILE_CONTENT_CHUNK_LEN);
		if (rc != FILE_CONTENT_CHUNK_LEN) {
			printf(SUB_1 "check_rec_file: rec file read "
			       "failed at chunk %i; read: %d\n", i, rc);
			goto err;
		}
		buf[rc] = 0;
		if (memcmp(buf, FILE_CONTENT_CHUNK, FILE_CONTENT_CHUNK_LEN)) {
			printf(SUB_1 "check_rec_file: memcmp failed at chunk "
			       "%d;\n" SUB_1 "got: %s", i, buf);
			goto err;
		}
	}

	rc = read(rec_fd, buf, FILE_CONTENT_CHUNK_LEN);
	if (rc != 0)
		goto err;

	close(rec_fd);

	unlink(SEND_FILE);
	unlink(REC_FILE);

	return true;
err:
	close(rec_fd);
	return false;
}

struct file_tx_notification {
	int count;
	int type;
	int arg0;
	int arg1;
};

static struct file_tx_notification g_notif;

static int event_callback(void *arg, int pd, struct osdp_event *ev)
{
	ARG_UNUSED(arg);
	ARG_UNUSED(pd);

	if (ev->type != OSDP_EVENT_NOTIFICATION) {
		return 0;
	}
	if (ev->notif.type != OSDP_NOTIFICATION_FILE_TX_DONE) {
		return 0;
	}
	g_notif.count++;
	g_notif.type = ev->notif.type;
	g_notif.arg0 = ev->notif.arg0;
	g_notif.arg1 = ev->notif.arg1;
	return 0;
}

static int cmd_callback(void *arg, struct osdp_cmd *cmd)
{
	ARG_UNUSED(arg);
	ARG_UNUSED(cmd);
	printf(SUB_1 "got cmd callback\n");
	return 0;
}

struct file_tx_opts {
	const char *label;
	bool line_noise;
	int read_busy_mod;        /* 0 = no busy injection */
	bool read_always_busy;
	int expected_outcome;     /* OSDP_FILE_TX_OUTCOME_* */
	int wait_deciseconds;     /* notification wait budget, 100ms units */
	bool verify_content;      /* compare REC_FILE against SEND_FILE */
};

static bool run_one_file_tx_case(struct test *t, const struct file_tx_opts *opts)
{
	bool result = false;
	int rc, size, offset;
	osdp_t *cp_ctx, *pd_ctx;
	int cp_runner = -1, pd_runner = -1;
	uint8_t status = 0;

	memset(&g_notif, 0, sizeof(g_notif));
	memset(&sender_data, 0, sizeof(sender_data));
	memset(&receiver_data, 0, sizeof(receiver_data));
	sender_data.is_cp = true;
	sender_data.read_busy_mod = opts->read_busy_mod;
	sender_data.read_always_busy = opts->read_always_busy;

	struct osdp_file_ops sender_ops = {
		.arg = (void *)&sender_data,
		.open = test_fops_open,
		.read = test_fops_read,
		.write = test_fops_write,
		.close = test_fops_close
	};

	struct osdp_file_ops receiver_ops = {
		.arg = (void *)&receiver_data,
		.open = test_fops_open,
		.read = test_fops_read,
		.write = test_fops_write,
		.close = test_fops_close
	};

	printf("\nBegin file transfer test: %s\n", opts->label);
	printf(SUB_1 "setting up OSDP devices\n");

	if (test_setup_devices_ext(t, &cp_ctx, &pd_ctx,
				   OSDP_FLAG_ENABLE_NOTIFICATION, 0)) {
		printf(SUB_1 "Failed to setup devices!\n");
		goto error;
	}

	/* Make sure neither side carries state from a previous case. */
	unlink(REC_FILE);
	if (test_create_file())
		goto error;

	osdp_cp_set_event_callback(cp_ctx, event_callback, NULL);
	osdp_pd_set_command_callback(pd_ctx, cmd_callback, NULL);

	osdp_file_register_ops(cp_ctx, 0, &sender_ops);
	osdp_file_register_ops(pd_ctx, 0, &receiver_ops);

	printf(SUB_1 "starting async runners\n");

	cp_runner = async_runner_start(cp_ctx, osdp_cp_refresh);
	pd_runner = async_runner_start(pd_ctx, osdp_pd_refresh);

	if (cp_runner < 0 || pd_runner < 0) {
		printf(SUB_1 "Failed to created CP/PD runners\n");
		goto error;
	}

	rc = 0;
	while (1) {
		if (rc > 10) {
			printf(SUB_1 "PD failed to come online");
			goto error;
		}
		osdp_get_status_mask(cp_ctx, &status);
		if (status & 1)
			break;
		usleep(1000 * 1000);
		rc++;
	}

	printf(SUB_1 "initiating file tx command\n");

	struct osdp_cmd cmd = {
		.id = OSDP_CMD_FILE_TX,
		.file_tx = {
			.id = 1,
			.flags = 0,
		}
	};
	if (osdp_cp_submit_command(cp_ctx, 0, &cmd)) {
		printf(SUB_1 "Failed to initiate file tx command\n");
		goto error;
	}

	printf(SUB_1 "waiting for file tx done notification\n");
	if (opts->line_noise)
		enable_line_noise();

	rc = 0;
	while (g_notif.count == 0) {
		usleep(100 * 1000);
		if (++rc > opts->wait_deciseconds) {
			printf(SUB_1 "file tx notification not received! "
			       "empty_reads=%d\n", sender_data.empty_read_count);
			if (opts->line_noise)
				print_line_noise_stats();
			goto error;
		}
	}

	if (g_notif.count != 1) {
		printf(SUB_1 "notification fired %d times; expected 1\n",
		       g_notif.count);
		goto error;
	}
	if (g_notif.type != OSDP_NOTIFICATION_FILE_TX_DONE) {
		printf(SUB_1 "unexpected notification type: %d\n", g_notif.type);
		goto error;
	}
	if (g_notif.arg0 != 1) {
		printf(SUB_1 "unexpected file_id: %d (want 1)\n", g_notif.arg0);
		goto error;
	}
	if (g_notif.arg1 != opts->expected_outcome) {
		printf(SUB_1 "unexpected outcome: %d (want %d)\n",
		       g_notif.arg1, opts->expected_outcome);
		goto error;
	}

	/* Poll API must report not-in-progress after completion. */
	if (osdp_get_file_tx_status(cp_ctx, 0, &size, &offset) != -1) {
		printf(SUB_1 "poll status did not reset after completion\n");
		goto error;
	}

	if (opts->verify_content) {
		result = test_check_rec_file();
		printf(SUB_1 "%s: %s (empty_reads=%d)\n", opts->label,
		       result ? "succeeded" : "failed",
		       sender_data.empty_read_count);
	} else {
		/* Aborted case: pin the "bounded retries then abort"
		 * contract. The transfer must not collapse on the first
		 * empty read — libosdp has to keep the link alive across
		 * several busy ticks before giving up. */
		const int min_retries = 5;
		if (sender_data.empty_read_count < min_retries) {
			printf(SUB_1 "%s: aborted after only %d empty reads "
			       "(want >= %d) — retry budget too small\n",
			       opts->label, sender_data.empty_read_count,
			       min_retries);
			result = false;
		} else {
			result = true;
			printf(SUB_1 "%s: aborted as expected after %d "
			       "empty reads\n", opts->label,
			       sender_data.empty_read_count);
		}
		unlink(SEND_FILE);
		unlink(REC_FILE);
	}

error:
	disable_line_noise();
	async_runner_stop(cp_runner);
	async_runner_stop(pd_runner);

	osdp_cp_teardown(cp_ctx);
	osdp_pd_teardown(pd_ctx);

	return result;
}

void run_file_tx_tests(struct test *t, bool line_noise)
{
	struct file_tx_opts opts = {
		.label = "baseline (no host busy)",
		.line_noise = line_noise,
		.expected_outcome = OSDP_FILE_TX_OUTCOME_OK,
		.wait_deciseconds = 600, /* 60s */
		.verify_content = true,
	};

	TEST_REPORT(t, run_one_file_tx_case(t, &opts));
}

void run_file_tx_intermittent_tests(struct test *t)
{
	/* CP host returns 0 every 3rd read. libosdp must send keep-alive
	 * frames in place of the missing chunks and resume cleanly once
	 * the host has data again — transfer should still complete with
	 * OUTCOME_OK and identical content. */
	struct file_tx_opts opts = {
		.label = "CP host busy intermittently (every 3rd read)",
		.read_busy_mod = 3,
		.expected_outcome = OSDP_FILE_TX_OUTCOME_OK,
		.wait_deciseconds = 600, /* 60s */
		.verify_content = true,
	};

	TEST_REPORT(t, run_one_file_tx_case(t, &opts));
}

void run_file_tx_permanent_busy_tests(struct test *t)
{
	/* CP host never returns data. libosdp must keep the link alive
	 * for a bounded number of retries and then abort the transfer
	 * via the existing OSDP_FILE_ERROR_RETRY_MAX path. */
	struct file_tx_opts opts = {
		.label = "CP host busy permanently",
		.read_always_busy = true,
		.expected_outcome = OSDP_FILE_TX_OUTCOME_ABORTED,
		.wait_deciseconds = 100, /* 10s — abort should be fast */
		.verify_content = false,
	};

	TEST_REPORT(t, run_one_file_tx_case(t, &opts));
}
