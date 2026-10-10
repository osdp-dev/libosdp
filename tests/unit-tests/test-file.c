/*
 * Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <fcntl.h>

#include <osdp.h>
#include "test.h"
#include "osdp_file.h"

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
	/*
	 * Tolerate an open() that arrives with the previous transfer's
	 * descriptor still around. Only the restart-from-completion case needs
	 * it: the engine closes the file after the completion callback, so a
	 * transfer started from inside that callback would otherwise be
	 * refused by the fops rather than by the engine under test.
	 */
	bool allow_reopen;
};

struct test_data sender_data;
struct test_data receiver_data;

static int test_fops_open(void *arg, int file_id, uint32_t *size)
{
	struct test_data *t = arg;

	if (file_id != 1 || (t->fd != 0 && !t->allow_reopen)) {
		printf("%s_open: fd:%d send_fd:%d\n",
			t->is_cp ? "sender" : "receiver", file_id, t->fd);
		return -1;
	}

	if (t->fd != 0) {
		close(t->fd);
		t->fd = 0;
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

static int test_fops_read(void *arg, void *buf, uint32_t size, uint32_t offset)
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

static int test_fops_write(void *arg, const void *buf, uint32_t size, uint32_t offset)
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
	int start;
	int progress;
	int type;
	int arg0;
	int arg1;
};

static struct file_tx_notification g_notif;

static struct test_completion g_compl;

static int event_callback(void *arg, int pd, struct osdp_event *ev)
{
	ARG_UNUSED(arg);
	ARG_UNUSED(pd);

	if (ev->type != OSDP_EVENT_NOTIFICATION) {
		return 0;
	}
	if (ev->notif.type == OSDP_NOTIFICATION_MP_START)
		g_notif.start++;
	else if (ev->notif.type == OSDP_NOTIFICATION_MP_PROGRESS)
		g_notif.progress++;
	else if (ev->notif.type == OSDP_NOTIFICATION_MP_DONE) {
		g_notif.count++;
		g_notif.type = ev->notif.type;
		g_notif.arg0 = ev->notif.mp.object_id;
		g_notif.arg1 = ev->notif.mp.outcome;
	} else
		return 0;
	return 0;
}

static int cmd_callback(void *arg, struct osdp_cmd *cmd)
{
	ARG_UNUSED(arg);
	ARG_UNUSED(cmd);
	printf(SUB_1 "got cmd callback\n");
	return 0;
}

/* Restart-from-completion probe: state for restart_completion_cb(). */
static osdp_t *g_restart_cp;
static atomic_int g_restart_rc;
static atomic_bool g_restart_armed;

/*
 * Starts a fresh FILE_TX from inside the FILE_TX completion. The engine is
 * mid-teardown there, so libosdp must refuse it -- accepting it would set up a
 * transfer that the completion's own file_state_reset() then wipes, silently
 * stranding the command.
 */
static void restart_completion_cb(void *arg, int pd, struct osdp_cmd *cmd,
				  enum osdp_completion_status status)
{
	if (cmd->id == OSDP_CMD_FILE_TX &&
	    atomic_exchange(&g_restart_armed, false)) {
		struct osdp_cmd again = {
			.id = OSDP_CMD_FILE_TX,
			.file_tx = { .id = 1, .flags = 0 },
		};

		atomic_store(&g_restart_rc,
			     osdp_cp_submit_command(g_restart_cp, 0, &again));
	}
	test_cmd_completion_cb(arg, pd, cmd, status);
}

struct file_tx_opts {
	const char *label;
	bool line_noise;
	int read_busy_mod;        /* 0 = no busy injection */
	bool read_always_busy;
	int expected_outcome;     /* OSDP_MP_OUTCOME_* */
	int wait_deciseconds;     /* transfer wait budget, 100ms units */
	bool verify_content;      /* compare REC_FILE against SEND_FILE */
	bool no_notifications;    /* set up without MP_* notifications */
	bool cancel_mid_transfer; /* cancel once the transfer is under way */
	bool restart_from_completion; /* re-submit FILE_TX from its completion */
};

static bool run_one_file_tx_case(struct test *t, const struct file_tx_opts *opts)
{
	bool result = false;
	int rc;
	uint32_t size, offset;
	osdp_t *cp_ctx, *pd_ctx;
	int cp_runner = -1, pd_runner = -1;
	uint8_t status = 0;

	memset(&g_notif, 0, sizeof(g_notif));
	memset(&sender_data, 0, sizeof(sender_data));
	memset(&receiver_data, 0, sizeof(receiver_data));
	sender_data.is_cp = true;
	sender_data.read_busy_mod = opts->read_busy_mod;
	sender_data.read_always_busy = opts->read_always_busy;
	sender_data.allow_reopen = opts->restart_from_completion;

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
				   opts->no_notifications ?
					   0 : OSDP_FLAG_ENABLE_NOTIFICATION,
				   0)) {
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

	if (opts->restart_from_completion) {
		g_restart_cp = cp_ctx;
		atomic_store(&g_restart_rc, 0);
		atomic_store(&g_restart_armed, true);
		osdp_cp_set_command_completion_callback(cp_ctx,
							 restart_completion_cb,
							 &g_compl);
	} else {
		osdp_cp_set_command_completion_callback(cp_ctx,
							 test_cmd_completion_cb,
							 &g_compl);
	}
	test_completion_reset(&g_compl);

	printf(SUB_1 "starting async runners\n");

	cp_runner = async_runner_start(cp_ctx, osdp_cp_refresh);
	pd_runner = async_runner_start(pd_ctx, osdp_pd_refresh);

	if (cp_runner < 0 || pd_runner < 0) {
		printf(SUB_1 "Failed to created CP/PD runners\n");
		goto error;
	}

	rc = 0;
	while (1) {
		if (rc > 10 * 1000) { /* ~10s online timeout (rc is ms) */
			printf(SUB_1 "PD failed to come online");
			goto error;
		}
		osdp_get_status_mask(cp_ctx, &status);
		if (status & 1)
			break;
		usleep(20 * 1000);
		rc += 20;
	}

	printf(SUB_1 "initiating file tx command\n");

	struct osdp_cmd cmd = {
		.id = OSDP_CMD_FILE_TX,
		.file_tx = {
			.id = 1,
			.flags = 0,
		}
	};
	if (!test_submit_command(cp_ctx, 0, &cmd)) {
		printf(SUB_1 "Failed to initiate file tx command\n");
		goto error;
	}

	/* The engine owns the command until the transfer ends, so nothing may
	 * have settled yet. */
	if (test_completion_count(&g_compl) != 0) {
		printf(SUB_1 "file tx completed at submit (count %d)\n",
		       test_completion_count(&g_compl));
		goto error;
	}

	if (opts->cancel_mid_transfer) {
		rc = 0;
		while (osdp_get_file_tx_status(cp_ctx, 0, &size, &offset) ||
		       offset == 0) {
			usleep(20 * 1000);
			if (++rc > 250) { /* 5s */
				printf(SUB_1 "transfer never got under way\n");
				goto error;
			}
		}
		/* Cancelling a type that is not running must be refused, so a
		 * 0 below means the file engine really took the request. */
		if (osdp_cp_cancel(cp_ctx, 0, OSDP_MP_MSG_BIOREAD) == 0) {
			printf(SUB_1 "cancel: BIOREAD accepted while idle\n");
			goto error;
		}
		if (osdp_cp_cancel(cp_ctx, 0, OSDP_MP_MSG_FILE_TRANSFER)) {
			printf(SUB_1 "cancel: refused a running transfer\n");
			goto error;
		}
		/* A cancel adds no completion of its own; the transfer's own
		 * command reports FAILED when it winds down. */
	}

	printf(SUB_1 "waiting for file tx done notification\n");
	if (opts->line_noise)
		enable_line_noise();

	if (!opts->no_notifications) {
		rc = 0;
		while (g_notif.count == 0) {
			if (test_completion_count(&g_compl) != 0) {
				printf(SUB_1 "completed before MP_DONE\n");
				goto error;
			}
			usleep(100 * 1000);
			if (++rc > opts->wait_deciseconds) {
				printf(SUB_1 "file tx notification not "
				       "received! empty_reads=%d\n",
				       sender_data.empty_read_count);
				if (opts->line_noise)
					print_line_noise_stats();
				goto error;
			}
		}

		if (g_notif.count != 1) {
			printf(SUB_1 "notification fired %d times; "
			       "expected 1\n", g_notif.count);
			goto error;
		}
		if (g_notif.type != OSDP_NOTIFICATION_MP_DONE) {
			printf(SUB_1 "unexpected notification type: %d\n",
			       g_notif.type);
			goto error;
		}
		if (g_notif.arg0 != 1) {
			printf(SUB_1 "unexpected file_id: %d (want 1)\n",
			       g_notif.arg0);
			goto error;
		}
		if (g_notif.arg1 != opts->expected_outcome) {
			printf(SUB_1 "unexpected outcome: %d (want %d)\n",
			       g_notif.arg1, opts->expected_outcome);
			goto error;
		}
	}

	/* The command settles once the transfer ends: exactly one completion,
	 * carrying the transfer's outcome. An aborted transfer reports FAILED,
	 * not ABORTED -- that status means "removed during teardown". */
	if (!test_completion_wait(&g_compl, 1,
				  (opts->wait_deciseconds + 9) / 10)) {
		printf(SUB_1 "file tx never completed\n");
		goto error;
	}
	if (opts->no_notifications &&
	    g_notif.count + g_notif.start + g_notif.progress != 0) {
		printf(SUB_1 "notifications fired while disabled\n");
		goto error;
	}
	if (test_completion_count(&g_compl) != 1 ||
	    test_completion_id(&g_compl) != OSDP_CMD_FILE_TX ||
	    test_completion_status(&g_compl) !=
		    (opts->expected_outcome == OSDP_MP_OUTCOME_OK ?
			     OSDP_COMPLETION_OK :
			     OSDP_COMPLETION_FAILED)) {
		printf(SUB_1 "file tx: %d completions, id %d, status %d\n",
		       test_completion_count(&g_compl),
		       test_completion_id(&g_compl),
		       test_completion_status(&g_compl));
		goto error;
	}

	if (opts->restart_from_completion) {
		if (atomic_load(&g_restart_armed)) {
			printf(SUB_1 "restart probe never ran\n");
			goto error;
		}
		if (atomic_load(&g_restart_rc) != -1) {
			printf(SUB_1 "restart from completion returned %d "
			       "(want -1)\n", atomic_load(&g_restart_rc));
			goto error;
		}
	}

	/* Poll API must report not-in-progress after completion. */
	if (osdp_get_file_tx_status(cp_ctx, 0, &size, &offset) != -1) {
		printf(SUB_1 "poll status did not reset after completion\n");
		goto error;
	}

	if (opts->verify_content) {
		if (!opts->no_notifications &&
		    (g_notif.start != 1 || g_notif.progress < 1)) {
			printf(SUB_1 "unexpected lifecycle: start=%d progress=%d "
			       "(want start==1, progress>=1)\n",
			       g_notif.start, g_notif.progress);
			goto error;
		}
		result = test_check_rec_file();
		printf(SUB_1 "%s: %s (empty_reads=%d)\n", opts->label,
		       result ? "succeeded" : "failed",
		       sender_data.empty_read_count);
	} else if (opts->cancel_mid_transfer) {
		result = true;
		printf(SUB_1 "%s: cancelled mid-transfer as expected\n",
		       opts->label);
		unlink(SEND_FILE);
		unlink(REC_FILE);
	} else {
		/* Aborted case: pin the "bounded retries then abort"
		 * contract. The transfer must not collapse on the first
		 * empty read — libosdp has to keep the link alive across
		 * several busy ticks before giving up. */
		const int min_retries = 5;
		/* START is delivered even when no fragment ever commits, so a
		 * DONE never arrives without a preceding START. */
		if (g_notif.start != 1) {
			printf(SUB_1 "aborted transfer: start=%d (want 1)\n",
			       g_notif.start);
			goto error;
		}
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
		.expected_outcome = OSDP_MP_OUTCOME_OK,
		.wait_deciseconds = 600, /* 60s */
		.verify_content = true,
	};

	TEST_CASE(t, "file_tx_baseline", run_one_file_tx_case(t, &opts));

	/* Same transfer, but the app tries to start the next one from inside
	 * the completion -- which the engine must refuse with -1. */
	opts.label = "restart from completion";
	opts.line_noise = false;
	opts.restart_from_completion = true;
	TEST_CASE(t, "file_tx_restart_from_completion",
		  run_one_file_tx_case(t, &opts));
}

