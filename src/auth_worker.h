#ifndef AUTH_WORKER_H
#define AUTH_WORKER_H
#include <pthread.h>
#include <stdint.h>
#include <sodium.h>
#define AUTH_CAP 16
enum AuthOperation { AUTH_REGISTER, AUTH_LOGIN, AUTH_PASSWORD };
typedef struct {
    enum AuthOperation operation;
    int slot, ok;
    uint64_t generation;
    long long user_id;
    char request_id[129], username[33];
    char password[129], new_password[129], hash[crypto_pwhash_STRBYTES];
} AuthJob;
typedef struct {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    AuthJob *waiting[AUTH_CAP], *done[AUTH_CAP];
    size_t waiting_count, done_count, outstanding;
    int stopping;
} AuthWorker;
int auth_worker_start(AuthWorker *worker);
int auth_worker_submit(AuthWorker *worker, AuthJob *job);
AuthJob *auth_worker_collect(AuthWorker *worker);
void auth_worker_stop(AuthWorker *worker);
void auth_job_free(AuthJob *job);
#endif
