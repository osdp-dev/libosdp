/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <fcntl.h>
#include <pthread.h>

#include <osdp.h>
#include "test.h"

#define NOTIF_FILE_SEND "test-notif-send.txt"
#define NOTIF_FILE_RECV "test-notif-recv.txt"
#define NOTIF_FILE_REPS 200
#define NOTIF_FILE_CHUNK "0123456789abcde\n"
#define NOTIF_FILE_CHUNK_LEN 16

struct notif_record {
	int count;
	int last_type;
	int last_arg0;
	int last_arg1;
};

struct notif_ctx {
	struct test *t;
	osdp_t *cp_ctx;
	osdp_t *pd_ctx;
	int cp_runner;
	int pd_runner;

	pthread_mutex_t lock;
	struct notif_record pd_pd_status;
	struct notif_record pd_sc_status;
	struct notif_record cp_file_tx_done;

	struct {
		bool is_cp;
		int fd;
	} sender;
	struct {
		bool is_cp;
		int fd;
	} receiver;
};

static struct notif_ctx g_nx;

static void notif_record_update(struct notif_record *r, int type, int a0, int a1)
{
	r->count++;
	r->last_type = type;
	r->last_arg0 = a0;
	r->last_arg1 = a1;
}

static int pd_cmd_cb(void *arg, struct osdp_cmd *cmd)
{
	struct notif_ctx *nx = arg;

	if (cmd->id != OSDP_CMD_NOTIFICATION) {
		return 0;
	}
	pthread_mutex_lock(&nx->lock);
	switch (cmd->notif.type) {
	case OSDP_NOTIFICATION_PD_STATUS:
		notif_record_update(&nx->pd_pd_status, cmd->notif.type,
				    cmd->notif.arg0, cmd->notif.arg1);
		break;
	case OSDP_NOTIFICATION_SC_STATUS:
		notif_record_update(&nx->pd_sc_status, cmd->notif.type,
				    cmd->notif.arg0, cmd->notif.arg1);
		break;
	default: break;
	}
	pthread_mutex_unlock(&nx->lock);
	return 0;
}

static int cp_event_cb(void *arg, int pd, struct osdp_event *ev)
{
	struct notif_ctx *nx = arg;

	ARG_UNUSED(pd);

	if (ev->type != OSDP_EVENT_NOTIFICATION) {
		return 0;
	}
	pthread_mutex_lock(&nx->lock);
	if (ev->notif.type == OSDP_NOTIFICATION_FILE_TX_DONE) {
		notif_record_update(&nx->cp_file_tx_done, ev->notif.type,
				    ev->notif.arg0, ev->notif.arg1);
	}
	pthread_mutex_unlock(&nx->lock);
	return 0;
}

static int notif_fops_open(void *arg, int file_id, int *size)
{
	struct { bool is_cp; int fd; } *t = arg;

	if (file_id != 1 || t->fd != 0)
		return -1;

	t->fd = t->is_cp ? open(NOTIF_FILE_SEND, O_RDONLY)
			 : open(NOTIF_FILE_RECV,
				O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);
	if (t->fd < 0) return -1;
	*size = NOTIF_FILE_REPS * NOTIF_FILE_CHUNK_LEN;
	return 0;
}

static int notif_fops_read(void *arg, void *buf, int size, int offset)
{
	struct { bool is_cp; int fd; } *t = arg;

	if (t->fd <= 0) return -1;
	return (int)pread(t->fd, buf, (size_t)size, (size_t)offset);
}

static int notif_fops_write(void *arg, const void *buf, int size, int offset)
{
	struct { bool is_cp; int fd; } *t = arg;

	if (t->fd <= 0) return -1;
	return (int)pwrite(t->fd, buf, (size_t)size, (size_t)offset);
}

static int notif_fops_close(void *arg)
{
	struct { bool is_cp; int fd; } *t = arg;

	if (t->fd == 0) return -1;
	close(t->fd);
	t->fd = 0;
	return 0;
}