void run_file_tx_no_notification_tests(struct test *t)
{
	/* Completions and notifications are independent features: an app that
	 * never asked for MP_* must still get its command back when the
	 * transfer ends. */
	struct file_tx_opts opts = {
		.label = "notifications disabled",
		.expected_outcome = OSDP_MP_OUTCOME_OK,
		.wait_deciseconds = 600, /* 60s */
		.verify_content = true,
		.no_notifications = true,
	};

	TEST_CASE(t, "file_tx_no_notification", run_one_file_tx_case(t, &opts));
}

void run_file_tx_cancel_tests(struct test *t)
{
	/* A cancel request rides in on its own command but is a control
	 * message, not an operation: it settles at submit while the transfer
	 * it cancels settles FAILED on the command that started it. */
	struct file_tx_opts opts = {
		.label = "cancelled mid-transfer",
		.expected_outcome = OSDP_MP_OUTCOME_ABORTED,
		.wait_deciseconds = 300, /* 30s */
		.cancel_mid_transfer = true,
	};

	TEST_CASE(t, "file_tx_cancel", run_one_file_tx_case(t, &opts));
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
		.expected_outcome = OSDP_MP_OUTCOME_OK,
		.wait_deciseconds = 600, /* 60s */
		.verify_content = true,
	};

	TEST_CASE(t, "file_tx_intermittent", run_one_file_tx_case(t, &opts));
}

