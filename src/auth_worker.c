#include "auth_worker.h"
#include <stdlib.h>
#include <string.h>
void auth_job_free(AuthJob *job) {
    if (job) { sodium_memzero(job, sizeof *job); free(job); }
}
static void *run(void *argument) {
    AuthWorker *worker = argument;
    for (;;) {
        pthread_mutex_lock(&worker->mutex);
        while (!worker->waiting_count && !worker->stopping)
            pthread_cond_wait(&worker->condition, &worker->mutex);
        if (worker->stopping) { pthread_mutex_unlock(&worker->mutex); break; }
        AuthJob *job = worker->waiting[0];
        memmove(worker->waiting, worker->waiting + 1, --worker->waiting_count * sizeof *worker->waiting);
        pthread_mutex_unlock(&worker->mutex);
        if (job->operation == AUTH_REGISTER) {
            job->ok = crypto_pwhash_str_alg(job->hash, job->password, strlen(job->password),
                crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE, crypto_pwhash_ALG_ARGON2ID13) == 0;
        } else {
            job->ok = crypto_pwhash_str_verify(job->hash, job->password, strlen(job->password)) == 0;
            if (job->ok && job->operation == AUTH_PASSWORD)
                job->ok = crypto_pwhash_str_alg(job->hash, job->new_password, strlen(job->new_password),
                    crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE, crypto_pwhash_ALG_ARGON2ID13) == 0;
        }
        sodium_memzero(job->password, sizeof job->password);
        sodium_memzero(job->new_password, sizeof job->new_password);
        pthread_mutex_lock(&worker->mutex);
        worker->done[worker->done_count++] = job;
        pthread_mutex_unlock(&worker->mutex);
    }
    return NULL;
}
int auth_worker_start(AuthWorker *worker) {
    memset(worker, 0, sizeof *worker);
    if (sodium_init() < 0 || pthread_mutex_init(&worker->mutex, NULL)) return -1;
    if (pthread_cond_init(&worker->condition, NULL)) { pthread_mutex_destroy(&worker->mutex); return -1; }
    if (pthread_create(&worker->thread, NULL, run, worker)) {
        pthread_cond_destroy(&worker->condition); pthread_mutex_destroy(&worker->mutex); return -1;
    }
    return 0;
}
int auth_worker_submit(AuthWorker *worker, AuthJob *job) {
    pthread_mutex_lock(&worker->mutex);
    int full = worker->outstanding == AUTH_CAP;
    if (!full) {
        worker->waiting[worker->waiting_count++] = job;
        worker->outstanding++;
        pthread_cond_signal(&worker->condition);
    }
    pthread_mutex_unlock(&worker->mutex);
    return full ? -1 : 0;
}
AuthJob *auth_worker_collect(AuthWorker *worker) {
    pthread_mutex_lock(&worker->mutex);
    AuthJob *job = NULL;
    if (worker->done_count) {
        job = worker->done[0];
        memmove(worker->done, worker->done + 1, --worker->done_count * sizeof *worker->done);
        worker->outstanding--;
    }
    pthread_mutex_unlock(&worker->mutex);
    return job;
}
void auth_worker_stop(AuthWorker *worker) {
    pthread_mutex_lock(&worker->mutex);
    worker->stopping = 1;
    pthread_cond_signal(&worker->condition);
    pthread_mutex_unlock(&worker->mutex);
    pthread_join(worker->thread, NULL);
    for (size_t i = 0; i < worker->waiting_count; i++) auth_job_free(worker->waiting[i]);
    for (size_t i = 0; i < worker->done_count; i++) auth_job_free(worker->done[i]);
    pthread_cond_destroy(&worker->condition);
    pthread_mutex_destroy(&worker->mutex);
}