static int notif_create_file(void)
{
	int fd, i;

	fd = open(NOTIF_FILE_SEND, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);
	if (fd < 0) return -1;
	for (i = 0; i < NOTIF_FILE_REPS; i++) {
		if (write(fd, NOTIF_FILE_CHUNK, NOTIF_FILE_CHUNK_LEN) !=
		    NOTIF_FILE_CHUNK_LEN) {
			close(fd);
			return -1;
		}
	}
	close(fd);
	return 0;
}

static bool wait_for_pd_online(int timeout_sec)
{
	int rc = 0;
	uint8_t status = 0;

	while (rc < timeout_sec * 10) {
		osdp_get_status_mask(g_nx.cp_ctx, &status);
		if (status & 1)
			return true;
		usleep(100 * 1000);
		rc++;
	}
	return false;
}

static bool wait_for_notif(struct notif_record *r, int timeout_ms)
{
	int elapsed = 0;
	bool hit;

	while (elapsed <= timeout_ms) {
		pthread_mutex_lock(&g_nx.lock);
		hit = r->count > 0;
		pthread_mutex_unlock(&g_nx.lock);
		if (hit) return true;
		usleep(50 * 1000);
		elapsed += 50;
	}
	return false;
}

static int setup_env(void)
{
	pthread_mutex_init(&g_nx.lock, NULL);

	if (test_setup_devices_ext(g_nx.t, &g_nx.cp_ctx, &g_nx.pd_ctx,
				   OSDP_FLAG_ENABLE_NOTIFICATION,
				   OSDP_FLAG_ENABLE_NOTIFICATION))
		return -1;

	osdp_pd_set_command_callback(g_nx.pd_ctx, pd_cmd_cb, &g_nx);
	osdp_cp_set_event_callback(g_nx.cp_ctx, cp_event_cb, &g_nx);

	g_nx.cp_runner = async_runner_start(g_nx.cp_ctx, osdp_cp_refresh);
	g_nx.pd_runner = async_runner_start(g_nx.pd_ctx, osdp_pd_refresh);
	if (g_nx.cp_runner < 0 || g_nx.pd_runner < 0)
		return -1;

	if (!wait_for_pd_online(10)) {
		printf(SUB_1 "PD did not come online\n");
		return -1;
	}
	return 0;
}

static void teardown_env(void)
{
	struct test *t = g_nx.t;

	if (g_nx.cp_runner >= 0) async_runner_stop(g_nx.cp_runner);
	if (g_nx.pd_runner >= 0) async_runner_stop(g_nx.pd_runner);
	if (g_nx.cp_ctx) osdp_cp_teardown(g_nx.cp_ctx);
	if (g_nx.pd_ctx) osdp_pd_teardown(g_nx.pd_ctx);
	pthread_mutex_destroy(&g_nx.lock);
	memset(&g_nx, 0, sizeof(g_nx));
	g_nx.cp_runner = g_nx.pd_runner = -1;
	g_nx.t = t;
}

static bool test_pd_online_and_sc_notifications(void)
{
	printf(SUB_2 "PD command callback sees PD_STATUS online and SC_STATUS active\n");

	/* Both notifications should already have fired during the
	 * setup_env handshake — setup blocks until the link is up. */
	if (g_nx.pd_pd_status.count == 0) {
		printf(SUB_2 "no PD_STATUS notification on PD side\n");
		return false;
	}
	if (g_nx.pd_pd_status.last_arg0 != 1) {
		printf(SUB_2 "PD_STATUS arg0=%d, want 1 (online)\n",
		       g_nx.pd_pd_status.last_arg0);
		return false;
	}
	if (g_nx.pd_sc_status.count == 0) {
		printf(SUB_2 "no SC_STATUS notification on PD side\n");
		return false;
	}
	if (g_nx.pd_sc_status.last_arg0 != 1) {
		printf(SUB_2 "SC_STATUS arg0=%d, want 1 (active)\n",
		       g_nx.pd_sc_status.last_arg0);
		return false;
	}
	return true;
}