/* Wire status codes, mirroring the private ones in osdp_file.c. */
#define KA_STATUS_ACK			0
#define KA_STATUS_CONTENTS_PROCESSED	1
#define KA_STATUS_KEEP_ALIVE		3

static uint32_t g_ka_size;
static bool g_ka_read_at_eof;

/* Sender read that flags any read attempt at or past EOF — the exact
 * regression the PD-requested keep-alive branch must avoid. */
static int ka_fops_read(void *arg, void *buf, uint32_t size, uint32_t offset)
{
	struct test_data *t = arg;

	if (offset >= g_ka_size) {
		g_ka_read_at_eof = true;
		return 0;
	}
	return (int)pread(t->fd, buf, (size_t)size, (size_t)offset);
}

static void ka_make_stat(uint8_t *b, int status)
{
	b[0] = 0x01; /* control */
	b[1] = 0;
	b[2] = 0; /* delay */
	b[3] = (uint8_t)status;
	b[4] = 0; /* status */
	b[5] = 0;
	b[6] = 0; /* rx_size */
}

/*
 * Directly exercises the sender-side PD-requested keep-alive branch of
 * osdp_file_cmd_tx_build. At completion a PD may reply KEEP_ALIVE to keep the
 * channel warm; the sender must then emit header-only pings WITHOUT invoking
 * the app read() at offset==total and WITHOUT consuming its retry budget.
 * libosdp's PD never emits KEEP_ALIVE on the wire, so the loopback harness
 * cannot reach this path — it is pinned here by driving the codec directly.
 */
void run_file_tx_pd_keep_alive_tests(struct test *t)
{
	bool result = false;
	osdp_t *cp_ctx = NULL, *pd_ctx = NULL;
	struct osdp_pd *pd;
	struct osdp_file *f;
	uint8_t buf[512];
	uint8_t stat[7];
	int n;

	printf("\nBegin file transfer test: PD-requested keep-alive (sender)\n");

	memset(&sender_data, 0, sizeof(sender_data));
	sender_data.is_cp = true;
	g_ka_read_at_eof = false;
	g_ka_size = FILE_CONTENT_REPS * FILE_CONTENT_CHUNK_LEN;

	struct osdp_file_ops sender_ops = {
		.arg = (void *)&sender_data,
		.open = test_fops_open,
		.read = ka_fops_read,
		.write = test_fops_write,
		.close = test_fops_close
	};

	if (test_setup_devices_ext(t, &cp_ctx, &pd_ctx,
				   OSDP_FLAG_ENABLE_NOTIFICATION, 0)) {
		printf(SUB_1 "Failed to setup devices!\n");
		goto done;
	}
	unlink(REC_FILE);
	if (test_create_file())
		goto teardown;

	osdp_file_register_ops(cp_ctx, 0, &sender_ops);
	pd = osdp_to_pd((struct osdp *)cp_ctx, 0);
	f = TO_FILE(pd);

	/* Capture lifecycle notifications so completion is verified by the
	 * MP_DONE event, not merely by the engine's offset/state. */
	memset(&g_notif, 0, sizeof(g_notif));
	osdp_cp_set_event_callback(cp_ctx, event_callback, NULL);

	if (osdp_file_tx_command(pd, 1, 0)) {
		printf(SUB_1 "keep-alive: tx_command failed\n");
		goto teardown;
	}

	/* Drive the transfer, replying KEEP_ALIVE to the frame that lands the
	 * final byte instead of the usual completion ack. */
	while (osdp_file_tx_is_active(pd) && f->mp.offset != f->mp.total) {
		bool last;

		n = osdp_file_cmd_tx_build(pd, buf, sizeof(buf));
		if (n < 0) {
			printf(SUB_1 "keep-alive: tx_build failed\n");
			goto teardown;
		}
		last = (f->mp.offset + (uint32_t)f->mp.last_len) >= f->mp.total;
		ka_make_stat(stat, last ? KA_STATUS_KEEP_ALIVE : KA_STATUS_ACK);
		if (osdp_file_cmd_stat_decode(pd, stat, sizeof(stat)) < 0) {
			printf(SUB_1 "keep-alive: stat_decode failed\n");
			goto teardown;
		}
	}

	/* KEEP_ALIVE must have kept the transfer active, parked at EOF. */
	if (!osdp_file_tx_is_active(pd) || f->mp.offset != f->mp.total) {
		printf(SUB_1 "keep-alive: transfer not parked at EOF\n");
		goto teardown;
	}

	/* Reaching offset==total is NOT terminal: MP_DONE must not have fired
	 * while the transfer is parked in keep-alive. */
	if (g_notif.count != 0) {
		printf(SUB_1 "keep-alive: MP_DONE fired while parked at EOF\n");
		goto teardown;
	}

	/* The keep-alive ping: header-only, no read at offset==total. */
	n = osdp_file_cmd_tx_build(pd, buf, sizeof(buf));
	if (n != 1 + OSDP_MP_HDR_SIZE(OSDP_MP_W32)) {
		printf(SUB_1 "keep-alive: ping not header-only (n=%d)\n", n);
		goto teardown;
	}
	if (g_ka_read_at_eof) {
		printf(SUB_1 "keep-alive: read() invoked at offset==total\n");
		goto teardown;
	}
	if (buf[0] != 1) {
		printf(SUB_1 "keep-alive: wrong type byte %d\n", buf[0]);
		goto teardown;
	}
	if (!osdp_file_tx_is_active(pd)) {
		printf(SUB_1 "keep-alive: ping ended the transfer\n");
		goto teardown;
	}

	/* PD finally accepts: transfer completes cleanly. */
	ka_make_stat(stat, KA_STATUS_CONTENTS_PROCESSED);
	if (osdp_file_cmd_stat_decode(pd, stat, sizeof(stat)) < 0) {
		printf(SUB_1 "keep-alive: final stat_decode failed\n");
		goto teardown;
	}
	if (osdp_file_tx_is_active(pd)) {
		printf(SUB_1 "keep-alive: transfer did not complete\n");
		goto teardown;
	}
	/* Completion delivers exactly one MP_DONE with the OK outcome. */
	if (g_notif.count != 1 || g_notif.type != OSDP_NOTIFICATION_MP_DONE ||
	    g_notif.arg1 != OSDP_MP_OUTCOME_OK) {
		printf(SUB_1 "keep-alive: MP_DONE not delivered on completion "
		       "(count=%d type=%d outcome=%d)\n",
		       g_notif.count, g_notif.type, g_notif.arg1);
		goto teardown;
	}
	result = true;
	printf(SUB_1 "PD-requested keep-alive: succeeded\n");

teardown:
	unlink(SEND_FILE);
	unlink(REC_FILE);
	osdp_cp_teardown(cp_ctx);
	osdp_pd_teardown(pd_ctx);
done:
	TEST_CASE(t, "file_tx_pd_keep_alive", result);
}

static int g_empty_open_calls;
static int g_empty_close_calls;

static int empty_fops_open(void *arg, int file_id, uint32_t *size)
{
	ARG_UNUSED(arg);
	ARG_UNUSED(file_id);
	g_empty_open_calls++;
	*size = 0;
	return 0;
}

static int empty_fops_close(void *arg)
{
	ARG_UNUSED(arg);
	g_empty_close_calls++;
	return 0;
}

/* A file that opens but is empty is refused, and must be closed again. */
void run_file_tx_empty_file_tests(struct test *t)
{
	bool result = false;
	osdp_t *cp_ctx = NULL, *pd_ctx = NULL;
	struct osdp_pd *pd;
	struct osdp_file_ops empty_ops = {
		.open = empty_fops_open,
		.read = test_fops_read,
		.write = test_fops_write,
		.close = empty_fops_close,
	};

	printf("\nBegin file transfer test: empty file\n");

	g_empty_open_calls = 0;
	g_empty_close_calls = 0;
	if (test_setup_devices(t, &cp_ctx, &pd_ctx)) {
		printf(SUB_1 "Failed to setup devices!\n");
		goto done;
	}
	osdp_file_register_ops(cp_ctx, 0, &empty_ops);
	pd = osdp_to_pd((struct osdp *)cp_ctx, 0);

	if (osdp_file_tx_command(pd, 1, 0) != -1) {
		printf(SUB_1 "empty file: transfer started\n");
		goto teardown;
	}
	if (g_empty_open_calls != 1 || g_empty_close_calls != 1) {
		printf(SUB_1 "empty file: open %d, close %d, want 1 each\n",
		       g_empty_open_calls, g_empty_close_calls);
		goto teardown;
	}
	result = true;

teardown:
	osdp_cp_teardown(cp_ctx);
	osdp_pd_teardown(pd_ctx);
done:
	TEST_CASE(t, "file_tx_empty_file", result);
}