static bool test_pd_offline_on_cp_silence(void)
{
	int deadline_ms;

	printf(SUB_2 "PD transitions offline when CP stops talking\n");

	/* Reset PD_STATUS record so we only observe the offline edge. */
	pthread_mutex_lock(&g_nx.lock);
	memset(&g_nx.pd_pd_status, 0, sizeof(g_nx.pd_pd_status));
	pthread_mutex_unlock(&g_nx.lock);

	/* Silence the CP side. PD should trip the 1.5s timeout (the test
	 * build overrides OSDP_PD_ONLINE_TOUT_MS) and post an offline
	 * notification through its command callback. */
	async_runner_stop(g_nx.cp_runner);
	g_nx.cp_runner = -1;

	/* 1500ms timeout + a 2500ms window for the PD refresh to run
	 * and the workqueue to schedule. */
	deadline_ms = 4000;
	if (!wait_for_notif(&g_nx.pd_pd_status, deadline_ms)) {
		printf(SUB_2 "no offline notification within %dms\n",
		       deadline_ms);
		return false;
	}
	if (g_nx.pd_pd_status.last_arg0 != 0) {
		printf(SUB_2 "PD_STATUS arg0=%d, want 0 (offline)\n",
		       g_nx.pd_pd_status.last_arg0);
		return false;
	}
	return true;
}

static bool test_cp_file_tx_abort_on_disable(void)
{
	struct osdp_cmd cmd = {
		.id = OSDP_CMD_FILE_TX,
		.file_tx = { .id = 1, .flags = 0 },
	};
	struct osdp_file_ops send_ops = {
		.arg = &g_nx.sender,
		.open = notif_fops_open, .read = notif_fops_read,
		.write = notif_fops_write, .close = notif_fops_close,
	};
	struct osdp_file_ops recv_ops = {
		.arg = &g_nx.receiver,
		.open = notif_fops_open, .read = notif_fops_read,
		.write = notif_fops_write, .close = notif_fops_close,
	};
	int rc, size, offset;

	printf(SUB_2 "CP aborts in-flight transfer when PD is disabled\n");

	g_nx.sender.is_cp = true;
	g_nx.receiver.is_cp = false;

	if (setup_env() != 0) {
		printf(SUB_2 "file-tx: failed to setup env\n");
		return false;
	}
	if (notif_create_file()) {
		printf(SUB_2 "file-tx: failed to create source file\n");
		teardown_env();
		return false;
	}
	if (osdp_file_register_ops(g_nx.cp_ctx, 0, &send_ops) ||
	    osdp_file_register_ops(g_nx.pd_ctx, 0, &recv_ops)) {
		printf(SUB_2 "file-tx: register ops failed\n");
		goto err;
	}
	if (osdp_cp_submit_command(g_nx.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "file-tx: submit failed\n");
		goto err;
	}

	/* Wait until transfer is observably in progress before we yank it. */
	rc = 0;
	while (rc++ < 50) {
		if (osdp_get_file_tx_status(g_nx.cp_ctx, 0, &size, &offset) == 0 &&
		    offset > 0)
			break;
		usleep(20 * 1000);
	}
	if (rc >= 50) {
		printf(SUB_2 "file-tx: never observed tx in progress\n");
		goto err;
	}

	/* Disabling the PD drives OSDP_CP_STATE_DISABLED, which must now
	 * call osdp_file_tx_abort() and fire FILE_TX_DONE/ABORTED. */
	pthread_mutex_lock(&g_nx.lock);
	memset(&g_nx.cp_file_tx_done, 0, sizeof(g_nx.cp_file_tx_done));
	pthread_mutex_unlock(&g_nx.lock);

	if (osdp_cp_disable_pd(g_nx.cp_ctx, 0)) {
		printf(SUB_2 "file-tx: disable_pd failed\n");
		goto err;
	}

	if (!wait_for_notif(&g_nx.cp_file_tx_done, 3000)) {
		printf(SUB_2 "file-tx: FILE_TX_DONE not seen after disable\n");
		goto err;
	}
	if (g_nx.cp_file_tx_done.last_arg1 != OSDP_FILE_TX_OUTCOME_ABORTED) {
		printf(SUB_2 "file-tx: outcome=%d, want ABORTED=%d\n",
		       g_nx.cp_file_tx_done.last_arg1,
		       OSDP_FILE_TX_OUTCOME_ABORTED);
		goto err;
	}
	if (osdp_get_file_tx_status(g_nx.cp_ctx, 0, &size, &offset) != -1) {
		printf(SUB_2 "file-tx: CP still reports transfer active\n");
		goto err;
	}
	teardown_env();
	unlink(NOTIF_FILE_SEND);
	unlink(NOTIF_FILE_RECV);
	return true;
err:
	teardown_env();
	unlink(NOTIF_FILE_SEND);
	unlink(NOTIF_FILE_RECV);
	return false;
}