/*
 * File ops are part of setup: a transfer can only be under way once the
 * context has been refreshed, so after that the ops are fixed.
 */
void run_file_register_ops_setup_only_tests(struct test *t)
{
	bool result = false;
	osdp_t *cp_ctx = NULL, *pd_ctx = NULL;
	struct osdp_file_ops ops = {
		.open = empty_fops_open,
		.read = test_fops_read,
		.write = test_fops_write,
		.close = empty_fops_close,
	};

	printf("\nBegin file transfer test: register ops before refresh\n");

	if (test_setup_devices(t, &cp_ctx, &pd_ctx)) {
		printf(SUB_1 "Failed to setup devices!\n");
		goto done;
	}
	if (osdp_file_register_ops(cp_ctx, 0, &ops) != 0 ||
	    osdp_file_register_ops(pd_ctx, 0, &ops) != 0) {
		printf(SUB_1 "register refused before the first refresh\n");
		goto teardown;
	}
	osdp_cp_refresh(cp_ctx);
	osdp_pd_refresh(pd_ctx);
	if (osdp_file_register_ops(cp_ctx, 0, &ops) != -1 ||
	    osdp_file_register_ops(pd_ctx, 0, &ops) != -1) {
		printf(SUB_1 "register accepted after the first refresh\n");
		goto teardown;
	}
	result = true;

teardown:
	osdp_cp_teardown(cp_ctx);
	osdp_pd_teardown(pd_ctx);
done:
	TEST_CASE(t, "file_register_ops_setup_only", result);
}

static int16_t stat_reply_status(const uint8_t *stat)
{
	return (int16_t)(stat[3] | ((uint16_t)stat[4] << 8));
}

/*
 * Exercises the receiver-side idle-frame path directly: a header-only frame
 * with offset >= total is the keep-alive a CP sends after FTSTAT status 3
 * ("finishing", OSDP 2.2 §7.25). The PD must ACK it without advancing the
 * offset and without tearing the transfer down — in the FILETRANSFER family
 * this encoding is NOT the §5.10.2 early termination.
 */
void run_file_rx_idle_frame_tests(struct test *t)
{
	bool result = false;
	osdp_t *cp_ctx = NULL, *pd_ctx = NULL;
	struct osdp_pd *pd;
	struct osdp_file *f;
	uint8_t frame[64], stat[16], data[16];
	const uint32_t total = 32;
	const int hdr = OSDP_MP_HDR_SIZE(OSDP_MP_W32);
	int n;

	printf("\nBegin file transfer test: receiver idle frame\n");

	memset(&receiver_data, 0, sizeof(receiver_data));
	receiver_data.is_cp = false;

	struct osdp_file_ops recv_ops = {
		.arg = (void *)&receiver_data,
		.open = test_fops_open,
		.read = test_fops_read,
		.write = test_fops_write,
		.close = test_fops_close
	};

	if (test_setup_devices(t, &cp_ctx, &pd_ctx)) {
		printf(SUB_1 "Failed to setup devices!\n");
		goto done;
	}
	unlink(REC_FILE);
	osdp_file_register_ops(pd_ctx, 0, &recv_ops);
	pd = osdp_to_pd((struct osdp *)pd_ctx, 0);
	f = TO_FILE(pd);
	memset(data, 0xA5, sizeof(data));

	/* First data frame opens the transfer: total=32, off=0, dlen=16. */
	frame[0] = 1;
	osdp_mp_hdr_write(OSDP_MP_W32, total, 0, 16, frame + 1);
	memcpy(frame + 1 + hdr, data, 16);
	if (osdp_file_cmd_tx_decode(pd, frame, 1 + hdr + 16) < 0) {
		printf(SUB_1 "idle: first frame rejected\n");
		goto teardown;
	}
	n = osdp_file_cmd_stat_build(pd, stat, sizeof(stat));
	if (n < 0 || stat_reply_status(stat) != KA_STATUS_ACK ||
	    f->mp.offset != 16) {
		printf(SUB_1 "idle: first ack n=%d status=%d off=%u\n", n,
		       stat_reply_status(stat), f->mp.offset);
		goto teardown;
	}

	/* Idle frame: off == total, zero length. Must be ACKed with no
	 * offset movement and no teardown. */
	frame[0] = 1;
	osdp_mp_hdr_write(OSDP_MP_W32, total, total, 0, frame + 1);
	if (osdp_file_cmd_tx_decode(pd, frame, 1 + hdr) < 0) {
		printf(SUB_1 "idle: idle frame rejected\n");
		goto teardown;
	}
	if (!osdp_file_tx_is_active(pd) || f->mp.offset != 16) {
		printf(SUB_1 "idle: idle frame ended/advanced transfer\n");
		goto teardown;
	}
	n = osdp_file_cmd_stat_build(pd, stat, sizeof(stat));
	if (n < 0 || stat_reply_status(stat) != KA_STATUS_ACK ||
	    f->mp.offset != 16 || !osdp_file_tx_is_active(pd)) {
		printf(SUB_1 "idle: ping ack n=%d status=%d off=%u\n", n,
		       stat_reply_status(stat), f->mp.offset);
		goto teardown;
	}

	/* Final data frame completes the transfer normally. */
	frame[0] = 1;
	osdp_mp_hdr_write(OSDP_MP_W32, total, 16, 16, frame + 1);
	memcpy(frame + 1 + hdr, data, 16);
	if (osdp_file_cmd_tx_decode(pd, frame, 1 + hdr + 16) < 0) {
		printf(SUB_1 "idle: final frame rejected\n");
		goto teardown;
	}
	n = osdp_file_cmd_stat_build(pd, stat, sizeof(stat));
	if (n < 0 ||
	    stat_reply_status(stat) != KA_STATUS_CONTENTS_PROCESSED ||
	    osdp_file_tx_is_active(pd)) {
		printf(SUB_1 "idle: completion n=%d status=%d active=%d\n", n,
		       stat_reply_status(stat), osdp_file_tx_is_active(pd));
		goto teardown;
	}
	result = true;
	printf(SUB_1 "receiver idle frame: succeeded\n");

teardown:
	unlink(REC_FILE);
	osdp_cp_teardown(cp_ctx);
	osdp_pd_teardown(pd_ctx);
done:
	TEST_CASE(t, "file_rx_idle_frame", result);
}

/* Wire statuses this suite asserts but osdp_file.c did not previously emit. */
#define KA_STATUS_PD_RESET	 2
#define KA_STATUS_ERR_ABORT	 -1
#define KA_STATUS_ERR_UNKNOWN	 -2
#define KA_STATUS_ERR_INVALID	 -3

/* Scripted finalize: reports "still working" g_fin_busy_calls times, then
 * completes with g_fin_outcome. The counters let the test assert that the
 * library re-polls rather than latching the first answer. */
static int g_fin_calls;
static int g_fin_busy_calls;
static int g_fin_close_calls;
static enum osdp_mp_outcome g_fin_outcome;

static bool fin_fops_finalize(void *arg, enum osdp_mp_outcome *outcome,
			      uint16_t *delay_ms)
{
	(void)arg;

	if (g_fin_calls++ < g_fin_busy_calls) {
		*delay_ms = 200;
		return false;
	}
	*outcome = g_fin_outcome;
	return true;
}

static int fin_fops_close(void *arg)
{
	g_fin_close_calls++;
	return test_fops_close(arg);
}

/* MP_DONE arrives at a PD app as a command callback of type
 * OSDP_CMD_NOTIFICATION; mirrors pd_cmd_cb() in test-notifications.c. */
static int g_fin_done_count;
static int g_fin_done_outcome;

static int fin_pd_cmd_cb(void *arg, struct osdp_cmd *cmd)
{
	(void)arg;

	if (cmd->id == OSDP_CMD_NOTIFICATION &&
	    cmd->notif.type == OSDP_NOTIFICATION_MP_DONE) {
		g_fin_done_count++;
		g_fin_done_outcome = cmd->notif.mp.outcome;
	}
	return 0;
}

/* Drive a 32-byte transfer as two 16-byte frames. Returns the wire status of
 * the reply to the frame that lands the last byte, or -100 on a harness
 * failure. Leaves the transfer in whatever state stat_build left it. */
static int fin_drive_to_eof(struct osdp_pd *pd, uint8_t *stat, size_t stat_len)
{
	const uint32_t total = 32;
	const int hdr = OSDP_MP_HDR_SIZE(OSDP_MP_W32);
	uint8_t frame[64], data[16];
	uint32_t off;
	int n;

	memset(data, 0xA5, sizeof(data));

	for (off = 0; off < total; off += 16) {
		frame[0] = 1;
		osdp_mp_hdr_write(OSDP_MP_W32, total, off, 16, frame + 1);
		memcpy(frame + 1 + hdr, data, 16);
		if (osdp_file_cmd_tx_decode(pd, frame, 1 + hdr + 16) < 0) {
			return -100;
		}
		n = osdp_file_cmd_stat_build(pd, stat, stat_len);
		if (n < 0) {
			return -100;
		}
	}
	return stat_reply_status(stat);
}

/* Send one CP idle keep-alive (header-only at offset == total) and return the
 * wire status of the PD's reply, or -100 on a harness failure. */
static int fin_ping(struct osdp_pd *pd, uint8_t *stat, size_t stat_len)
{
	const uint32_t total = 32;
	const int hdr = OSDP_MP_HDR_SIZE(OSDP_MP_W32);
	uint8_t frame[64];
	int n;

	frame[0] = 1;
	osdp_mp_hdr_write(OSDP_MP_W32, total, total, 0, frame + 1);
	if (osdp_file_cmd_tx_decode(pd, frame, 1 + hdr) < 0) {
		return -100;
	}
	n = osdp_file_cmd_stat_build(pd, stat, stat_len);
	if (n < 0) {
		return -100;
	}
	return stat_reply_status(stat);
}

static void fin_reset_app_state(int busy_calls, enum osdp_mp_outcome outcome)
{
	memset(&receiver_data, 0, sizeof(receiver_data));
	receiver_data.is_cp = false;
	g_fin_calls = 0;
	g_fin_busy_calls = busy_calls;
	g_fin_close_calls = 0;
	g_fin_outcome = outcome;
	g_fin_done_count = 0;
	g_fin_done_outcome = -1;
	unlink(REC_FILE);
}

/*
 * The PD-side terminal decision. Until finalize existed, reaching EOF always
 * meant wire status 1 and immediate teardown; a PD could neither hold the
 * transfer open while it worked on the file nor report why it refused one.
 */