static bool test_pd_file_tx_abort_on_offline(void)
{
	struct osdp_cmd cmd = {
		.id = OSDP_CMD_FILE_TX,
		.file_tx = { .id = 1, .flags = 0 },
	};
	struct osdp_file_ops send_ops = {
		.arg = &g_nx.sender,
		.open = notif_fops_open, .read = notif_fops_read,
		.write = notif_fops_write, .close = notif_fops_close,
	};
	struct osdp_file_ops recv_ops = {
		.arg = &g_nx.receiver,
		.open = notif_fops_open, .read = notif_fops_read,
		.write = notif_fops_write, .close = notif_fops_close,
	};
	int rc, size, offset, elapsed;

	printf(SUB_2 "PD aborts in-flight transfer when CP goes silent\n");

	g_nx.sender.is_cp = true;
	g_nx.receiver.is_cp = false;

	if (setup_env() != 0) {
		printf(SUB_2 "pd-file-tx: failed to setup env\n");
		return false;
	}
	if (notif_create_file()) {
		printf(SUB_2 "pd-file-tx: create file failed\n");
		teardown_env();
		return false;
	}
	if (osdp_file_register_ops(g_nx.cp_ctx, 0, &send_ops) ||
	    osdp_file_register_ops(g_nx.pd_ctx, 0, &recv_ops)) {
		printf(SUB_2 "pd-file-tx: register ops failed\n");
		goto err;
	}
	if (osdp_cp_submit_command(g_nx.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "pd-file-tx: submit failed\n");
		goto err;
	}

	/* Wait until the PD has actually started receiving. */
	rc = 0;
	while (rc++ < 50) {
		if (osdp_get_file_tx_status(g_nx.pd_ctx, 0, &size, &offset) == 0 &&
		    offset > 0)
			break;
		usleep(20 * 1000);
	}
	if (rc >= 50) {
		printf(SUB_2 "pd-file-tx: never observed PD-side rx\n");
		goto err;
	}

	async_runner_stop(g_nx.cp_runner);
	g_nx.cp_runner = -1;

	/* Wait for the PD to trip its online timeout; it should then call
	 * osdp_file_tx_abort() on its own transfer. */
	elapsed = 0;
	while (elapsed < 5000) {
		if (osdp_get_file_tx_status(g_nx.pd_ctx, 0, &size, &offset) == -1)
			break;
		usleep(50 * 1000);
		elapsed += 50;
	}
	if (osdp_get_file_tx_status(g_nx.pd_ctx, 0, &size, &offset) != -1) {
		printf(SUB_2 "pd-file-tx: PD still reports transfer active\n");
		goto err;
	}
	teardown_env();
	unlink(NOTIF_FILE_SEND);
	unlink(NOTIF_FILE_RECV);
	return true;
err:
	teardown_env();
	unlink(NOTIF_FILE_SEND);
	unlink(NOTIF_FILE_RECV);
	return false;
}

void run_notification_tests(struct test *t)
{
	bool ok = true;

	printf("\nBegin Notification Tests\n");

	g_nx.t = t;
	g_nx.cp_runner = g_nx.pd_runner = -1;

	/* Group A: PD-side command-callback notifications */
	if (setup_env() != 0) {
		printf(SUB_1 "Failed to setup test environment\n");
		TEST_REPORT(t, false);
		return;
	}
	ok &= test_pd_online_and_sc_notifications();
	ok &= test_pd_offline_on_cp_silence();
	teardown_env();

	/* Group B: file_tx abort on offline (separate envs so each test
	 * starts from a freshly-online link) */
	ok &= test_cp_file_tx_abort_on_disable();
	ok &= test_pd_file_tx_abort_on_offline();

	printf(SUB_1 "Notification tests %s\n", ok ? "succeeded" : "failed");
	TEST_REPORT(t, ok);
}