void run_file_rx_finalize_tests(struct test *t)
{
	bool result = false;
	osdp_t *cp_ctx = NULL, *pd_ctx = NULL;
	struct osdp_pd *pd;
	struct osdp_file *f;
	uint8_t stat[16];
	size_t i;
	int status;

	struct {
		enum osdp_mp_outcome outcome;
		int status;
	} terminals[] = {
		{ OSDP_MP_OUTCOME_OK, KA_STATUS_CONTENTS_PROCESSED },
		{ OSDP_MP_OUTCOME_ABORTED, KA_STATUS_ERR_ABORT },
		{ OSDP_MP_OUTCOME_UNRECOGNIZED, KA_STATUS_ERR_UNKNOWN },
		{ OSDP_MP_OUTCOME_INVALID, KA_STATUS_ERR_INVALID },
	};

	struct osdp_file_ops recv_ops = {
		.arg = (void *)&receiver_data,
		.open = test_fops_open,
		.read = test_fops_read,
		.write = test_fops_write,
		.close = fin_fops_close,
		.finalize = fin_fops_finalize
	};

	printf("\nBegin file transfer test: receiver finalize\n");

	if (test_setup_devices_ext(t, &cp_ctx, &pd_ctx, 0,
				   OSDP_FLAG_ENABLE_NOTIFICATION)) {
		printf(SUB_1 "Failed to setup devices!\n");
		goto done;
	}
	pd = osdp_to_pd((struct osdp *)pd_ctx, 0);
	osdp_pd_set_command_callback(pd_ctx, fin_pd_cmd_cb, NULL);

	/* 1. Busy twice, then reboot. */
	fin_reset_app_state(2, OSDP_MP_OUTCOME_OK_REBOOTING);
	osdp_file_register_ops(pd_ctx, 0, &recv_ops);
	f = TO_FILE(pd); /* pd->file is allocated by the register_ops above */

	status = fin_drive_to_eof(pd, stat, sizeof(stat));
	if (status != KA_STATUS_KEEP_ALIVE || !osdp_file_tx_is_active(pd)) {
		printf(SUB_1 "finalize: eof status=%d active=%d\n", status,
		       osdp_file_tx_is_active(pd));
		goto teardown;
	}
	if (f->mp.offset != f->mp.total) {
		printf(SUB_1 "finalize: eof offset moved while parked "
		       "(off=%u total=%u)\n", f->mp.offset, f->mp.total);
		goto teardown;
	}
	if (g_fin_close_calls != 0 || g_fin_done_count != 0) {
		printf(SUB_1 "finalize: closed/done while still working\n");
		goto teardown;
	}

	status = fin_ping(pd, stat, sizeof(stat));
	if (status != KA_STATUS_KEEP_ALIVE || !osdp_file_tx_is_active(pd)) {
		printf(SUB_1 "finalize: ping1 status=%d active=%d\n", status,
		       osdp_file_tx_is_active(pd));
		goto teardown;
	}
	if (f->mp.offset != f->mp.total) {
		printf(SUB_1 "finalize: ping1 offset moved while parked "
		       "(off=%u total=%u)\n", f->mp.offset, f->mp.total);
		goto teardown;
	}

	status = fin_ping(pd, stat, sizeof(stat));
	if (status != KA_STATUS_PD_RESET || osdp_file_tx_is_active(pd)) {
		printf(SUB_1 "finalize: ping2 status=%d active=%d\n", status,
		       osdp_file_tx_is_active(pd));
		goto teardown;
	}
	if (g_fin_calls != 3 || g_fin_close_calls != 1) {
		printf(SUB_1 "finalize: calls=%d closes=%d (want 3/1)\n",
		       g_fin_calls, g_fin_close_calls);
		goto teardown;
	}
	if (g_fin_done_count != 1 ||
	    g_fin_done_outcome != OSDP_MP_OUTCOME_OK_REBOOTING) {
		printf(SUB_1 "finalize: done=%d outcome=%d\n", g_fin_done_count,
		       g_fin_done_outcome);
		goto teardown;
	}

	/* 2. Every terminal outcome reaches the wire as its own status. */
	for (i = 0; i < sizeof(terminals) / sizeof(terminals[0]); i++) {
		fin_reset_app_state(0, terminals[i].outcome);
		osdp_file_register_ops(pd_ctx, 0, &recv_ops);

		status = fin_drive_to_eof(pd, stat, sizeof(stat));
		if (status != terminals[i].status ||
		    osdp_file_tx_is_active(pd)) {
			printf(SUB_1
			       "finalize: outcome %d -> status %d (want %d) active=%d\n",
			       terminals[i].outcome, status,
			       terminals[i].status,
			       osdp_file_tx_is_active(pd));
			goto teardown;
		}
		if (g_fin_close_calls != 1) {
			printf(SUB_1 "finalize: outcome %d closes=%d\n",
			       terminals[i].outcome, g_fin_close_calls);
			goto teardown;
		}
	}

	/* 3. An unregistered (NULL) finalize must still behave as returning
	 * true with OSDP_MP_OUTCOME_OK, so the default is not silently
	 * changed later. */
	{
		struct osdp_file_ops nofin_ops = {
			.arg = (void *)&receiver_data,
			.open = test_fops_open,
			.read = test_fops_read,
			.write = test_fops_write,
			.close = fin_fops_close
		};

		fin_reset_app_state(0, OSDP_MP_OUTCOME_OK);
		osdp_file_register_ops(pd_ctx, 0, &nofin_ops);

		status = fin_drive_to_eof(pd, stat, sizeof(stat));
		if (status != KA_STATUS_CONTENTS_PROCESSED ||
		    osdp_file_tx_is_active(pd)) {
			printf(SUB_1
			       "finalize: NULL finalize status=%d (want %d) active=%d\n",
			       status, KA_STATUS_CONTENTS_PROCESSED,
			       osdp_file_tx_is_active(pd));
			goto teardown;
		}
	}

	result = true;
	printf(SUB_1 "receiver finalize: succeeded\n");

teardown:
	unlink(REC_FILE);
	osdp_cp_teardown(cp_ctx);
	osdp_pd_teardown(pd_ctx);
done:
	TEST_CASE(t, "file_rx_finalize", result);
}

/*
 * Receiver error paths around engine frame rejection:
 *  - a malformed FIRST frame that passes the pre-open length bound (so the
 *    app open() has already run) must tear the just-opened transfer back
 *    down — no dangling open file / INPROG engine state;
 *  - a malformed MID-TRANSFER frame must leave the transfer open so the CP
 *    can retry, and the transfer must still complete afterwards.
 */
void run_file_rx_reject_paths_tests(struct test *t)
{
	bool result = false;
	osdp_t *cp_ctx = NULL, *pd_ctx = NULL;
	struct osdp_pd *pd;
	struct osdp_file *f;
	uint8_t frame[64], stat[16], data[16];
	const uint32_t total = 32;
	const int hdr = OSDP_MP_HDR_SIZE(OSDP_MP_W32);
	int n;

	printf("\nBegin file transfer test: receiver reject paths\n");

	memset(&receiver_data, 0, sizeof(receiver_data));
	receiver_data.is_cp = false;

	struct osdp_file_ops recv_ops = {
		.arg = (void *)&receiver_data,
		.open = test_fops_open,
		.read = test_fops_read,
		.write = test_fops_write,
		.close = test_fops_close
	};

	if (test_setup_devices(t, &cp_ctx, &pd_ctx)) {
		printf(SUB_1 "Failed to setup devices!\n");
		goto done;
	}
	unlink(REC_FILE);
	osdp_file_register_ops(pd_ctx, 0, &recv_ops);
	pd = osdp_to_pd((struct osdp *)pd_ctx, 0);
	f = TO_FILE(pd);
	memset(data, 0x5A, sizeof(data));

	/* Malformed first frame: forward gap (off=4 on the opening frame).
	 * The pre-open length bound passes, open() runs, the engine rejects
	 * — the transfer must not stay half-open. */
	frame[0] = 1;
	osdp_mp_hdr_write(OSDP_MP_W32, total, 4, 4, frame + 1);
	memcpy(frame + 1 + hdr, data, 4);
	if (osdp_file_cmd_tx_decode(pd, frame, 1 + hdr + 4) == 0) {
		printf(SUB_1 "reject: gapped first frame accepted\n");
		goto teardown;
	}
	if (osdp_file_tx_is_active(pd) || receiver_data.fd != 0) {
		printf(SUB_1 "reject: dangling transfer (active=%d fd=%d)\n",
		       osdp_file_tx_is_active(pd), receiver_data.fd);
		goto teardown;
	}

	/* Fresh transfer: good first frame. */
	frame[0] = 1;
	osdp_mp_hdr_write(OSDP_MP_W32, total, 0, 16, frame + 1);
	memcpy(frame + 1 + hdr, data, 16);
	if (osdp_file_cmd_tx_decode(pd, frame, 1 + hdr + 16) < 0) {
		printf(SUB_1 "reject: good first frame refused\n");
		goto teardown;
	}
	n = osdp_file_cmd_stat_build(pd, stat, sizeof(stat));
	if (n < 0 || stat_reply_status(stat) != KA_STATUS_ACK ||
	    f->mp.offset != 16) {
		printf(SUB_1 "reject: first ack off=%u\n", f->mp.offset);
		goto teardown;
	}

	/* Mid-transfer frame with a differing total: rejected, but the
	 * transfer stays open at the committed offset. */
	frame[0] = 1;
	osdp_mp_hdr_write(OSDP_MP_W32, total + 1, 16, 4, frame + 1);
	memcpy(frame + 1 + hdr, data, 4);
	if (osdp_file_cmd_tx_decode(pd, frame, 1 + hdr + 4) == 0) {
		printf(SUB_1 "reject: mismatched-total frame accepted\n");
		goto teardown;
	}
	if (!osdp_file_tx_is_active(pd) || f->mp.offset != 16) {
		printf(SUB_1 "reject: mid-transfer reject closed transfer "
		       "(active=%d off=%u)\n",
		       osdp_file_tx_is_active(pd), f->mp.offset);
		goto teardown;
	}

	/* CP retries correctly: the transfer still completes. */
	frame[0] = 1;
	osdp_mp_hdr_write(OSDP_MP_W32, total, 16, 16, frame + 1);
	memcpy(frame + 1 + hdr, data, 16);
	if (osdp_file_cmd_tx_decode(pd, frame, 1 + hdr + 16) < 0) {
		printf(SUB_1 "reject: final frame refused\n");
		goto teardown;
	}
	n = osdp_file_cmd_stat_build(pd, stat, sizeof(stat));
	if (n < 0 ||
	    stat_reply_status(stat) != KA_STATUS_CONTENTS_PROCESSED ||
	    osdp_file_tx_is_active(pd)) {
		printf(SUB_1 "reject: completion n=%d status=%d active=%d\n",
		       n, stat_reply_status(stat), osdp_file_tx_is_active(pd));
		goto teardown;
	}
	result = true;
	printf(SUB_1 "receiver reject paths: succeeded\n");

teardown:
	unlink(REC_FILE);
	osdp_cp_teardown(cp_ctx);
	osdp_pd_teardown(pd_ctx);
done:
	TEST_CASE(t, "file_rx_reject_paths", result);
}

void run_file_tx_permanent_busy_tests(struct test *t)
{
	/* CP host never returns data. libosdp must keep the link alive
	 * for a bounded number of retries and then abort the transfer
	 * via the existing OSDP_FILE_ERROR_RETRY_MAX path. */
	struct file_tx_opts opts = {
		.label = "CP host busy permanently",
		.read_always_busy = true,
		.expected_outcome = OSDP_MP_OUTCOME_ABORTED,
		.wait_deciseconds = 100, /* 10s — abort should be fast */
		.verify_content = false,
	};

	TEST_CASE(t, "file_tx_permanent_busy", run_one_file_tx_case(t, &opts));
}
